#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t percentile(uint64_t *sorted, size_t n, double p) {
    size_t idx = (size_t)(p / 100.0 * n);
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port> [count] [size]\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port         = atoi(argv[2]);
    size_t count     = (argc > 3) ? atoi(argv[3]) : 10000;
    size_t psize     = (argc > 4) ? atoi(argv[4]) : 64;

    if (psize < 8) psize = 8;
    if (psize > 1472) psize = 1472;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt");
        close(fd);
        return 1;
    }

    struct sockaddr_in server = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    if (inet_pton(AF_INET, host, &server.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        close(fd);
        return 1;
    }

    uint64_t *latencies = malloc(count * sizeof(uint64_t));
    if (!latencies) {
        perror("malloc");
        close(fd);
        return 1;
    }

    char send_buf[1472];
    char recv_buf[1472];
    memset(send_buf, 0xAB, psize);

    printf("Sending %zu packets of %zu bytes to %s:%d...\n", count, psize, host, port);

    size_t received = 0;
    size_t timeout  = 0;
    uint64_t start  = now_ns();

    for (size_t i = 0; i < count; i++) {
        uint64_t t0 = now_ns();
        memcpy(send_buf, &t0, sizeof(t0));

        ssize_t s = sendto(fd, send_buf, psize, 0, (struct sockaddr *)&server, sizeof(server));
        if (s < 0) {
            perror("sendto");
            break;
        }

        ssize_t r = recvfrom(fd, recv_buf, sizeof(recv_buf), 0, NULL, NULL);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                timeout++;
                continue;
            }
            perror("recvfrom");
            break;
        }

        uint64_t t1           = now_ns();
        latencies[received++] = t1 - t0;
    }

    uint64_t total_ns = now_ns() - start;

    if (received == 0) {
        printf("No packets received. Is the server running?\n");
        free(latencies);
        close(fd);
        return 1;
    }
    qsort(latencies, received, sizeof(uint64_t), cmp_u64);

    uint64_t sum = 0;
    for (size_t i = 0; i < received; i++)
        sum += latencies[i];

    printf("\n--- Results (%s v1: blocking recvfrom) ---\n", host);
    printf("Packets sent:     %zu\n", count);
    printf("Packets received: %zu\n", received);
    printf("Timed out:        %zu\n", timeout);
    printf("Packet size:      %zu bytes\n", psize);
    printf("Total time:       %.3f s\n", total_ns / 1e9);
    printf("Throughput:       %.0f pps\n", received / (total_ns / 1e9));
    printf("\nRound-trip latency (µs):\n");
    printf("  min   : %.2f\n", latencies[0] / 1000.0);
    printf("  p50   : %.2f\n", percentile(latencies, received, 50) / 1000.0);
    printf("  p90   : %.2f\n", percentile(latencies, received, 90) / 1000.0);
    printf("  p99   : %.2f\n", percentile(latencies, received, 99) / 1000.0);
    printf("  p99.9 : %.2f\n", percentile(latencies, received, 99.9) / 1000.0);
    printf("  max   : %.2f\n", latencies[received - 1] / 1000.0);
    printf("  avg   : %.2f\n", (double)sum / received / 1000);

    free(latencies);
    close(fd);
    return 0;
}
