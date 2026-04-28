#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* recvmmsg/sendmmsg batch size — packets harvested per syscall.
 * 64 is a common sweet spot: large enough to amortize the syscall cost across
 * many packets, small enough that bufs[] (~94 KB) fits in L2 and the batch
 * doesn't add meaningful tail latency under bursty load. */
#define BATCH 64

typedef struct thread_arg_t {
    int tid;
    int port;
} thread_arg_t;

static void *worker_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    int           port = targ->port;
    int           tid  = targ->tid;

    /* Identical socket setup to v3 — only the recv/send strategy changes in v4. */
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

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        close(fd);
        return NULL;
    }

    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl");
        close(epfd);
        close(fd);
        return NULL;
    }

    printf("thread %d: listening on UDP port %d (non-blocking, epoll, recvmmsg/sendmmsg, batch=%d)\n", tid, port,
           BATCH);

    /* Per-thread batch state — stack allocated, sized at compile time.
     *   bufs[i]  — receive buffer for packet i
     *   iovs[i]  — points iovec at bufs[i]; length is patched after recv (see hot loop)
     *   srcs[i]  — sender address for packet i; recvmmsg fills one per packet
     *   msgs[i]  — wires it all together for recvmmsg/sendmmsg */
    char               bufs[BATCH][MAX_PKG_SIZE];
    struct iovec       iovs[BATCH];
    struct sockaddr_in srcs[BATCH];
    struct mmsghdr     msgs[BATCH];

    /* One-time wiring: iovec → buf, mmsghdr → iovec + sockaddr.
     * Inside the hot loop we only patch msg_namelen (re-arm) and iov_len (post-recv). */
    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < BATCH; i++) {
        iovs[i].iov_base           = bufs[i];
        iovs[i].iov_len            = MAX_PKG_SIZE;
        msgs[i].msg_hdr.msg_name   = &srcs[i];
        msgs[i].msg_hdr.msg_iov    = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    struct epoll_event events[MAX_EVENTS];
    uint64_t           pkg_cnt = 0;

    while (1) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        /* Drain the socket in batches until EAGAIN. */
        while (1) {
            /* Re-arm msg_namelen — recvmmsg writes the actual addr length back into it,
             * so without this the next call would see a too-small buffer. iov_len is
             * also patched below to the received size; restore it to the full capacity
             * here so the next recv has room for a full MTU packet. */
            for (int i = 0; i < BATCH; i++) {
                msgs[i].msg_hdr.msg_namelen = sizeof(srcs[i]);
                iovs[i].iov_len             = MAX_PKG_SIZE;
            }

            int r = recvmmsg(fd, msgs, BATCH, MSG_DONTWAIT, NULL);
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* drained */
                if (errno == EINTR) continue;
                perror("recvmmsg");
                goto done;
            }

            /* sendmmsg sends iov_len bytes per packet — patch it to the actually-received
             * length (msg_len), otherwise we'd echo the full 1472-byte buffer regardless
             * of payload size. Easy gotcha. */
            for (int i = 0; i < r; i++) iovs[i].iov_len = msgs[i].msg_len;

            int s = sendmmsg(fd, msgs, r, 0);
            if (s < 0) {
                /* tx ring full — drop the whole batch, same policy as v3's per-packet drop */
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                if (errno == EINTR) continue;
                perror("sendmmsg");
                goto done;
            }
            /* sendmmsg may return s < r — trailing packets are dropped. */

            uint64_t prev_milestone = pkg_cnt / 50000;
            pkg_cnt += (uint64_t)s;
            if (pkg_cnt / 50000 > prev_milestone)
                printf("thread %d: echoed %llu packets\n", tid, (unsigned long long)pkg_cnt);
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
