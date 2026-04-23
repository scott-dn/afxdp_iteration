#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define MAX_PKG_SIZE 1472 /* mtu(1500) - ip(20) - udp(8) */
#define DEFAULT_PORT 9000
#define DEFAULT_THREADS 8

typedef struct thread_arg_t {
    int tid;
    int port;
} thread_arg_t;

static void *worker_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    int           port = targ->port;
    int           tid  = targ->tid;

    /* SOCK_DGRAM = UDP; 0 = default protocol for this socket type */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return NULL;
    }

    /* SO_REUSEADDR: allow immediate rebind after restart, avoiding "Address already in use"
     * SO_REUSEPORT: allow multiple sockets to bind the same port — kernel distributes
     *   incoming datagrams across them by hashing the 4-tuple (src IP, src port, dst IP, dst port).
     *   This is the only change from v1: each thread owns a socket, kernel does the balancing. */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(fd);
        return NULL;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEPORT");
        close(fd);
        return NULL;
    }

    /* enlarge the kernel receive buffer to absorb bursts before the thread drains them.
     * the kernel silently caps this at /proc/sys/net/core/rmem_max (typically 208KB by default).
     * to allow larger values: sysctl -w net.core.rmem_max=16777216 */
    int       rcvbuf = 8 * 1024 * 1024;
    socklen_t optlen = sizeof(rcvbuf);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, optlen) < 0) {
        perror("setsockopt SO_RCVBUF");
        close(fd);
        return NULL;
    }
    /* read back the actual value — kernel may have capped it at rmem_max */
    int actual_rcvbuf;
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual_rcvbuf, &optlen);
    printf("thread %d: rcvbuf requested %d bytes, kernel gave %d bytes\n", tid, rcvbuf, actual_rcvbuf);

    struct sockaddr_in host = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(fd, (struct sockaddr *)&host, sizeof(host)) < 0) {
        perror("bind");
        close(fd);
        return NULL;
    }

    printf("thread %d: listening on UDP port %d (blocking, multi-threaded)\n", tid, port);

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

        /* Print a status line every 50,000 packets so we can see it's alive
         * without flooding stdout (which itself would skew benchmarks). */
        if (pkg_cnt % 50000 == 0) printf("thread %d: echoed %llu packets\n", tid, (unsigned long long)pkg_cnt);
    }

    close(fd);
    return NULL;
}

int main(int argc, char *argv[]) {
    int port        = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    int num_threads = (argc > 2) ? atoi(argv[2]) : DEFAULT_THREADS;

    if (num_threads < 1 || num_threads > 256) {
        fprintf(stderr, "num_threads must be 1..256\n");
        return 1;
    }

    printf("Starting %d worker threads on port %d\n", num_threads, port);

    pthread_t    tids[num_threads];
    thread_arg_t args[num_threads];

    for (int i = 0; i < num_threads; i++) {
        args[i] = (thread_arg_t){.tid = i, .port = port};
        pthread_create(&tids[i], NULL, worker_thread, &args[i]);
    }

    for (int i = 0; i < num_threads; i++) pthread_join(tids[i], NULL);

    return 0;
}
