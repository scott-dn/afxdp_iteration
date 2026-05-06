#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <liburing.h>

#define MAX_PKG_SIZE 1472 /* mtu(1500) - ip(20) - udp(8) */
#define DEFAULT_PORT 9000
#define DEFAULT_THREADS 8

/* Provided buffer ring depth — number of recv buffers handed to the kernel.
 * Must be a power of two (kernel requirement for io_uring_buf_ring).
 * Each in-flight packet (recv'd, awaiting send CQE) ties up one entry, so this also
 * caps concurrent in-flight echoes per thread. 1024 is well above the steady-state
 * working set (~latency × pps); it just needs headroom for bursty arrivals. */
#define BUF_RING_ENTRIES 1024
#define BUF_RING_MASK (BUF_RING_ENTRIES - 1)

/* Per-buffer size. Multishot recvmsg lays out:
 *   [io_uring_recvmsg_out hdr][name (sockaddr_in)][control (0)][payload]
 * 2048 leaves comfortable headroom over hdr(~16) + name(16) + 1472. */
#define BUF_BYTES 2048

/* SQ/CQ depth — bounds in-flight ops (1 multishot recv + up to BUF_RING_ENTRIES sends). */
#define RING_ENTRIES 4096

/* user_data scheme on CQEs:
 *   0      → multishot recvmsg completion (one SQE, many CQEs)
 *   1..N   → send completion; encodes bid as (user_data - 1)
 * Distinguishing recv vs send by user_data avoids a side table. */
#define UD_RECV 0ULL
#define UD_SEND_BASE 1ULL

/* Buffer group ID for the provided buffer ring. Single group per thread is enough;
 * multiple groups would matter only if we had multiple buffer sizes. */
#define BUF_GROUP 0

/* SQPOLL idle timeout (ms). The kernel poller parks itself after this much idle time;
 * the next SQE submission costs an io_uring_enter wakeup. 2 s keeps the poller hot
 * across short lulls without burning a core forever when the server is truly idle. */
#define SQPOLL_IDLE_MS 2000

typedef struct thread_arg_t {
    int tid;
    int port;
} thread_arg_t;

/* Returns pointer to the BUF_BYTES-sized slot for buffer id `bid`. */
static inline unsigned char *buf_slot(unsigned char *base, int bid) {
    return base + (size_t)bid * BUF_BYTES;
}

/* Hand buffer `bid` back to the kernel via the provided buffer ring.
 * advance() publishes the entry to the kernel with the right memory barrier. */
static inline void recycle_buf(struct io_uring_buf_ring *br, unsigned char *base, int bid) {
    io_uring_buf_ring_add(br, buf_slot(base, bid), BUF_BYTES, (unsigned short)bid,
                          io_uring_buf_ring_mask(BUF_RING_ENTRIES), 0);
    io_uring_buf_ring_advance(br, 1);
}

/* Submit one multishot recvmsg SQE. The kernel keeps it armed and posts a CQE per
 * incoming packet (with CQE_F_MORE set) until something terminates it; on terminal
 * CQEs (F_MORE clear) we re-arm by calling this again. */
static int arm_multishot_recv(struct io_uring *ring, int fd, struct msghdr *proto) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -ENOSPC;
    io_uring_prep_recvmsg_multishot(sqe, fd, proto, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT; /* let kernel pick a buffer from BUF_GROUP */
    sqe->buf_group = BUF_GROUP;
    io_uring_sqe_set_data64(sqe, UD_RECV);
    return 0;
}

static void *worker_thread(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    int           port = targ->port;
    int           tid  = targ->tid;

    /* Identical socket setup to v3/v4 — only the I/O engine changes in v5. */
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

    /* Ring setup. SQPOLL puts a kernel thread on the SQ — userspace writes SQEs and the
     * kernel picks them up without io_uring_enter syscalls. The cost is a kernel thread
     * per ring; the value is zero-syscall hot path. */
    struct io_uring        ring;
    struct io_uring_params params = {0};
    params.flags                   = IORING_SETUP_SQPOLL;
    params.sq_thread_idle          = SQPOLL_IDLE_MS;
    int rc = io_uring_queue_init_params(RING_ENTRIES, &ring, &params);
    if (rc < 0) {
        /* liburing returns negative errno directly — not the -1+errno convention. */
        fprintf(stderr, "thread %d: io_uring_queue_init_params: %s\n", tid, strerror(-rc));
        close(fd);
        return NULL;
    }

    /* Backing memory for the BUF_RING_ENTRIES recv buffers. Page-aligned via mmap so
     * the buffer ring registration is happy. MAP_POPULATE prefaults the pages — avoids
     * minor page faults when the kernel first writes recv data. */
    unsigned char *buf_base = mmap(NULL, (size_t)BUF_RING_ENTRIES * BUF_BYTES, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (buf_base == MAP_FAILED) {
        perror("mmap buf_base");
        io_uring_queue_exit(&ring);
        close(fd);
        return NULL;
    }

    /* Buffer ring (provided buffers, IORING_REGISTER_PBUF_RING). io_uring_setup_buf_ring
     * allocates+registers in one call. The kernel pulls a free buf from this ring for
     * each multishot recv; we push freed bids back after their send completes. */
    int                       br_ret = 0;
    struct io_uring_buf_ring *buf_ring =
        io_uring_setup_buf_ring(&ring, BUF_RING_ENTRIES, BUF_GROUP, 0, &br_ret);
    if (!buf_ring) {
        fprintf(stderr, "io_uring_setup_buf_ring: %s\n", strerror(-br_ret));
        munmap(buf_base, (size_t)BUF_RING_ENTRIES * BUF_BYTES);
        io_uring_queue_exit(&ring);
        close(fd);
        return NULL;
    }

    /* Seed the buffer ring with all BUF_RING_ENTRIES buffers. After this the kernel
     * owns them; we get them back via CQEs (with F_BUFFER + bid) and recycle on send. */
    io_uring_buf_ring_init(buf_ring);
    for (int i = 0; i < BUF_RING_ENTRIES; i++) {
        io_uring_buf_ring_add(buf_ring, buf_slot(buf_base, i), BUF_BYTES, (unsigned short)i,
                              io_uring_buf_ring_mask(BUF_RING_ENTRIES), i);
    }
    io_uring_buf_ring_advance(buf_ring, BUF_RING_ENTRIES);

    /* Per-bid send msghdr/iovec — built at send time pointing into the recv buffer.
     * Lifetime: the kernel reads these structs (plus the buffer they point at) until
     * the send CQE arrives, after which we recycle the bid. Indexed by bid so each
     * in-flight send has a stable slot. */
    struct iovec  send_iovs[BUF_RING_ENTRIES] = {0};
    struct msghdr send_msgs[BUF_RING_ENTRIES] = {0};

    /* Template for the multishot recvmsg. The kernel reads namelen/controllen out of
     * this to know how much of the buffer to reserve for name/ctrl; the rest is payload.
     * msg_iov is unused for multishot recv — the buffer is supplied via the buf_ring. */
    struct msghdr recv_proto = {0};
    recv_proto.msg_namelen   = sizeof(struct sockaddr_in);
    recv_proto.msg_controllen = 0;

    if (arm_multishot_recv(&ring, fd, &recv_proto) < 0) {
        fprintf(stderr, "thread %d: arm_multishot_recv failed\n", tid);
        goto cleanup;
    }
    io_uring_submit(&ring);

    printf("thread %d: listening on UDP port %d (io_uring SQPOLL, multishot recvmsg, "
           "buf_ring=%d, sqpoll_idle=%dms)\n",
           tid, port, BUF_RING_ENTRIES, SQPOLL_IDLE_MS);

    uint64_t pkg_cnt = 0;

    while (1) {
        struct io_uring_cqe *cqe;
        /* Block until at least one CQE is available. With SQPOLL this is the only place
         * we may transition into the kernel — and only when the CQ is empty. */
        int wret = io_uring_wait_cqe(&ring, &cqe);
        if (wret < 0) {
            if (wret == -EINTR) continue;
            fprintf(stderr, "io_uring_wait_cqe: %s\n", strerror(-wret));
            break;
        }

        /* Drain everything currently in the CQ in one pass — amortizes the wait. */
        unsigned head;
        unsigned drained        = 0;
        uint64_t prev_milestone = pkg_cnt / 50000;
        io_uring_for_each_cqe(&ring, head, cqe) {
            drained++;
            uint64_t ud  = io_uring_cqe_get_data64(cqe);
            int      res = cqe->res;
            uint32_t flg = cqe->flags;

            if (ud == UD_RECV) {
                /* Multishot recv CQE. If F_BUFFER is set, a buffer was consumed and we
                 * own bid until we recycle it. If F_MORE is clear, the kernel has
                 * terminated this multishot — re-arm with a fresh SQE. */
                int rearm = !(flg & IORING_CQE_F_MORE);

                if (flg & IORING_CQE_F_BUFFER) {
                    int            bid = flg >> IORING_CQE_BUFFER_SHIFT;
                    unsigned char *buf = buf_slot(buf_base, bid);

                    if (res < 0) {
                        /* Error path that still consumed a buffer — recycle, drop. */
                        recycle_buf(buf_ring, buf_base, bid);
                    } else {
                        struct io_uring_recvmsg_out *o = io_uring_recvmsg_validate(buf, res, &recv_proto);
                        if (!o) {
                            /* Truncated/malformed — drop and recycle. */
                            recycle_buf(buf_ring, buf_base, bid);
                        } else {
                            void  *name    = io_uring_recvmsg_name(o);
                            void  *payload = io_uring_recvmsg_payload(o, &recv_proto);
                            size_t paylen  = io_uring_recvmsg_payload_length(o, res, &recv_proto);

                            /* Build the send msghdr in this bid's slot. Pointers into
                             * the recv buffer stay valid until we recycle the bid (after
                             * the matching send CQE). */
                            send_iovs[bid].iov_base = payload;
                            send_iovs[bid].iov_len  = paylen;
                            send_msgs[bid]          = (struct msghdr){
                                         .msg_name    = name,
                                         .msg_namelen = sizeof(struct sockaddr_in),
                                         .msg_iov     = &send_iovs[bid],
                                         .msg_iovlen  = 1,
                            };

                            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                            if (!sqe) {
                                /* SQ full — drop, recycle. RING_ENTRIES is sized to make
                                 * this very unlikely; matches v3/v4 drop-on-tx-full policy. */
                                recycle_buf(buf_ring, buf_base, bid);
                            } else {
                                io_uring_prep_sendmsg(sqe, fd, &send_msgs[bid], 0);
                                io_uring_sqe_set_data64(sqe, UD_SEND_BASE + (uint64_t)bid);
                                pkg_cnt++;
                            }
                        }
                    }
                } else if (res < 0 && res != -ENOBUFS && res != -EINTR) {
                    fprintf(stderr, "thread %d: recv cqe err: %s\n", tid, strerror(-res));
                }

                if (rearm) {
                    if (arm_multishot_recv(&ring, fd, &recv_proto) < 0)
                        fprintf(stderr, "thread %d: re-arm failed\n", tid);
                }
            } else {
                /* Send CQE. Recycle the recv buffer regardless of success/failure —
                 * a failed send still releases ownership back to us. */
                int bid = (int)(ud - UD_SEND_BASE);
                if (res < 0 && res != -EAGAIN && res != -EINTR)
                    fprintf(stderr, "thread %d: send cqe err: %s\n", tid, strerror(-res));
                recycle_buf(buf_ring, buf_base, bid);
            }
        }
        io_uring_cq_advance(&ring, drained);

        /* Push any send SQEs we built this iteration. With SQPOLL the kernel poller
         * picks them up; submit only enters the kernel if the poller is parked. */
        io_uring_submit(&ring);

        if (pkg_cnt / 50000 > prev_milestone)
            printf("thread %d: echoed %llu packets\n", tid, (unsigned long long)pkg_cnt);
    }

cleanup:
    munmap(buf_base, (size_t)BUF_RING_ENTRIES * BUF_BYTES);
    io_uring_queue_exit(&ring);
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
