# benchmark.c — the load generator

A multi-threaded **fire-and-forget UDP benchmark** that drives the v1–vN echo
servers in this repo. It blasts packets at line-rate, collects per-packet RTTs
from echoed payloads, and reports throughput + latency percentiles. The design
is deliberate: every choice (timestamp-in-payload, lock-free sample collection,
N×K socket fanout, per-thread CPU pinning) exists to make the measurement
**reproducible** rather than just fast.

## Run

```sh
# 1. Start a server in one shell (compose handles cpuset + caps + seccomp)
VERSION=v5_iou_mt docker compose up -d --build --wait

# 2. Run the bench in the bench profile — separate container, separate cgroup,
#    pinned to physically disjoint cores from the server.
docker compose run --rm bench \
    ./build/benchmark 127.0.0.1 9000 5 1472 4 4
#                     │         │    │ │    │ │
#                     │         │    │ │    │ └── sockets per sender (K)
#                     │         │    │ │    └──── number of senders (N)
#                     │         │    │ └───────── packet size in bytes
#                     │         │    └─────────── duration in seconds
#                     │         └──────────────── server UDP port
#                     └────────────────────────── server IP (loopback via shared netns)
```

CLI: `./build/benchmark <host> <port> [duration_s=5] [size=64] [num_senders=4] [sockets_per_sender=1]`

## CLI knobs in one table

| arg                      | default | what it controls                                                   |
| ------------------------ | ------- | ------------------------------------------------------------------ |
| `host`                   | —       | server IP (loopback or LAN address)                                |
| `port`                   | —       | server UDP port                                                    |
| `duration_s`             | 5       | wall-clock seconds the senders run                                 |
| `size`                   | 64      | total packet bytes (first 16 = header, rest = zero-padded payload) |
| `num_senders` (N)        | 4       | how many sender _threads_                                          |
| `sockets_per_sender` (K) | 1       | how many sockets each sender rotates across (fanout multiplier)    |

Total sockets in flight: **N × K**. Total bench threads: **N + N×K + 1 main**.

## 1. The whole picture, one frame

```
                                bench process
   ┌──────────────────────────────────────────────────────────────────────────┐
   │                                                                          │
   │   main thread:                                                           │
   │     • parse argv → host, port, dur, size, N, K                           │
   │     • allocate all_fds[N*K]                                              │
   │     • create sockets, set SO_RCVTIMEO=100ms, SO_RCVBUF=4MB               │
   │     • spawn N senders + N*K receivers                                    │
   │     • sleep(dur); set g_running = 0                                      │
   │     • join, free, print results                                          │
   │                                                                          │
   │   ┌─────────────────────┐         ┌──────────────────────────────────┐   │
   │   │  sender threads (N) │         │  receiver threads (N*K)          │   │
   │   │                     │         │                                  │   │
   │   │  s=0  ──► fds[0..K) │         │   r=0   ──► fd = all_fds[0]      │   │
   │   │  s=1  ──► fds[K..2K)│         │   r=1   ──► fd = all_fds[1]      │   │
   │   │  s=2  ──► fds[2K..) │         │   ...                            │   │
   │   │  ...                │         │   r=N*K-1 ─► fd = all_fds[N*K-1] │   │
   │   │                     │         │                                  │   │
   │   │  each rotates       │         │   each drains ONE socket,        │   │
   │   │  sendto across K    │         │   computes RTT from payload,     │   │
   │   │  fds round-robin    │         │   appends to g_latencies[]       │   │
   │   └──────────┬──────────┘         └────────────────┬─────────────────┘   │
   │              │                                     ▲                     │
   │              │ sendto(fd, [seq, t0, padding])      │ recvfrom(fd, ...)   │
   │              ▼                                     │ RTT = now - t0      │
   │       ┌────────────────────────────────────────────┴────────────┐        │
   │       │              N*K UDP sockets in this process            │        │
   │       └────────────────────────────────────────────┬────────────┘        │
   └────────────────────────────────────────────────────┼─────────────────────┘
                                                        │
                                                kernel UDP loopback
                                                        │
   ┌────────────────────────────────────────────────────┼─────────────────────┐
   │   server process (v1..v5)                          │                     │
   │      port 9000, N_server worker threads,           │                     │
   │      SO_REUSEPORT distributes packets across       │                     │
   │      workers based on a hash of the 4-tuple        │                     │
   └──────────────────────────────────────────────────────────────────────────┘
```

## 2. The load pattern — fire-and-forget, not ping-pong

A naive RTT benchmark sends one packet, waits for the echo, computes the RTT,
sends the next. That pattern measures **single-flight latency** but caps
throughput at `1 / RTT`. With a 100 µs RTT, that's 10k pps no matter how fast
the server actually is.

Fire-and-forget separates the two:

```
   ping-pong (NOT what we do):
       send → wait → recv → send → wait → recv → ...
       throughput ≤ 1 / RTT

   fire-and-forget (what we do):
       sender_thread:    send send send send send send send ...
       receiver_thread:  ... recv recv recv recv recv recv ...
       throughput limited by server, not by RTT
```

Sender and receiver run on different threads, possibly on different CPUs. The
sender never reads. The receiver never writes. Each packet carries its own
send timestamp in the payload, so the receiver can compute RTT **without any
shared state with the sender** — no atomic-load of "when did we send seq N",
no map lookup, no synchronization.

## 3. Packet layout

```
   16-byte header + zero padding to `size` bytes
   ┌──────────────────────┬──────────────────────┬────────────────────────┐
   │  seq (uint64_t LE)   │  send_time_ns        │  payload (zero-filled) │
   │  bytes 0..7          │  bytes 8..15         │  bytes 16..size-1      │
   └──────────────────────┴──────────────────────┴────────────────────────┘

   sender writes:
       memcpy(buf,     &seq, 8);     // sequence number, per-sender, not used
                                     //  in measurements but useful when
                                     //  inspecting captured pcap.
       memcpy(buf + 8, &t0,  8);     // CLOCK_MONOTONIC nanoseconds at send

   receiver reads:
       memcpy(&send_time, buf + 8, 8);
       rtt = now_ns() - send_time;
```

The server is a verbatim echo — it returns whatever bytes it received, in
order. The timestamp survives the round-trip unchanged, so the receiver
computes RTT directly. **No clock synchronization between sender and receiver
is required** because both timestamps (`t0` at sender, `now_ns()` at receiver)
come from the same `CLOCK_MONOTONIC` in the **same process**.

Sanity check on validity:

```c
if (send_time == 0 || send_time > t1) continue;
```

- `== 0`: payload byte 8..15 was zero — not a packet from us, or corruption.
- `> t1`: would imply a negative RTT — impossible, skip rather than poison
  the histogram.

## 4. Why N × K sockets — server worker coverage

The server uses `SO_REUSEPORT` to load-balance among its worker threads.
Which worker gets a packet is decided by hashing the **4-tuple**
(`src_ip`, `src_port`, `dst_ip`, `dst_port`). For loopback against one server
port, only `src_port` varies — and `src_port` is the **ephemeral port** the
kernel picked when the bench's socket was created.

```
                            one socket per src_port
                            ────────────────────────

                         src_port=52001       src_port=52002       ...
                              │                    │
                              ▼                    ▼
                          ┌───────────────────────────────────────┐
                          │  hash(src_port, ...) % num_workers    │
                          └──┬────┬────┬────┬────┬────┬────┬────┬─┘
                             │    │    │    │    │    │    │    │
                            w0   w1   w2   w3   w4   w5   w6   w7
```

With **N senders × 1 socket each**, only N distinct src_ports → at most N
distinct hash inputs. If N < num_server_workers, several workers see zero
traffic. Even if N == num_server_workers, the hash function isn't guaranteed
to map them onto a permutation — some workers might get two flows, others
zero.

With **N senders × K sockets each**, we get **N × K** distinct src_ports —
many more hash inputs than workers, so the hash distribution evens out and
every worker gets roughly fair load:

```
   N=4, K=4 → 16 distinct flows
                  ▼
   ┌────────────────────────────────────────────────────────┐
   │ hash() % 8                                             │
   └─┬───┬───┬───┬───┬───┬───┬───┬─────────────────────────┘
     │   │   │   │   │   │   │   │
     2   2   2   2   2   2   2   2     ← every worker gets ~2 flows
     w0  w1  w2  w3  w4  w5  w6  w7
```

But we don't want N×K _sender threads_ — that's just extra context-switch
cost. So each sender thread rotates through K sockets round-robin and gets K
distinct src_ports out of N sender threads. The fanout is a property of the
socket count, not the thread count.

## 5. all_fds — one allocation, two views

```c
int *all_fds = calloc(num_sockets, sizeof(int));   // num_sockets = N * K
```

The trick: the **same fd array** is sliced two different ways for senders vs
receivers:

```
                          all_fds[0..N*K)
   ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
   │ fd0  │ fd1  │ fd2  │ fd3  │ fd4  │ fd5  │ fd6  │ fd7  │ fd8  │ fd9  │ ...
   └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
     │      │      │      │      │      │      │      │      │      │
     │  sender 0's slice (K=4)   │  sender 1's slice         │  sender 2's │
     │     ──────────────────    │   ────────────────        │  ─────────  │
     ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼
   senders[0].fds = &all_fds[0]   ── sender 0 rotates across [fd0..fd3]
                                  senders[1].fds = &all_fds[K]
                                  senders[2].fds = &all_fds[2K]
                                  ...

   receivers[r].fd = all_fds[r]   ── one receiver per fd, 1:1
```

`senders[s].fds = &all_fds[s * k]` is a **pointer slice** — same underlying
buffer, different start offset, no copy. The slice length is implicit
(`senders[s].k`); each sender only ever indexes `0..k-1`, so it stays inside
its slice.

Receivers don't slice — `receivers[r].fd = all_fds[r]` is just one int,
because each receiver owns exactly one socket.

This works because **each socket is shared between exactly one sender (writer)
and exactly one receiver (reader)**. UDP sockets are full-duplex — concurrent
send and recv on the same fd from different threads is safe (sender writes the
TX path, receiver reads the RX path, kernel keeps them separate).

## 6. The sender hot loop

```c
while (g_running) {
    uint64_t t0 = now_ns();
    memcpy(buf,     &seq, 8);
    memcpy(buf + 8, &t0,  8);

    int     fd = sarg->fds[i];
    ssize_t s  = sendto(fd, buf, g_psize, 0, &g_host, sizeof(g_host));
    if (s > 0) seq++;

    i++; if (i >= sarg->k) i = 0;     // round-robin across K sockets
}
```

Properties:

- **No `recvfrom`** anywhere — the sender is write-only.
- **No backpressure**. A failed `sendto` increments an error counter but
  doesn't slow the loop. UDP loopback rarely fails outright; what gets dropped
  shows up later as "sent but not received" in the stats.
- **Per-sender sequence numbers**, not global. seq doesn't matter for
  measurement; it's a debugging aid (pcap inspection, lost-packet investigation).
- **Round-robin across K fds**: simple counter, no atomic, no shuffling. The
  sender thread is the only writer to `i`, so no synchronization needed.

## 7. The receiver hot loop

```c
while (1) {
    ssize_t r = recvfrom(rarg->fd, buf, sizeof(buf), 0, NULL, NULL);

    if (r < 0) {
        if (!g_running) break;                            // shutdown
        if (errno == EAGAIN || errno == EWOULDBLOCK) stalls++;
        continue;
    }
    if (r < PKT_HDR) continue;

    uint64_t t1 = now_ns();
    uint64_t send_time;
    memcpy(&send_time, buf + 8, 8);
    if (send_time == 0 || send_time > t1) continue;

    received++;
    size_t idx = atomic_fetch_add(&g_lat_idx, 1);
    if (idx < MAX_SAMPLES) g_latencies[idx] = t1 - send_time;
}
```

Three things to notice:

1. **`SO_RCVTIMEO = 100 ms`** on the socket. `recvfrom` returns `EAGAIN`
   instead of blocking forever. This is how the receiver wakes up to check
   `g_running` after the sender stops — without it, `pthread_join` would hang.
2. **Mid-run `EAGAIN` = stall**. If the sender is still running but the
   server has gone silent for 100 ms, we count it. Useful as a "did the server
   wedge briefly?" signal that doesn't require parsing latencies.
3. **Lock-free sample append**. `atomic_fetch_add(&g_lat_idx, 1)` claims a
   slot; the receiver writes into `g_latencies[idx]`. Race-free because each
   receiver gets a _different_ index (atomic counter), and the array is
   pre-allocated and never reallocated.

## 8. How latency samples land in the histogram

```
   N*K receiver threads, each computing RTTs concurrently
   ─────────────────────────────────────────────────────────

        receiver 0          receiver 1          receiver 2          ...
            │                   │                   │
            │ atomic_fetch_add  │ atomic_fetch_add  │
            ▼                   ▼                   ▼
        idx=0               idx=1               idx=2
            │                   │                   │
            ▼                   ▼                   ▼
   ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬─ ... ─┬──────┐
   │  l0  │  l1  │  l2  │  l3  │  l4  │  l5  │  l6  │       │  ?   │  g_latencies[]
   └──────┴──────┴──────┴──────┴──────┴──────┴──────┴─ ... ─┴──────┘
   ◄────── claimed slots ─────►◄──────── unused ────────►
                                                       MAX_SAMPLES (100M)

   end of run:
       n_lat = min(total_recv, MAX_SAMPLES)
       qsort(g_latencies, n_lat, ...)
       compute min, p50, p90, p99, p99.9, max, avg
```

Why this works:

- **No mutex on the hot path.** `atomic_fetch_add` is a single atomic RMW
  instruction (`lock xadd` on x86, `LDADD` on ARMv8.1+) — much cheaper than a
  pthread mutex.
- **Each writer touches a distinct cache line region** (most of the time),
  because consecutive `idx` values get appended at consecutive array
  positions and receivers don't synchronize their throughput. There's still
  some cache-line contention on `g_lat_idx` itself, which is the bottleneck
  if you push receiver count very high.
- **Bounded memory**. `MAX_SAMPLES = 100 M` caps the latency buffer at
  ~800 MB. Beyond that, samples are dropped silently — but at 100 M samples
  you already have ridiculous statistical resolution for percentiles.
- **Sorted lazily**. We don't maintain order during the run; one big
  `qsort` at the end is faster than maintaining a sorted structure under
  concurrent inserts.

## 9. CPU pinning — `pin_to_nth_allowed_cpu`

Every bench thread pins itself to one CPU within the container's cpuset
(set to `8-15` by `compose.yaml` for the `bench` service — physically disjoint
from the server's cpuset `0-7`).

```c
pin_to_nth_allowed_cpu(sarg->sender_id, sarg->sender_id);   // sender s → cpu s
pin_to_nth_allowed_cpu(1000 + rarg->recv_id, rarg->recv_id); // receiver r → cpu r
```

The helper (in `utils.h`) does three things:

1. Reads the current affinity mask via `sched_getaffinity` — this is what
   Docker handed us via cpuset.
2. Finds the **nth set bit** in that mask (`n % CPU_COUNT(mask)`).
3. Calls `pthread_setaffinity_np` to clamp this thread to that one CPU.

```
   sched_getaffinity → mask = {8, 9, 10, 11, 12, 13, 14, 15}
                              ▲   ▲   ▲   ▲   ▲   ▲   ▲   ▲
   pin_to_nth_allowed_cpu(0, 0) ─┘  │  │  │  │  │  │  │
   pin_to_nth_allowed_cpu(1, 1) ────┘  │  │  │  │  │  │
   pin_to_nth_allowed_cpu(2, 2) ───────┘  │  │  │  │  │
   ...
```

Why pin? Without pinning:

- The scheduler can migrate threads mid-run → cold caches → variance.
- Receiver and sender may opportunistically share an SMT sibling → unusually
  fast RTT for a few microseconds → spiky throughput numbers.
- Run-to-run noise is **dominated** by scheduler luck, not by what you're
  trying to measure.

With pinning:

- Every packet takes the same path through the same caches between the same
  CPUs.
- Run-to-run variance collapses.
- p50 and p99 represent the server's actual behavior, not the OS's mood.

The "1000 +" on the receiver pin call doesn't change the chosen CPU — it's
just a log tag. The helper's first arg (`tid`) only appears in the log line
(`thread 1000+r: pinned to cpu X`); the **second arg** (`n`) is what picks the
CPU via `n % allowed_count`. So receiver `r` pins to `allowed[r % total]`,
same as if we passed `(recv_id, recv_id)`. The 1000+ exists so the log shows
at a glance whether a pin event is a sender or receiver. What actually
prevents sender/receiver from contending on the same CPU is having enough
CPUs in the cpuset to spread them out — with 4 senders + 16 receivers on an
8-CPU cpuset, they **will** share, and that's fine because the receivers are
mostly idle (blocked in `recvfrom`) while senders are CPU-bound.

## 10. Lifecycle — the orchestration

```
   main thread                          worker threads
   ───────────                          ──────────────

   parse argv
   allocate all_fds[N*K]
   create sockets, setsockopt × 3
   allocate g_latencies[100M]
   allocate senders[N], receivers[N*K]
   wire up senders[s].fds slices
   wire up receivers[r].fd direct refs

   g_running = 1
                                        ┌─── pthread_create N*K receivers ──►
                                        │    each pins, then loops on recvfrom
                                        │
                                        └─── pthread_create N senders ──────►
                                             each pins, then loops on sendto
   sleep(dur)                                                            │
                                                                         │
   g_running = 0     ───── observed by senders ────────────────────────►─┤
                                                                         │
                                        sender threads exit              │
                                                                         │
                                        receivers wake on EAGAIN,        │
                                        see g_running=0, exit            │
   pthread_join all
   close(all_fds[i])
   sum sent/received/stalls
   qsort latencies
   compute percentiles
   print
   free everything
```

Note: the **`g_running` flag is `_Atomic int`** (relaxed-ordering use), so
the writes/reads are visible across threads without needing a fence. We don't
need stricter ordering because we don't gate any other memory operation on
this flag — it's purely a "should I keep looping" hint.

## 11. The results output, decoded

```
Benchmarking 127.0.0.1:9000 for 5s, psize=1472 bytes, senders=4, sockets/sender=4 (total flows=16)...
[bench runs for 5 seconds]

--- Results ---
Duration:         5s
Senders:          4 (sockets/sender=4, total flows=16)
Packet size:      1472 bytes
Packets sent:     14051970         ← summed across all sender threads
Packets received: 7577500          ← summed across all receiver threads
Dropped:          6474470 (46.1%)  ← sent - received (kernel drops + lost echoes)
Mid-run stalls:   0 (≥100ms idle windows)
Throughput:       1515500 pps      ← received / duration

Round-trip latency (µs) — 7577500 samples:
  min   : 7.75
  p50   : 392.00 (median)
  p90   : 633.17
  p99   : 976.54
  p99.9 : 2051.38
  max   : 4435.96
  avg   : 404.34
```

What each line really means:

| Line             | Definition                                                   | What to look for                                |
| ---------------- | ------------------------------------------------------------ | ----------------------------------------------- |
| Packets sent     | Sum of `sender.sent` — every `sendto` that returned > 0      | Sanity: should grow ~linearly with `N × dur`    |
| Packets received | Sum of `receiver.received` — every well-formed echoed packet | This is the headline measurement                |
| Dropped          | `sent - received` — packets lost somewhere in the round trip | High % = server can't keep up at this load      |
| Mid-run stalls   | Count of `EAGAIN`s while `g_running` was still true          | Non-zero = server paused for ≥100 ms during run |
| Throughput       | `received / duration` — _delivered_ pps, not offered load    | The number to compare versions on               |
| min / p50 / ...  | Percentiles over all collected RTTs (post-`qsort`)           | p99 / p99.9 reveal tail-latency cliffs          |
| max              | Highest RTT observed                                         | Often noisy on cold-cache first packets         |
| avg              | Arithmetic mean over the sample set                          | Sensitive to outliers — prefer p50              |

## 12. Pitfalls and gotchas

- **`size < 16` is rounded up to 16** — must fit the header. Anything between
  16 and 1472 is fine.
- **`size > 1472` is clamped to 1472** — anything larger would fragment over
  Ethernet (MTU 1500 - IP 20 - UDP 8 = 1472).
- **Latency cap silently truncates**. If the server delivers more than 100 M
  packets in your run window, samples past 100 M are dropped from the
  histogram. They're still counted in "received" — but the percentiles
  represent only the first 100 M. Reduce `dur` or `psize` if you hit this.
- **`MAX_SAMPLES * 8 bytes = 800 MB`** is allocated up front via `malloc`,
  not faulted in lazily. On a memory-constrained host you may want to lower
  this constant.
- **The `Mid-run stalls` counter has 100 ms resolution.** Anything quicker
  than that won't show up here — use the p99.9 / max in the latency table for
  finer-grain pauses.
- **Loopback is not a substitute for real NIC measurement.** Loopback skips
  the driver, NIC ring, and physical-layer cost. v3 → v4 → v5 deltas are
  smaller on loopback than on a real NIC — the syscall cost is a higher
  fraction of total work there.
- **You're measuring the bench too.** If the bench's senders or receivers are
  the bottleneck (one core saturated at 100%), the "throughput" number is the
  bench's limit, not the server's. Scale `num_senders` or `sockets_per_sender`
  to confirm the server is the limiter.

## 13. Tuning advice

Start with the defaults (`N=4, K=1`) to confirm the server works. Then:

- **More flow coverage** → bump `K`. For an 8-thread server, `K=4` with `N=4`
  gives 16 flows, which the SO_REUSEPORT hash should distribute evenly.
- **More offered load** → bump `N`. Each sender contributes ~one core's worth
  of `sendto` pressure. If your bench cpuset has 8 CPUs, `N=4` (senders) +
  `N*K=16` (receivers) is already over-subscribed.
- **Lower noise floor** → larger `dur` (e.g. 30 s). The first ~100 ms is
  always cold-cache noise; longer runs amortize it. p99.9 also stabilizes
  with more samples.
- **Stress the server tail** → larger `size` (e.g. 1472). Smaller packets per
  byte means more pps for the same MB/s and exposes per-packet overhead.

## 14. Why this design — design choices justified

| Choice                                  | Alternative                                    | Why we picked this                                                                                           |
| --------------------------------------- | ---------------------------------------------- | ------------------------------------------------------------------------------------------------------------ |
| Fire-and-forget                         | Ping-pong (send→wait→recv)                     | Decouples throughput from RTT; one slow run doesn't ruin throughput; throughput limited by server, not bench |
| Timestamp in payload                    | Shared map: seq → send_time                    | No mutex/atomic on the sender hot path; receiver has zero shared state with sender                           |
| `CLOCK_MONOTONIC` from same process     | Per-host NTP-synced clocks                     | Bench is single-process; no skew between sender and receiver                                                 |
| `atomic_fetch_add` slot claim           | Per-thread arrays + concat at end              | Simpler; one big sort at end; cache-line ping-pong on `g_lat_idx` is the only cost                           |
| Pre-allocated 800 MB buffer             | Resize on demand                               | No alloc on the hot path; bounded memory; sorting is faster on contiguous memory                             |
| Pin every thread to one CPU             | Let scheduler decide                           | Run-to-run variance was dominated by scheduling decisions; pinning makes the measurement reproducible        |
| N senders × K sockets (not N×K senders) | N×K sender threads                             | Same flow coverage, fewer threads, fewer context switches on the bench side                                  |
| `SO_RCVTIMEO = 100 ms` on receivers     | Non-blocking + epoll                           | Simpler code; bench is allowed to "waste" a bit on EAGAIN since it isn't the measurement target              |
| One big `qsort` at end                  | Streaming percentile estimator (HDR, t-digest) | Exact percentiles; debuggable; cost is amortized once, off the hot path                                      |

The benchmark is intentionally simple — every layer that _could_ be smarter
(streaming percentiles, lock-free MPMC sample queues, vectored I/O on the
sender) was rejected because the goal is **measurement fidelity for v1..vN
deltas**, not benchmark-tool throughput records.
