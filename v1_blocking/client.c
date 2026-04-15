#include <errno.h>
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

#include "../utils.h"

int main(int argc, char *argv[]) {
    /* argc is the argument count including the program name (argv[0]);
     * < 3 means only the program name + at most one arg was given — missing host or port.
     * fprintf to stderr so the usage message doesn't pollute stdout (e.g. in pipelines).
     * argv[0] is the program name as invoked, so the usage line reflects the actual binary name. */
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port> [count] [size]\n", argv[0]);
        return 1;
    }

    const char *host  = argv[1];
    int         port  = atoi(argv[2]);
    size_t      count = (argc > 3) ? atoi(argv[3]) : 10000; /* number of packets to send */
    size_t      psize = (argc > 4) ? atoi(argv[4]) : 64;    /* payload size in bytes */

    /* clamp psize: minimum 8 bytes to fit the uint64_t timestamp header;
     * maximum 1472 bytes to stay within one Ethernet frame (MTU 1500 - IP 20 - UDP 8) */
    if (psize < 8) psize = 8;
    if (psize > 1472) psize = 1472;

    /* SOCK_DGRAM = UDP; 0 = default protocol for this socket type (IPPROTO_UDP).
     * socket() returns a file descriptor — a small integer the kernel uses to identify
     * this socket; all subsequent send/recv calls reference it by this fd.
     * < 0 on failure (e.g. too many open files, no kernel resources). */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    /* SO_RCVTIMEO: set a receive timeout so recvfrom() doesn't block forever
     * if the server is down or drops a packet.
     * timeval takes seconds + microseconds separately; {2, 0} = 2 seconds.
     * after timeout, recvfrom() returns -1 with errno=EAGAIN/EWOULDBLOCK
     * rather than blocking indefinitely — handled in the loop below. */
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt");
        close(fd);
        return 1;
    }

    /* Build the destination address struct for sendto().
     * sin_addr is left unset here — filled in by inet_pton() below.
     * All other fields (sin_zero padding) are zero-initialized by the designated initializer. */
    struct sockaddr_in server = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    /* inet_pton: "presentation to network" — converts a dotted-decimal string ("127.0.0.1")
     * into the binary representation stored in server.sin_addr.
     * returns 1 on success, 0 if the string is not a valid address, -1 on error.
     * <= 0 catches both invalid input and system error. */
    if (inet_pton(AF_INET, host, &server.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        close(fd);
        return 1;
    }

    /* Allocate the full latency sample array upfront — one slot per packet.
     * Storing every sample (rather than updating a running stat) allows exact
     * percentile calculation via qsort later, at the cost of O(n) memory.
     * With count=10000: 10000 * 8 = ~78 KB; with count=1M: ~7.6 MB. */
    uint64_t *latencies = malloc(count * sizeof(uint64_t));
    if (!latencies) {
        perror("malloc");
        close(fd);
        return 1;
    }

    char send_buf[1472]; /* outgoing packet buffer — sized to max UDP payload */
    char recv_buf[1472]; /* incoming packet buffer — sized to max UDP payload */
    /* fill send_buf with 0xAB pattern up to psize; the first 8 bytes will be
     * overwritten with the timestamp on each iteration, the rest is padding */
    memset(send_buf, 0xAB, psize);

    printf("Sending %zu packets of %zu bytes to %s:%d...\n", count, psize, host, port);

    size_t   received = 0;
    size_t   timeout  = 0;
    uint64_t start    = now_ns();

    for (size_t i = 0; i < count; i++) {
        uint64_t t0 = now_ns();            /* record send time before any syscall overhead */
        memcpy(send_buf, &t0, sizeof(t0)); /* stamp t0 into first 8 bytes of payload;
                                            * embedded so the server echoes it back — though
                                            * RTT is measured via t1-t0 locally, not from payload */

        /* sendto: send psize bytes from send_buf to server address.
         * 0 = no flags; (struct sockaddr*)&server = destination IP+port.
         * returns bytes sent on success; < 0 on error (e.g. network down, no route).
         * break instead of continue — a send failure is unrecoverable, no point retrying. */
        ssize_t s = sendto(fd, send_buf, psize, 0, (struct sockaddr *)&server, sizeof(server));
        if (s < 0) {
            perror("sendto");
            break;
        }

        /* blocks here until a packet arrives or the 2s SO_RCVTIMEO fires.
         * NULL src address — we accept responses from any sender (see bug note above).
         * sizeof(recv_buf) = 1472, large enough for any valid echo response. */
        ssize_t r = recvfrom(fd, recv_buf, sizeof(recv_buf), 0, NULL, NULL);
        if (r < 0) {
            /* EAGAIN/EWOULDBLOCK: the 2s timeout fired — server didn't respond.
             * count it and move on to the next packet rather than aborting. */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                timeout++;
                continue;
            }
            /* any other error (e.g. ECONNREFUSED, ENETDOWN) is unrecoverable */
            perror("recvfrom");
            break;
        }

        uint64_t t1 = now_ns(); /* record time immediately after recvfrom returns */
        latencies[received++] =
            t1 - t0; /* RTT in nanoseconds; received used as array index then incremented */
    }

    /* total wall time covers the entire run including timeouts */
    uint64_t total_ns = now_ns() - start;

    /* guard against all packets being lost — percentile/division below would be undefined */
    if (received == 0) {
        printf("No packets received. Is the server running?\n");
        free(latencies);
        close(fd);
        return 1;
    }

    /* sort latencies ascending so percentile() can index directly into the array */
    qsort(latencies, received, sizeof(uint64_t), cmp_u64);

    /* sum all samples for average calculation */
    uint64_t sum = 0;
    for (size_t i = 0; i < received; i++)
        sum += latencies[i];

    /* print summary — all latencies converted from ns to µs by dividing by 1000 */
    printf("\n--- Results (%s v1: blocking recvfrom) ---\n", host);
    printf("Packets sent:     %zu\n", count);
    printf("Packets received: %zu\n", received);
    printf("Timed out:        %zu\n", timeout);
    printf("Packet size:      %zu bytes\n", psize);
    printf("Total time:       %.3f s\n", total_ns / 1e9);
    printf("Throughput:       %.0f pps\n", received / (total_ns / 1e9));
    printf("\nRound-trip latency (µs):\n");
    printf("  min   : %.2f\n", latencies[0] / 1000.0); /* first element after sort */
    printf("  p50   : %.2f\n", percentile(latencies, received, 50) / 1000.0);
    printf("  p90   : %.2f\n", percentile(latencies, received, 90) / 1000.0);
    printf("  p99   : %.2f\n", percentile(latencies, received, 99) / 1000.0);
    printf("  p99.9 : %.2f\n", percentile(latencies, received, 99.9) / 1000.0);
    printf("  max   : %.2f\n", latencies[received - 1] / 1000.0); /* last element after sort */
    printf("  avg   : %.2f\n",
           (double)sum / received /
               1000); /* cast to double before division to avoid integer truncation */

    free(latencies);
    close(fd);
    return 0;
}
