#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>

#define MAX_PKG_SIZE 1472 /* mtu(1500) - ip(20) - udp(8) */
#define DEFAULT_PORT 9000
#define DEFAULT_THREADS 8
#define MAX_EVENTS 1 /* only one fd registered per epoll instance */

typedef struct thread_arg_t {
    int tid;
    int port;
} thread_arg_t;

static void *worker_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    int           port = targ->port;
    int           tid  = targ->tid;

    /* SOCK_NONBLOCK: recvfrom/sendto return EAGAIN instead of blocking.
     * Pairs with epoll_wait so the thread blocks on the epoll fd, not the socket. */
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        perror("socket");
        return NULL;
    }

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

    int       rcvbuf = 8 * 1024 * 1024;
    socklen_t optlen = sizeof(rcvbuf);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, optlen) < 0) {
        perror("setsockopt SO_RCVBUF");
        close(fd);
        return NULL;
    }

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

    /* per-thread epoll instance — thread blocks on epoll_wait instead of recvfrom,
     * so it can be extended to multiplex other fds (timers, signals, control sockets). */
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        close(fd);
        return NULL;
    }

    /* level-triggered: epoll_wait returns while data is available.
     * combined with the drain loop below, behaves like edge-triggered for throughput. */
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl");
        close(epfd);
        close(fd);
        return NULL;
    }

    printf("thread %d: listening on UDP port %d (non-blocking, epoll)\n", tid, port);

    char               buf[MAX_PKG_SIZE];
    struct epoll_event events[MAX_EVENTS];
    uint64_t           pkg_cnt = 0;

    while (1) {
        /* timeout = -1: block until at least one fd is readable */
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        /* drain the socket until EAGAIN — amortizes epoll_wait over any backlog */
        while (1) {
            struct sockaddr_in src;
            socklen_t          srclen = sizeof(src);

            ssize_t r = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &srclen);
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                perror("recvfrom");
                goto done;
            }

            ssize_t s = sendto(fd, buf, r, 0, (struct sockaddr *)&src, srclen);
            if (s < 0) {
                /* send buffer full — drop this packet rather than queueing */
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                perror("sendto");
                goto done;
            }

            pkg_cnt++;
            if (pkg_cnt % 50000 == 0) printf("thread %d: echoed %llu packets\n", tid, (unsigned long long)pkg_cnt);
        }
    }

done:
    close(epfd);
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
