# v5 — `iou_mt` walkthrough

UDP echo server that replaces v4's `epoll` + `recvmmsg`/`sendmmsg` with a per-thread `io_uring` ring using **`SQPOLL`** (kernel-side submission poller), **multishot `recvmsg`** (one SQE produces many recv completions), and **provided buffer rings** (kernel pulls recv buffers from a pool the app registers). Net effect on the hot path: zero syscalls, zero per-packet SQE bookkeeping for recv.

## Requirements

- **Linux kernel ≥ 6.0** — UDP multishot `recvmsg` landed in 6.0.
- **liburing ≥ 2.5** — for `io_uring_setup_buf_ring` / `io_uring_recvmsg_validate` helpers (Ubuntu 24.04+ ships this; the bundled `Dockerfile` uses 26.04).
- **`seccomp=unconfined`** in Docker — the default seccomp profile blocks `io_uring_setup`, which surfaces as `EPERM` on `io_uring_queue_init_params`. The provided `compose.yaml` already sets this; running with bare `docker run` will not work without it.

## 1. The whole picture, one frame

```
                          UDP datagrams arrive on port 9000
                          ────────────────────────────────────
                                         │
                          ┌──────────────┴──────────────┐
                          │   kernel UDP layer          │
                          │   SO_REUSEPORT flow hash    │   ← spreads flows
                          │   (BPF or default 4-tuple)  │     across N sockets
                          └──┬──┬──┬──┬──┬──┬──┬──┬─────┘
                             │  │  │  │  │  │  │  │
                          ┌──┘  │  │  │  │  │  │  └──┐
                          │     │  │  │  │  │  │     │
   ┌──────────────────────┴─┐  ...  ...  ...  ...  ┌─┴──────────────────────┐
   │  worker thread 0       │                      │  worker thread 7       │
   │  ────────────────      │                      │  ────────────────      │
   │                        │                      │                        │
   │   socket fd  (bound)   │                      │   socket fd  (bound)   │
   │       │                │                      │       │                │
   │       ▼                │                      │       ▼                │
   │   ┌─────────────┐      │                      │   ┌─────────────┐      │
   │   │  io_uring   │      │                      │   │  io_uring   │      │
   │   │             │      │                      │   │             │      │
   │   │  SQ ring  ◄─┼──┐   │                      │   │  SQ ring  ◄─┼──┐   │
   │   │  CQ ring  ──┼──┼─► │                      │   │  CQ ring  ──┼──┼─► │
   │   │  buf_ring   │  │   │                      │   │  buf_ring   │  │   │
   │   └──────┬──────┘  │   │                      │   └──────┬──────┘  │   │
   │          │         │   │                      │          │         │   │
   │   ┌──────┴──────┐  │   │                      │   ┌──────┴──────┐  │   │
   │   │ buf_base[]  │  │   │                      │   │ buf_base[]  │  │   │
   │   │ 1024×2048 B │  │   │                      │   │ 1024×2048 B │  │   │
   │   │ (mmap'd)    │  │   │                      │   │ (mmap'd)    │  │   │
   │   └─────────────┘  │   │                      │   └─────────────┘  │   │
   │                    │   │                      │                    │   │
   │       SQPOLL  ─────┘   │                      │       SQPOLL  ─────┘   │
   │       kthread (kernel) │                      │       kthread (kernel) │
   └────────────────────────┘                      └────────────────────────┘
```

Nothing crosses thread boundaries. Same shape as v3/v4 — only the box marked "io_uring" replaces v4's `epoll` + `recvmmsg`/`sendmmsg`.

## 2. Three rings, three jobs

io_uring isn't one ring — it's three, each with a different producer/consumer split.

```
                    producer       consumer       what flows
                    ────────       ────────       ──────────
   SQ  (submission)    app          kernel        SQEs (work to do)
   CQ  (completion)    kernel       app           CQEs (work that finished)
   buf_ring (provided) app          kernel        free buffers (recv pool)
```

You write SQEs, the kernel writes CQEs, and the buf_ring is a _separate_ ring of free recv buffers the kernel pulls from when a packet arrives. The buf_ring is what makes multishot worth it: the kernel doesn't need an SQE per recv, it just grabs a free buffer.

## 3. What setup costs (per thread)

```
  socket(AF_INET, SOCK_DGRAM)                             ── same as v3/v4
  setsockopt SO_REUSEADDR / SO_REUSEPORT / SO_RCVBUF=8MB
  bind :9000

  io_uring_queue_init_params(RING_ENTRIES=4096,           ── creates SQ/CQ
                             SETUP_SQPOLL,                   spawns SQPOLL kthread
                             sq_thread_idle=2000ms)

  mmap(1024 × 2048 B, PRIVATE|ANON|POPULATE)              ── recv buffer pool
                                                             (~2 MB per thread)

  io_uring_setup_buf_ring(BUF_GROUP=0, 1024 entries)      ── allocates+registers
                                                             buf_ring for that pool
  for i in 0..1023:                                       ── seed: hand all 1024
      buf_ring_add(slot[i], bid=i)                           buffers to the kernel
  buf_ring_advance(1024)

  arm_multishot_recv()                                    ── ONE SQE armed forever
  io_uring_submit()                                          (re-armed only when
                                                              kernel terminates it)
```

After this, **the hot loop never touches the SQ for recv**. One multishot SQE drives every packet.

## 4. Buffer-ring layout (the part that's easy to get wrong)

`buf_base` is one contiguous mmap. `buf_ring` is a separate small ring of `(addr, len, bid)` triples that _point_ into it.

```
              ┌──────────┬──────────┬──────────┬─────┐
   buf_base   │  slot 0  │  slot 1  │  slot 2  │ ... │   ← contiguous mmap,
   (mmap)     │ (2048 B) │ (2048 B) │ (2048 B) │     │     1024 × 2048 B
              └────┬─────┴────┬─────┴────┬─────┴─────┘
                   │          │          │
                   ▼          ▼          ▼
              ┌──────────┬──────────┬──────────┬─────┐
   buf_ring   │ addr=&s0 │ addr=&s1 │ addr=&s2 │ ... │   ← kernel pops from head
   (1024      │ len=2048 │ len=2048 │ len=2048 │     │     app pushes at tail
    entries)  │ bid=0    │ bid=1    │ bid=2    │     │
              └──────────┴──────────┴──────────┴─────┘
```

When a packet arrives:

1. Kernel pops one entry → it now knows where to write the packet.
2. Kernel writes a CQE that includes the **bid** (in the high bits of `cqe->flags`) and `IORING_CQE_F_BUFFER`.
3. App owns that bid until it pushes it back via `buf_ring_add` + `buf_ring_advance`.

The `bid` is the only handle — given `bid`, the app computes the address with `buf_base + bid * 2048`.

## 5. Inside one buffer (multishot recvmsg layout)

A 2048-byte slot doesn't store just the payload. The kernel writes a small struct at the front, then the sender's address, then the payload.

```
   one buf slot (2048 B)
   ┌─────────────────────────────────────────────────────────────┐
   │ io_uring_recvmsg_out  │ name (sockaddr_in) │  payload  │... │
   │     (16 B header)     │     (16 B)         │  ≤ 1472 B │    │
   └───────────┬───────────┴──────────┬─────────┴─────┬─────┴────┘
               │                      │               │
       namelen, controllen,      sender addr     echo payload
       payloadlen, flags         (ip+port)       (we send this back)

   helpers (defined in liburing.h):
     io_uring_recvmsg_validate(buf, res, &proto)   → struct *o or NULL
     io_uring_recvmsg_name(o)                      → ptr to sockaddr_in
     io_uring_recvmsg_payload(o, &proto)           → ptr to payload
     io_uring_recvmsg_payload_length(o, res, &proto) → payload bytes
```

Critical detail: **these pointers are valid only until you recycle the bid.** That's why the send must use them and the recycle must happen _after_ the send CQE.

## 6. The hot path — one full round trip

```
  ┌──────────────── time ────────────────►

  T0   userspace blocks in io_uring_wait_cqe()

  T1   packet arrives at NIC → kernel UDP → SO_REUSEPORT → thread N's socket
       │
       │   SQPOLL kthread (already polling) sees a multishot recv ready
       │   pulls bid=37 from buf_ring head
       │   writes packet into buf_base[37 * 2048]
       │   posts CQE: { user_data=0, res=1472, flags=F_MORE|F_BUFFER|(37<<16) }
       │
  T2   wait_cqe returns → app drains CQ:
            ud=0 (UD_RECV), bid=37, res=1472 ✓
            o = recvmsg_validate(buf+37, 1472, &proto)
            name    = recvmsg_name(o)        ── points inside slot 37
            payload = recvmsg_payload(o, ...) ── points inside slot 37
            paylen  = recvmsg_payload_length(o, ...)

            send_iovs[37] = { .base=payload, .len=paylen }
            send_msgs[37] = { .name=name, .iov=&send_iovs[37], ... }

            sqe = io_uring_get_sqe(&ring)
            io_uring_prep_sendmsg(sqe, fd, &send_msgs[37], 0)
            sqe->user_data = UD_SEND_BASE + 37   ── encodes bid in user_data

       (after for_each_cqe loop)
       io_uring_cq_advance(drained)
       io_uring_submit()    ── tail bump; SQPOLL picks it up without syscall

  T3   SQPOLL kthread sees new SQE in SQ
       executes sendmsg(fd, &send_msgs[37], 0)
       reads name + payload directly from slot 37  ← still owned by us
       writes CQE: { user_data=38, res=1472 }     ── 38 = UD_SEND_BASE + 37

  T4   next wait_cqe() returns → app sees ud=38:
            bid = 38 - 1 = 37
            buf_ring_add(slot 37, bid=37)
            buf_ring_advance(1)                   ── slot 37 back in pool

  T5   block again on wait_cqe()
```

**Two key invariants:**

1. **Slot 37 is borrowed from the kernel between T2 and T4.** The kernel must not overwrite it. That's enforced by _not putting bid 37 back in buf_ring until T4_.
2. **Userspace never makes a syscall on this path** — provided SQPOLL is hot. `wait_cqe` is just a memory read when CQEs are already available; it only enters the kernel when the CQ is empty.

## 7. user_data — tagging completions without a side table

```
   ud  = 0           → recv multishot CQE
   ud >= 1           → send CQE; bid = ud - 1

   bid space:  0 .. 1023   →  ud send space: 1 .. 1024
   ud=0 reserved for the one armed multishot recv
```

Why not use a side table indexed by SQE? Because with multishot, _one_ SQE produces many CQEs — there's no 1:1 between SQEs and CQEs. user_data is the only thing that survives.

## 8. Buffer-ownership state machine

Each `bid` is in exactly one of these states:

```
                      ┌─ owned by kernel ─┐         ┌─ owned by app ─┐
                      │                   │         │                │
                      ▼                   ▼         ▼                ▼
                ┌──────────┐  recv     ┌─────────────────┐
                │ in buf_  │ consumes  │   bid in CQE    │
        seed ──►│ ring     ├──────────►│ F_BUFFER set    │
                │ (free)   │           │                 │
                └──────────┘           └────────┬────────┘
                      ▲                         │
                      │                         │ build send msghdr,
                      │                         │ submit send SQE
                      │                         ▼
                      │                ┌─────────────────┐
                      │                │  send pending   │ ◄── kernel reads
                      │ buf_ring_add   │  (slot pinned)  │     name/payload
                      │ buf_ring_      │                 │     from this slot
                      │ advance        └────────┬────────┘
                      │                         │
                      │                         │ send CQE arrives
                      │                         ▼
                      │                ┌─────────────────┐
                      └────────────────│  recyclable     │
                                       └─────────────────┘
```

Drop paths (validate failure, SQ full, send error) collapse "send pending" → "recyclable" immediately — same destination, just no kernel work in between.

## 9. SQPOLL — what changes vs v4

```
v4 hot loop:
  while (1) {
      epoll_wait()      ── syscall
      while drain:
          recvmmsg(64)  ── syscall
          patch iov_len
          sendmmsg(64)  ── syscall
  }
  → 3 syscalls per 64 packets


v5 hot loop:
  while (1) {
      wait_cqe()                    ── only enters kernel when CQ is empty
      for each cqe:
          if recv: build send msghdr
                   push SQE         (memory write — no syscall)
          else:    recycle bid
      cq_advance()
      submit()                      ── memory write; wakes parked SQPOLL
  }                                    kthread only when it had parked
  → 0 syscalls per 64 packets while SQPOLL is hot
```

The SQPOLL kthread spins on the SQ tail for `sq_thread_idle` ms (we set 2000) after the last activity. While it's spinning, every `submit()` is just a memory write — no `io_uring_enter` syscall. If it parks, the next `submit()` does cost one syscall to wake it.

The bet: under load, SQPOLL stays hot, and the syscall count on the hot path drops to zero. The cost is a kernel thread per ring, which is why this would be wasteful at low pps.

## 10. Why the syscall delta isn't the whole story

What's still in the path even with v5:

```
   packet → NIC → IRQ → softirq → IP layer → UDP layer → socket queue
                                                              │
                                                              ▼
                                                      io_uring recv
                                                              │
                                                              ▼
                                                          buf_ring
                                                              │
                                                              ▼
                                                            CQE
   (echo path traverses the full stack again on the way out)
```

Every packet still allocates an `sk_buff`, runs IP/UDP parse, sits in the socket receive queue, and traverses the tx side symmetrically. That's what v6 (XDP/AF_XDP) finally removes — eBPF intercepts the packet at the driver, redirects into UMEM, and the `sk_buff` is never created.

So v5's win is real but bounded:

- On loopback (no NIC, no driver), the syscall savings is most of what you can save → modest delta over v4.
- On a real NIC at high pps, syscalls and kernel-userspace transitions cost more relative to packet processing → bigger delta.
- Either way, the kernel stack itself is the next bottleneck, and that's v6's problem.
