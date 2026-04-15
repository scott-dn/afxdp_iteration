# UDP Server Performance Iteration

A step-by-step progression of UDP echo server implementations in C, each removing one specific bottleneck. The goal is to demonstrate measurable performance improvement at every layer — from a naive blocking server to kernel-bypass via AF_XDP.

| Version | Name             | Implementation & Benefit                                                                                                                                                                                  | Bottleneck / Problem                                                                       |
| ------- | ---------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| `v1`    | `blocking_st`    | Single thread, `recvfrom`/`sendto` loop. Simplest possible baseline                                                                                                                                       | Single core; one syscall per packet; thread stalls inside kernel                           |
| `v2`    | `blocking_mt`    | N threads + `SO_REUSEPORT`, each runs the v1 loop. Kernel distributes packets; throughput scales with cores                                                                                               | Each thread stalls on `recvfrom`; OS thread overhead (~1MB stack, context switches)        |
| `v3`    | `nonblocking_mt` | N threads + `SO_REUSEPORT`, each with a private `epoll_wait(timeout=-1)` + `O_NONBLOCK` socket. Threads no longer trapped in `recvfrom` — `epoll_wait` multiplexes across fds (sockets, timers, signals). | One syscall per packet per thread — kernel/userspace transition dominates at high pps      |
| `v4`    | `mmsg_mt`        | v3 with `recvmmsg`/`sendmmsg` — harvests N packets per syscall. Syscall cost amortized across the batch                                                                                                   | Full kernel stack per packet — `sk_buff` alloc, IP/UDP parse, socket layer still traversed |
| `v5`    | `iou_mt`         | N threads, each owns an `io_uring` ring. `SQPOLL` kernel thread drains submissions — zero syscalls on hot path                                                                                            | Kernel stack still traversed; `sk_buff` still allocated per packet                         |
| `v6`    | `xdp`            | eBPF XDP program redirects packets into UMEM before `sk_buff` allocation. `AF_XDP` ring per NIC RX queue — kernel stack bypassed, near-zero-copy                                                          | XDP-capable NIC required; eBPF + UMEM ring setup complexity                                |

## Iteration

Each version changes exactly one variable so the benchmark delta is attributable to a single cause. All versions are measured with the same `benchmark` binary.

- **v1 → v2** — add cores. `SO_REUSEPORT` lets N threads each own a socket on the same port. Removes the single-core ceiling; threads still block.
- **v2 → v3** — eliminate blocking + fd multiplexing. Replace blocking `recvfrom` with `epoll_wait` + `O_NONBLOCK`. Threads are no longer trapped in a single-fd syscall — `epoll_wait` can wake on any registered fd. Per-packet syscall cost remains. This is the event-loop model behind Node.js, Netty, and Tokio.
- **v3 → v4** — batch syscalls. `recvmmsg`/`sendmmsg` handle N packets per syscall. Bottleneck shifts to kernel stack traversal. `mmsg_st` skipped — batching gain can be inferred by running v4 with one thread.
- **v4 → v5** — async I/O rings. `io_uring` replaces `epoll` + `recvmmsg` with submission/completion rings; `SQPOLL` eliminates syscalls on the hot path. Delta over v4 may be modest; the value is demonstrating ring-based I/O. Requires kernel 6.0+ for UDP multishot.
- **v5 → v6** — bypass the kernel stack. eBPF XDP program intercepts packets before `sk_buff` allocation, redirects into UMEM. IP/UDP parsing and socket layer eliminated entirely.
