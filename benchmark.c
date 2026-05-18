/*
 * benchmark.c — multi-sender fire-and-forget UDP benchmark
 *
 * Thread model (per sender):
 *   1 sender thread   — rotates sendto across K sockets, blasting flat-out
 *   K receiver threads — each drains one socket independently, records RTT
 *
 * Why K sockets per sender (not just more senders):
 *   SO_REUSEPORT on the server hashes the 4-tuple to pick which bound socket
 *   gets the packet. With N senders × 1 socket the bench produces N unique
 *   4-tuples; if N < num_server_threads, some server threads never see traffic
 *   (and the rest get unevenly hashed). Multiplying flows decouples flow count
 *   from sender-thread count: we get N×K distinct flows without N×K sender
 *   threads chewing bench CPU.
 *
 * Total bench threads = N + N×K (e.g. N=4, K=4 → 20 threads).
 * All threads pin to one CPU via pin_to_nth_allowed_cpu so the scheduler can't
 * migrate them mid-loop — kills a major variance source on the measurement side.
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
 *   ./benchmark <host> <port> [duration_s] [size] [num_senders] [sockets_per_sender]
 *   ./benchmark 127.0.0.1 9000                          # defaults: 5s, 64B, 4 senders, K=1
 *   ./benchmark 127.0.0.1 9000 5 1472 4 4               # 5s, max-size, 4 senders × 4 socks
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
#define PKT_HDR 16            /* packet header: seq(8) + send_time_ns(8) */
#define MAX_SAMPLES 100000000 /* cap latencies at 100M samples (~800MB) */

/* -------------------------------------------------------------------------- */
/* Globals shared between threads                                             */
/* -------------------------------------------------------------------------- */
static _Atomic int        g_running;
static struct sockaddr_in g_host;
static size_t             g_psize;

static uint64_t      *g_latencies;
static _Atomic size_t g_lat_idx = 0;

/* -------------------------------------------------------------------------- */
/* Sender / receiver argument structs                                          */
/* -------------------------------------------------------------------------- */
/* Sender: one thread, K fds. Rotates sendto across the K fds in round-robin
 * order so each call drains to a different ephemeral src port, producing K
 * distinct 4-tuples per sender thread without spawning K sender threads. */
typedef struct sender_arg_t {
    int      sender_id; /* used as pin index */
    int      k;
    int     *fds; /* k fds, lifetime tied to main */
    uint64_t sent;
} sender_arg_t;

/* Receiver: one thread per socket. recv_id is a globally unique index across
 * all bench receivers (0..N*K) — used for pinning so each receiver lands on
 * a distinct CPU when the cpuset has room. */
typedef struct receiver_arg_t {
    int      recv_id; /* used as pin index */
    int      fd;
    uint64_t received;
    uint64_t stalls;
} receiver_arg_t;

/* -------------------------------------------------------------------------- */
/* Sender thread                                                              */
/* -------------------------------------------------------------------------- */
static void *sender_thread(void *arg) {
    sender_arg_t *sarg = arg;

    /* Pin: sender 0 → first allowed cpu, sender 1 → second, etc. */
    pin_to_nth_allowed_cpu(sarg->sender_id, sarg->sender_id);

    char     buf[MAX_PKG_SIZE];
    uint64_t seq = 0;
    int      i   = 0; /* round-robin index across fds */

    memset(buf, 0, g_psize);

    while (g_running) {
        uint64_t t0 = now_ns();

        memcpy(buf, &seq, 8);
        memcpy(buf + 8, &t0, 8);

        int     fd = sarg->fds[i];
        ssize_t s  = sendto(fd, buf, g_psize, 0, (struct sockaddr *)&g_host, sizeof(g_host));
        if (s > 0) seq++;
        else fprintf(stderr, "sendto failed (seq=%llu): %s\n", (unsigned long long)seq, strerror(errno));

        i++;
        if (i >= sarg->k) i = 0;
    }

    sarg->sent = seq;
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Receiver thread                                                             */
/* -------------------------------------------------------------------------- */
static void *receiver_thread(void *arg) {
    receiver_arg_t *rarg = arg;

    /* Pin: receivers are offset past the senders in the pin sequence so
     * receiver 0 doesn't collide with sender 0. The helper modulos against
     * the allowed cpu count, so the offset just shifts which cpu each lands on. */
    pin_to_nth_allowed_cpu(1000 + rarg->recv_id, rarg->recv_id);

    char     buf[MAX_PKG_SIZE];
    uint64_t received = 0;
    uint64_t stalls   = 0;

    while (1) {
        ssize_t r = recvfrom(rarg->fd, buf, sizeof(buf), 0, NULL, NULL);

        /* SO_RCVTIMEO fires as EAGAIN — once sender has stopped and buffer
         * is empty, exit. Mid-run timeouts (sender still running) count as stalls. */
        if (r < 0) {
            if (!g_running) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) stalls++;
            continue;
        }

        if (r < PKT_HDR) continue;

        uint64_t t1 = now_ns();

        uint64_t send_time;
        memcpy(&send_time, buf + 8, 8);

        if (send_time == 0 || send_time > t1) continue;

        received++;

        size_t idx = atomic_fetch_add(&g_lat_idx, 1);
        if (idx < MAX_SAMPLES) g_latencies[idx] = t1 - send_time;
    }

    rarg->received = received;
    rarg->stalls   = stalls;
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Main                                                                        */
/* -------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port> [duration_s] [size] [num_senders] [sockets_per_sender]\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int         port = atoi(argv[2]);
    int         dur  = (argc > 3) ? atoi(argv[3]) : 5;
    g_psize          = (argc > 4) ? (size_t)atoi(argv[4]) : 64;
    int num_senders  = (argc > 5) ? atoi(argv[5]) : 4;
    int k            = (argc > 6) ? atoi(argv[6]) : 1;

    if (g_psize < PKT_HDR) g_psize = PKT_HDR;
    if (g_psize > MAX_PKG_SIZE) g_psize = MAX_PKG_SIZE;
    if (num_senders < 1 || num_senders > 256) {
        fprintf(stderr, "num_senders must be 1..256\n");
        return 1;
    }
    if (k < 1 || k > 256) {
        fprintf(stderr, "sockets_per_sender must be 1..256\n");
        return 1;
    }

    g_host = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    if (inet_pton(AF_INET, host, &g_host.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        return 1;
    }

    int num_sockets = num_senders * k;

    /* One socket per (sender, fanout) pair — each gets a distinct ephemeral
     * src port → distinct 4-tuple → distinct SO_REUSEPORT hash → broader
     * coverage of server worker threads. */
    int           *all_fds = calloc(num_sockets, sizeof(int));
    struct timeval tv      = {.tv_sec = 0, .tv_usec = 100000};
    int            rcvbuf  = 4 * 1024 * 1024;
    for (int i = 0; i < num_sockets; i++) {
        all_fds[i] = socket(AF_INET, SOCK_DGRAM, 0);
        if (all_fds[i] < 0) {
            perror("socket");
            return 1;
        }
        setsockopt(all_fds[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(all_fds[i], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }

    g_latencies = malloc(MAX_SAMPLES * sizeof(uint64_t));
    if (!g_latencies) {
        perror("malloc");
        return 1;
    }

    printf("Benchmarking %s:%d for %ds, psize=%zu bytes, senders=%d, sockets/sender=%d (total flows=%d)...\n", host,
           port, dur, g_psize, num_senders, k, num_sockets);

    sender_arg_t   *senders   = calloc(num_senders, sizeof(sender_arg_t));
    receiver_arg_t *receivers = calloc(num_sockets, sizeof(receiver_arg_t));

    for (int s = 0; s < num_senders; s++) {
        senders[s].sender_id = s;
        senders[s].k         = k;
        senders[s].fds       = &all_fds[s * k];
    }
    for (int r = 0; r < num_sockets; r++) {
        receivers[r].recv_id = r;
        receivers[r].fd      = all_fds[r];
    }

    g_running        = 1;
    pthread_t *stids = calloc(num_senders, sizeof(pthread_t));
    pthread_t *rtids = calloc(num_sockets, sizeof(pthread_t));
    for (int r = 0; r < num_sockets; r++) pthread_create(&rtids[r], NULL, receiver_thread, &receivers[r]);
    for (int s = 0; s < num_senders; s++) pthread_create(&stids[s], NULL, sender_thread, &senders[s]);

    sleep(dur);
    g_running = 0;

    for (int s = 0; s < num_senders; s++) pthread_join(stids[s], NULL);
    for (int r = 0; r < num_sockets; r++) pthread_join(rtids[r], NULL);
    for (int i = 0; i < num_sockets; i++) close(all_fds[i]);

    uint64_t total_sent   = 0;
    uint64_t total_recv   = 0;
    uint64_t total_stalls = 0;
    for (int s = 0; s < num_senders; s++) total_sent += senders[s].sent;
    for (int r = 0; r < num_sockets; r++) {
        total_recv += receivers[r].received;
        total_stalls += receivers[r].stalls;
    }

    size_t n_lat = total_recv < MAX_SAMPLES ? (size_t)total_recv : MAX_SAMPLES;

    double drop_pct = total_sent > 0 ? 100.0 * (double)(total_sent - total_recv) / (double)total_sent : 0.0;

    printf("\n--- Results ---\n");
    printf("Duration:         %ds\n", dur);
    printf("Senders:          %d (sockets/sender=%d, total flows=%d)\n", num_senders, k, num_sockets);
    printf("Packet size:      %zu bytes\n", g_psize);
    printf("Packets sent:     %llu\n", (unsigned long long)total_sent);
    printf("Packets received: %llu\n", (unsigned long long)total_recv);
    printf("Dropped:          %llu (%.1f%%)\n", (unsigned long long)(total_sent - total_recv), drop_pct);
    printf("Mid-run stalls:   %llu (≥100ms idle windows)\n", (unsigned long long)total_stalls);
    printf("Throughput:       %.0f pps\n", (double)total_recv / dur);

    if (n_lat == 0) {
        printf("No latency samples collected.\n");
        free(g_latencies);
        free(all_fds);
        free(senders);
        free(receivers);
        free(stids);
        free(rtids);
        return 0;
    }

    qsort(g_latencies, n_lat, sizeof(uint64_t), cmp_u64);

    uint64_t sum = 0;
    for (size_t i = 0; i < n_lat; i++) sum += g_latencies[i];

    printf("\nRound-trip latency (µs) — %zu samples:\n", n_lat);
    printf("  min   : %.2f\n", (double)g_latencies[0] / 1000.0);
    printf("  p50   : %.2f (median)\n", (double)percentile(g_latencies, n_lat, 50) / 1000.0);
    printf("  p90   : %.2f\n", (double)percentile(g_latencies, n_lat, 90) / 1000.0);
    printf("  p99   : %.2f\n", (double)percentile(g_latencies, n_lat, 99) / 1000.0);
    printf("  p99.9 : %.2f\n", (double)percentile(g_latencies, n_lat, 99.9) / 1000.0);
    printf("  max   : %.2f\n", (double)g_latencies[n_lat - 1] / 1000.0);
    printf("  avg   : %.2f\n", (double)sum / (double)n_lat / 1000.0);

    free(g_latencies);
    free(all_fds);
    free(senders);
    free(receivers);
    free(stids);
    free(rtids);
    return 0;
}
