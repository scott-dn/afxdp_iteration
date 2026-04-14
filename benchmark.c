/*
 * benchmark.c — parallel fire-and-forget UDP benchmark
 *
 * Two threads: sender blasts packets as fast as possible,
 * receiver collects echoes and records RTT independently.
 * This decouples send rate from RTT, allowing the server to
 * be stressed with many packets in flight simultaneously.
 *
 * Packet layout (16-byte header + padding):
 *   [0..7]   uint64_t seq           — sequence number (for future use)
 *   [8..15]  uint64_t send_time_ns  — CLOCK_MONOTONIC at send time
 *   [16..N]  zero padding
 *
 * RTT is computed from the echoed send_time_ns in the received payload —
 * no shared state needed between sender and receiver threads.
 *
 * Build:
 *   gcc -O2 -D_GNU_SOURCE -lpthread -o benchmark benchmark.c
 *
 * Run:
 *   ./benchmark 127.0.0.1 9000
 *   ./benchmark 127.0.0.1 9000 5 64     # 5s duration, 64 byte packets
 */

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

#define PKT_HDR 16           /* packet header size: seq(8 bytes) + send_time_ns(8 bytes) */
#define MAX_SAMPLES 10000000 /* cap latency array at 10M samples (~80MB) to bound memory usage */

/* -------------------------------------------------------------------------- */
/* Globals shared between threads                                              */
/* -------------------------------------------------------------------------- */
/* _Atomic: stronger than volatile — guarantees atomic read/write and prevents
 * CPU reordering, not just compiler caching. correct for a flag shared between threads. */
static _Atomic int        g_running;
static int                g_fd;     /* shared socket — sendto, recvfrom */
static struct sockaddr_in g_server; /* destination address */
static size_t             g_psize;  /* packet size in bytes, immutable after thread launch */

/* _Atomic: both threads increment these concurrently without a mutex;
 * C11 atomics guarantee no torn reads/writes and prevent compiler reordering */
static _Atomic uint64_t g_sent     = 0; /* packets fired by sender thread */
static _Atomic uint64_t g_received = 0; /* echo responses collected by receiver thread */

static uint64_t      *g_latencies; /* heap array of RTT samples in nanoseconds */
static _Atomic size_t g_lat_n = 0; /* samples written so far; atomic — receiver increments */
static size_t         g_lat_max;   /* capacity of g_latencies */

/* -------------------------------------------------------------------------- */
/* Sender thread                                                               */
/* -------------------------------------------------------------------------- */
static void *sender_thread(void *arg) {
    (void)arg;
    char     buf[1472];
    uint64_t seq = 0;

    memset(buf, 0, g_psize);

    while (g_running) {
        uint64_t t0 = now_ns();

        /* write header into first 16 bytes of payload */
        memcpy(buf, &seq, 8);
        memcpy(buf + 8, &t0, 8);

        sendto(g_fd, buf, g_psize, 0, (struct sockaddr *)&g_server, sizeof(g_server));

        atomic_fetch_add(&g_sent, 1);
        seq++;
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Receiver thread                                                             */
/* -------------------------------------------------------------------------- */
static void *receiver_thread(void *arg) {
    (void)arg;
    char buf[1472];

    while (g_running) {
        ssize_t r = recvfrom(g_fd, buf, sizeof(buf), 0, NULL, NULL);

        /* SO_RCVTIMEO fires as EAGAIN — just loop back to check g_running */
        if (r < 0) continue;

        /* packet too short to contain a valid header */
        if (r < PKT_HDR) continue;

        uint64_t t1 = now_ns();

        /* read the send timestamp embedded in the echoed payload */
        uint64_t send_time;
        memcpy(&send_time, buf + 8, 8);

        /* sanity check: discard if echoed time looks wrong */
        if (send_time == 0 || send_time > t1) continue;

        atomic_fetch_add(&g_received, 1);

        /* store RTT sample if we still have room */
        size_t idx = atomic_fetch_add(&g_lat_n, 1);
        if (idx < g_lat_max) g_latencies[idx] = t1 - send_time;
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Main                                                                        */
/* -------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port> [duration_s] [size]\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int         port = atoi(argv[2]);
    int         dur  = (argc > 3) ? atoi(argv[3]) : 5;
    g_psize          = (argc > 4) ? (size_t)atoi(argv[4]) : 64;

    if (g_psize < PKT_HDR) g_psize = PKT_HDR;
    if (g_psize > 1472) g_psize = 1472;

    g_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_fd < 0) {
        perror("socket");
        return 1;
    }

    /* 100ms receive timeout so receiver thread exits promptly after g_running=0 */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
    setsockopt(g_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    g_server = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    if (inet_pton(AF_INET, host, &g_server.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        close(g_fd);
        return 1;
    }

    /* allocate latency sample array — cap at MAX_SAMPLES */
    g_lat_max   = MAX_SAMPLES;
    g_latencies = malloc(g_lat_max * sizeof(uint64_t));
    if (!g_latencies) {
        perror("malloc");
        close(g_fd);
        return 1;
    }

    printf("Benchmarking %s:%d for %ds, psize=%zu bytes...\n", host, port, dur, g_psize);

    g_running = 1;
    pthread_t stid, rtid;
    pthread_create(&rtid, NULL, receiver_thread, NULL);
    pthread_create(&stid, NULL, sender_thread, NULL);

    sleep(dur);
    g_running = 0;

    pthread_join(stid, NULL);
    pthread_join(rtid, NULL);
    close(g_fd);

    uint64_t total_sent = atomic_load(&g_sent);
    uint64_t total_recv = atomic_load(&g_received);
    size_t   n_lat      = atomic_load(&g_lat_n);
    if (n_lat > g_lat_max) n_lat = g_lat_max;

    double drop_pct =
        total_sent > 0 ? 100.0 * (double)(total_sent - total_recv) / (double)total_sent : 0.0;

    printf("\n--- Results ---\n");
    printf("Duration:         %ds\n", dur);
    printf("Packet size:      %zu bytes\n", g_psize);
    printf("Packets sent:     %lu\n", total_sent);
    printf("Packets received: %lu\n", total_recv);
    printf("Dropped:          %lu (%.1f%%)\n", total_sent - total_recv, drop_pct);
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
    printf("  min   : %.2f\n", g_latencies[0] / 1000.0);
    printf("  p50   : %.2f\n", percentile(g_latencies, n_lat, 50) / 1000.0);
    printf("  p90   : %.2f\n", percentile(g_latencies, n_lat, 90) / 1000.0);
    printf("  p99   : %.2f\n", percentile(g_latencies, n_lat, 99) / 1000.0);
    printf("  p99.9 : %.2f\n", percentile(g_latencies, n_lat, 99.9) / 1000.0);
    printf("  max   : %.2f\n", g_latencies[n_lat - 1] / 1000.0);
    printf("  avg   : %.2f\n", (double)sum / (double)n_lat / 1000.0);

    free(g_latencies);
    return 0;
}
