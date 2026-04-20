/*
 * benchmark.c — multi-sender fire-and-forget UDP benchmark
 *
 * Thread model (per sender):
 *   sender   — blasts packets as fast as possible on its own socket
 *   receiver — collects echoes from the same socket, records RTT independently
 *
 * Decoupling send from receive lets many packets be in flight simultaneously,
 * stressing the server without the client blocking on each echo.
 *
 * Multiple senders (default 4): each sender owns a distinct UDP socket, which
 * gets a different ephemeral source port from the OS. This produces distinct
 * 4-tuples (src IP, src port, dst IP, dst port), causing SO_REUSEPORT on the
 * server to hash them to different server threads — all threads get exercised.
 *
 * Packet layout (16-byte header + padding):
 *   [0..7]   uint64_t seq          — per-sender sequence number
 *   [8..15]  uint64_t send_time_ns — CLOCK_MONOTONIC at send time
 *   [16..N]  zero padding
 *
 * RTT = recv_time - send_time_ns (read from echoed payload, no shared state needed).
 *
 * Build:
 *   gcc -O2 -D_GNU_SOURCE -lpthread -o benchmark benchmark.c
 *
 * Run:
 *   ./benchmark <host> <port> [duration_s] [size] [num_senders]
 *   ./benchmark 127.0.0.1 9000                    # defaults: 5s, 64B, 4 senders
 *   ./benchmark 127.0.0.1 9000 10 1472 8          # 10s, max-size packets, 8 senders
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "utils.h"

#define MAX_PKG_SIZE 1472     /* mtu(1500) - ip(20) - udp(8) */
#define PKT_HDR 16            /* packet header size: seq(8 bytes) + send_time_ns(8 bytes) */
#define MAX_SAMPLES 100000000 /* cap latencies at 100M samples (~800MB) to bound memory usage */

/* -------------------------------------------------------------------------- */
/* Globals shared between threads                                             */
/* -------------------------------------------------------------------------- */
/* _Atomic: stronger than volatile — guarantees atomic read/write and prevents
 * CPU reordering, not just compiler caching. correct for a flag shared between threads. */
static _Atomic int        g_running;
static struct sockaddr_in g_host;  /* destination address */
static size_t             g_psize; /* packet size in bytes, immutable after thread launch */

static uint64_t      *g_latencies;   /* heap array of RTT samples in nanoseconds */
static _Atomic size_t g_lat_idx = 0; /* next free slot; atomic — multiple receivers claim indices */

/* Per-pair argument and result struct.
 * fd is written by main before thread launch (read-only to both threads).
 * sent/received are written by each thread after exit, read by main after join — no race. */
typedef struct {
    int      fd;       /* in:  socket owned by this sender/receiver pair */
    uint64_t sent;     /* out: total packets sent, filled by sender_thread */
    uint64_t received; /* out: total echoes received, filled by receiver_thread */
} worker_arg_t;

/* -------------------------------------------------------------------------- */
/* Sender thread                                                              */
/* -------------------------------------------------------------------------- */
static void *sender_thread(void *arg) {
    worker_arg_t *warg = arg;

    char     buf[MAX_PKG_SIZE];
    uint64_t seq = 0; /* seq doubles as sent count — incremented only on success */

    memset(buf, 0, g_psize); /* optional */

    while (g_running) {
        uint64_t t0 = now_ns();

        /* write header into first 16 bytes of payload */
        memcpy(buf, &seq, 8);
        memcpy(buf + 8, &t0, 8);

        ssize_t s = sendto(warg->fd, buf, g_psize, 0, (struct sockaddr *)&g_host, sizeof(g_host));
        if (s > 0) seq++;
        else fprintf(stderr, "sendto failed (seq=%llu): %s\n", (unsigned long long)seq, strerror(errno));
    }

    warg->sent = seq;

    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Receiver thread                                                             */
/* -------------------------------------------------------------------------- */
static void *receiver_thread(void *arg) {
    worker_arg_t *warg = arg;

    char     buf[MAX_PKG_SIZE];
    uint64_t received = 0;

    while (1) {
        ssize_t r = recvfrom(warg->fd, buf, sizeof(buf), 0, NULL, NULL);

        /* SO_RCVTIMEO fires as EAGAIN — once sender has stopped and buffer
         * is empty (timeout with no packets), exit the receiver loop */
        if (r < 0) {
            if (!g_running) break; /* sender done + buffer drained */
            continue;
        }

        /* packet too short to contain a valid header */
        if (r < PKT_HDR) continue;

        uint64_t t1 = now_ns();

        /* read the send timestamp embedded in the echoed payload */
        uint64_t send_time;
        memcpy(&send_time, buf + 8, 8);

        /* sanity check: discard if echoed time looks wrong */
        if (send_time == 0 || send_time > t1) continue;

        received++;

        /* store RTT sample if we still have room */
        size_t idx = atomic_fetch_add(&g_lat_idx, 1);
        if (idx < MAX_SAMPLES) g_latencies[idx] = t1 - send_time;
    }

    warg->received = received;

    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Main                                                                        */
/* -------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port> [duration_s] [size] [num_senders]\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int         port = atoi(argv[2]);
    int         dur  = (argc > 3) ? atoi(argv[3]) : 5;
    g_psize          = (argc > 4) ? (size_t)atoi(argv[4]) : 64;
    int num_senders  = (argc > 5) ? atoi(argv[5]) : 4;

    if (g_psize < PKT_HDR) g_psize = PKT_HDR;
    if (g_psize > MAX_PKG_SIZE) g_psize = MAX_PKG_SIZE;
    if (num_senders < 1) num_senders = 1;

    g_host = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    if (inet_pton(AF_INET, host, &g_host.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        return 1;
    }

    /* one socket per sender — distinct ephemeral src port each, so SO_REUSEPORT on the
     * server distributes packets across server threads by 4-tuple hash */
    worker_arg_t *workers = malloc((size_t)num_senders * sizeof(worker_arg_t));
    if (!workers) {
        perror("malloc");
        return 1;
    }
    struct timeval tv     = {.tv_sec = 0, .tv_usec = 100000}; /* 100ms recv timeout */
    int            rcvbuf = 4 * 1024 * 1024;
    for (int i = 0; i < num_senders; i++) {
        workers[i].fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (workers[i].fd < 0) {
            perror("socket");
            free(workers);
            return 1;
        }
        setsockopt(workers[i].fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(workers[i].fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }

    g_latencies = malloc(MAX_SAMPLES * sizeof(uint64_t));
    if (!g_latencies) {
        perror("malloc");
        free(workers);
        return 1;
    }

    printf("Benchmarking %s:%d for %ds, psize=%zu bytes, senders=%d...\n", host, port, dur, g_psize,
           num_senders);

    g_running        = 1;
    pthread_t *stids = malloc((size_t)num_senders * sizeof(pthread_t));
    pthread_t *rtids = malloc((size_t)num_senders * sizeof(pthread_t));
    if (!stids || !rtids) {
        perror("malloc");
        free(stids);
        free(rtids);
        free(g_latencies);
        free(workers);
        return 1;
    }
    for (int i = 0; i < num_senders; i++) {
        pthread_create(&rtids[i], NULL, receiver_thread, &workers[i]);
        pthread_create(&stids[i], NULL, sender_thread, &workers[i]);
    }

    sleep(dur);
    g_running = 0;

    for (int i = 0; i < num_senders; i++) {
        pthread_join(stids[i], NULL);
        pthread_join(rtids[i], NULL);
        close(workers[i].fd);
    }
    free(stids);
    free(rtids);

    uint64_t total_sent = 0;
    uint64_t total_recv = 0;
    for (int i = 0; i < num_senders; i++) {
        total_sent += workers[i].sent;
        total_recv += workers[i].received;
    }
    free(workers);

    size_t n_lat = total_recv < MAX_SAMPLES ? (size_t)total_recv : MAX_SAMPLES;

    double drop_pct =
        total_sent > 0 ? 100.0 * (double)(total_sent - total_recv) / (double)total_sent : 0.0;

    printf("\n--- Results ---\n");
    printf("Duration:         %ds\n", dur);
    printf("Senders:          %d\n", num_senders);
    printf("Packet size:      %zu bytes\n", g_psize);
    printf("Packets sent:     %llu\n", (unsigned long long)total_sent);
    printf("Packets received: %llu\n", (unsigned long long)total_recv);
    printf("Dropped:          %llu (%.1f%%)\n", (unsigned long long)(total_sent - total_recv), drop_pct);
    printf("Throughput:       %.0f pps\n", (double)total_recv / dur);

    if (n_lat == 0) {
        printf("No latency samples collected.\n");
        free(g_latencies);
        return 0;
    }

    qsort(g_latencies, n_lat, sizeof(uint64_t), cmp_u64);

    uint64_t sum = 0;
    for (size_t i = 0; i < n_lat; i++)
        sum += g_latencies[i];

    printf("\nRound-trip latency (µs) — %zu samples:\n", n_lat);
    printf("  min   : %.2f\n", (double)g_latencies[0] / 1000.0);
    printf("  p50   : %.2f\n", (double)percentile(g_latencies, n_lat, 50) / 1000.0);
    printf("  p90   : %.2f\n", (double)percentile(g_latencies, n_lat, 90) / 1000.0);
    printf("  p99   : %.2f\n", (double)percentile(g_latencies, n_lat, 99) / 1000.0);
    printf("  p99.9 : %.2f\n", (double)percentile(g_latencies, n_lat, 99.9) / 1000.0);
    printf("  max   : %.2f\n", (double)g_latencies[n_lat - 1] / 1000.0);
    printf("  avg   : %.2f\n", (double)sum / (double)n_lat / 1000.0);

    free(g_latencies);
    return 0;
}
