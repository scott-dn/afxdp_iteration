#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define MAX_PKG_SIZE 1472 /* mtu(1500) - ip(20) - udp(8) */
#define DEFAULT_PORT 9000

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;

    /* SOCK_DGRAM = UDP; 0 = default protocol for this socket type */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    /* SOL_SOCKET: option lives at the socket layer, not the protocol layer (IPPROTO_UDP etc.)
     * SO_REUSEADDR: allow immediate rebind after restart, avoiding "Address already in use"
     * &opt/sizeof(opt): setsockopt takes void* + length because options vary in type;
     *   here opt=1 (int) means enable */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return 1;
    }

    /* enlarge the kernel receive buffer to absorb bursts before the server drains them.
     * the kernel silently caps this at /proc/sys/net/core/rmem_max (typically 208KB by default).
     * to allow larger values: sysctl -w net.core.rmem_max=16777216 */
    int       rcvbuf = 4 * 1024 * 1024; /* request 4MB */
    socklen_t optlen = sizeof(rcvbuf);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, optlen) < 0) {
        perror("setsockopt SO_RCVBUF");
        close(fd);
        return 1;
    }
    /* read back the actual value — kernel may have capped it at rmem_max */
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &optlen);
    printf("rcvbuf: requested 4MB, kernel gave %d bytes (%.1f KB)\n", rcvbuf, rcvbuf / 1024.0);

    /* sockaddr_in: IPv4 socket address struct passed to bind()
     * .sin_family      = AF_INET     — address family: IPv4
     * .sin_port        = htons(port) — port in network byte order (big-endian)
     * .sin_addr.s_addr = INADDR_ANY  — listen on all available interfaces (0.0.0.0) */
    struct sockaddr_in host = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    /* bind() assigns the address (IP + port) to the socket
     * (struct sockaddr *)&host — cast required: bind() takes a generic sockaddr*,
     *   not sockaddr_in*; both have the same memory layout so the cast is safe
     * sizeof(host) — tells the kernel how many bytes to read from the pointer
     * < 0 — on failure: print the OS error, close the fd to avoid leaking it, exit */
    if (bind(fd, (struct sockaddr *)&host, sizeof(host)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    printf("Listening on UDP port %d (blocking, single-threaded)\n", port);

    char     buf[MAX_PKG_SIZE]; /* receive buffer — sized to the max UDP payload (1472 bytes) */
    uint64_t pkg_cnt = 0;
    while (1) {
        struct sockaddr_in src;                  /* sender address, filled by recvfrom() */
        socklen_t          srclen = sizeof(src); /* must be initialised each iteration;
                                                  * recvfrom() overwrites it with the actual length */

        /* Block until a UDP packet arrives. */
        ssize_t recv_bytes = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &srclen);
        if (recv_bytes < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }

        /* Echo the exact bytes back to whoever sent them. */
        ssize_t sent_bytes = sendto(fd, buf, recv_bytes, 0, (struct sockaddr *)&src, srclen);
        if (sent_bytes < 0) {
            perror("sendto");
            break;
        }

        pkg_cnt++;

        /* Print a status line every 20,000 packets so we can see it's alive
         * without flooding stdout (which itself would skew benchmarks). */
        if (pkg_cnt % 20000 == 0) {
            printf("Echoed %llu packets\n", (unsigned long long)pkg_cnt);
        }
    }

    close(fd);
}
