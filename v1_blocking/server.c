#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define MAX_PACKET_SIZE 1472 /* mtu(1500) - ip(20) - udp(8) */
#define DEFAUT_PORT 9000

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : DEFAUT_PORT;

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
        return 1;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    printf("Listening on UDP port %d (blocking, single-threaded)\n", port);

    char buf[MAX_PACKET_SIZE];
    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);

    uint64_t pkg_cnt = 0;
    while (1) {
        ssize_t recv = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &srclen);
        if (recv < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }

        ssize_t sent = sendto(fd, buf, recv, 0, (struct sockaddr *)&src, srclen);
        if (sent < 0) {
            perror("sendto");
            break;
        }

        pkg_cnt++;

        if (pkg_cnt % 10000 == 0) {
            printf("Echoed %lu packets\n", pkg_cnt);
        }
    }

    close(fd);
}
