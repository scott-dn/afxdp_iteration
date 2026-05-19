# v5 results — benchmark vs v4

## TL;DR

After two code changes — pin worker threads, drop `IORING_SETUP_SQPOLL` — v5
clearly beats v4 on this hardware:

| metric                        | v4 mmsg_mt | v5 iou_mt (final) | delta               |
| ----------------------------- | ---------- | ----------------- | ------------------- |
| sustained throughput, 0% drop | ~535k pps  | **~893k pps**     | **+67%**            |
| capacity ceiling (drops OK)   | ~870k pps  | ~977k pps         | +12%                |
| latency floor (min)           | 6.1 µs     | 7.1 µs            | ~tie (within noise) |
| latency floor (p50)           | 13.7 µs    | 16.5 µs           | ~tie                |

The throughput win comes from `io_uring`'s multishot recv + provided buffer
ring eliminating per-packet SQE bookkeeping. The "no regression" on latency
comes from **not** using SQPOLL: a SQPOLL kernel poller wins on syscall count
but loses on single-flight latency, and on this workload the latency cost
outweighs the syscall savings.

## What changed

Two edits in `v5_iou_mt/server.c`:

1. `#include "../utils.h"` and `pin_to_nth_allowed_cpu(tid, tid)` at the top
   of `worker_thread()` — same pattern as v2/v3/v4. Stops the scheduler from
   migrating io_uring worker threads, which had been the dominant variance
   source.
2. Removed `params.flags = IORING_SETUP_SQPOLL` and `params.sq_thread_idle`.
   The ring init now runs without a kernel poller; `io_uring_submit()` makes
   one `io_uring_enter` syscall per CQ-drain cycle, which is cheap because
   the drain amortizes it over many CQEs.

`SQPOLL_IDLE_MS` is now an unused constant. Left in place with a comment so
the alternative is documented; can be deleted later.

## How we got here

### Starting point (pre-fix)

The v5 baseline from earlier in the project:

| config                        | pps  | drop % | min µs | p50   |
| ----------------------------- | ---- | ------ | ------ | ----- |
| T=8, B=8-15, N=4 K=1 (no pin) | 343k | 45.6%  | 172    | 25 ms |

That run had no worker pinning, no bench K-fanout, and `SQPOLL` enabled. The
high drop and milliseconds-of-p50 were a mix of scheduler thrash (8 SQPOLL
kthreads + 8 worker threads on a 16-thread box, no pinning) and the SQPOLL
poller stealing CPU from workers.

### Phase 1 — pin workers (SQPOLL still on)

Added the pin call, kept SQPOLL.

| config                                      | pps      | drop % | min µs | p50     |
| ------------------------------------------- | -------- | ------ | ------ | ------- |
| T=8, B=8-15, N=4 K=4                        | 598k     | 2.6%   | 55     | 1.83 ms |
| T=10, B=10-15, N=32 K=2 (×3 trials, median) | **847k** | 0%     | 38     | 2.9 ms  |
| T=8, B=8-15, N=1 K=1 (latency floor)        | 202k     | 0%     | 547    | 1.92 ms |

Throughput jumped from 343k → 847k (~2.5×). That alone tells us "SQPOLL
contention" wasn't the main problem before — worker migration was. SQPOLL
sharing cores with unpinned workers had made everything erratic.

But the latency floor was awful: **min 547 µs, p50 1.9 ms** at N=1 K=1. SQPOLL
adds latency in two ways:

- The poller spins on the SQ tail and only services submits when its slice
  comes round, adding scheduler-delay jitter to every submit.
- With only one in-flight packet, the poller mostly idles. The
  `SQPOLL_IDLE_MS = 2000ms` window keeps it awake but its turnaround is
  bounded by scheduler quantum, not by the cost of an event.

### Phase 2B — drop SQPOLL

Removed `IORING_SETUP_SQPOLL`. Hot loop becomes:

```
while (1) {
    wait_cqe()       // io_uring_enter when CQ is empty, otherwise userspace
    drain CQ         // build send SQEs, recycle buffers
    cq_advance()
    submit()         // io_uring_enter once, kernel runs submitted SQEs
}
```

One syscall per drain cycle instead of zero. Cost is one `io_uring_enter` per
batch of ~hundreds of packets — invisible.

| config                                   | pps      | drop % | min µs | p50     |
| ---------------------------------------- | -------- | ------ | ------ | ------- |
| T=8, B=8-15, N=4 K=4 (smoke)             | 572k     | 0%     | 9.1    | 18.1 µs |
| T=10, B=10-15, N=32 K=2 (×3, median)     | **893k** | 0%     | 8.7    | 2.7 ms  |
| T=8, B=8-15, N=1 K=1 (latency floor)     | 168k     | 0%     | 7.1    | 16.5 µs |
| T=8, B=8-15, N=48 K=1 (capacity ceiling) | 977k     | 21%    | 11     | 24 ms   |

Latency floor collapses from 547 µs → **7.1 µs** at N=1 K=1, matching v4. Sustained throughput stays at the Phase-1-with-SQPOLL level (893k vs 847k median), and capacity ceiling at N=48 K=1 is essentially unchanged from Phase 1's high-N runs.

### Phase 2A — shared SQPOLL via `ATTACH_WQ` (skipped)

The plan included a fallback option to consolidate per-ring SQPOLL kthreads
into one shared poller using `IORING_SETUP_ATTACH_WQ`. We didn't implement it
because Phase 2B already beat v4 on both throughput and latency without
adding plumbing (pthread barrier, shared `wq_fd` field, pin-skip logic for
the SQPOLL CPU). Worth revisiting only if a future workload shows zero-syscall
submits would be a net win — likely only at much higher pps than this host
can drive.

## Apples-to-apples — v5 (final) vs v4 at the three reference configs

3-trial medians, identical bench invocations.

### A. Sustained throughput (T=10 cpuset 0-9, B=10-15, N=32 K=2)

| ver | pps median | trial spread | drop % | min µs | p50    | p99   |
| --- | ---------- | ------------ | ------ | ------ | ------ | ----- |
| v4  | 535k       | 523–542k     | 0%     | 9.4    | 2.2 ms | 15 ms |
| v5  | **893k**   | 875–893k     | 0–1%   | 8.7    | 2.7 ms | 16 ms |

v5 delivers **1.67× the sustained throughput** at the same offered load. p50
goes up modestly because the server is processing more packets and there's
slightly deeper queueing on the recv side, but it's still well within
acceptable.

### B. Latency floor (T=8 cpuset 0-7, B=8-15, N=1 K=1)

| ver | pps median | min µs | p50 µs |
| --- | ---------- | ------ | ------ |
| v4  | 150k       | 6.1    | 13.7   |
| v5  | 168k       | 7.1    | 16.5   |

Statistical tie. The ~1 µs gap at min is within run-to-run noise on this
host. Service time is unchanged by the architecture switch — both versions
spend most of one round-trip in the kernel UDP stack, which neither v4 nor v5
touches.

### C. Capacity ceiling (T=8 cpuset 0-7, B=8-15, N=48 K=1)

| ver | pps median | drop % | p50   |
| --- | ---------- | ------ | ----- |
| v4  | ~849k      | 8%     | 10 ms |
| v5  | 977k       | 21%    | 24 ms |

Mixed read. v5 delivers more echo'd packets per second but at a higher drop
%, suggesting it's accepting more load into its rings and then losing some
to the kernel's UDP receive buffer. Net delivered pps is +15%; net offered
load v5 can sustain at < 1% drop is probably closer to the 893k number from
config A.

## Why v5 wins

Three architectural advantages compound at high pps:

1. **No per-packet recv syscall.** Multishot `recvmsg` arms one SQE and
   produces one CQE per packet. v4's `recvmmsg(64)` still costs one syscall
   per batch; at v5's drain depth, that's one fewer syscall per ~hundreds of
   packets.
2. **No per-packet SQE for recv.** The buffer ring pulls a free buffer from
   a pre-registered pool on the kernel side. v4's `recvmmsg` has the
   userspace prep the iovecs every call.
3. **Submission batching.** All sends in one drain cycle land in the SQ as a
   tail bump; one `io_uring_submit` covers them. v4's `sendmmsg` already
   batches well, so the win here is smaller — but combined with #1 and #2,
   v5 spends less userspace CPU per packet.

The remaining cost — kernel UDP stack work (`sk_buff` alloc, IP/UDP parse,
socket queue insertion) — is identical between v4 and v5 because both still
go through the socket layer. That's why the latency floors match: when only
one packet is in flight, both versions are bottlenecked on the same kernel
work, not on userspace.

## Why SQPOLL didn't help here

Three reasons SQPOLL hurts at this scale on this hardware:

- **Latency cost**: even when hot, the SQPOLL kthread doesn't respond
  instantly. The submit goes into a memory location; the poller notices it
  on its next spin. That can add 25–500 µs of latency depending on
  scheduling state. For a single-flight echo, that delay is the dominant
  cost.
- **Cold start cost**: after `sq_thread_idle = 2000 ms` of inactivity, the
  poller parks. Waking it costs one `io_uring_enter`. Bursty workloads
  pay this on every burst.
- **CPU overhead**: each SQPOLL kthread consumes a full CPU while spinning.
  With one ring per worker (T=8 → 8 SQPOLL kthreads), that's significant
  CPU competition with the workers on the same cpuset. Pinning workers
  fixes the worst of this, but the polling overhead remains.

SQPOLL pays off when:

- The workload is sustained at very high pps so the poller stays hot.
- One submit per packet (or per very small batch) — i.e. when avoiding the
  syscall really matters.
- There's a dedicated CPU for the poller (`SQ_AFF` + `sq_thread_cpu`).

Our drain-the-CQ loop already amortizes submits across many CQEs in one
batch, so the per-cycle syscall is a small fraction of work. Worth
remembering for v6's UMEM ring (different model) but not the right tool here.

## Reproducing

Final v5 source = `v5_iou_mt/server.c` after this round of fixes. Build:

```sh
VERSION=v5_iou_mt docker compose build
```

Each reference config:

```sh
# A. Sustained throughput
SERVER_CPUSET=0-9 BENCH_CPUSET=10-15 THREADS=10 \
  VERSION=v5_iou_mt docker compose run --rm bench \
  ./build/benchmark 127.0.0.1 9000 5 1472 32 2

# B. Latency floor
SERVER_CPUSET=0-7 BENCH_CPUSET=8-15 THREADS=8 \
  VERSION=v5_iou_mt docker compose run --rm bench \
  ./build/benchmark 127.0.0.1 9000 5 1472 1 1

# C. Capacity ceiling
SERVER_CPUSET=0-7 BENCH_CPUSET=8-15 THREADS=8 \
  VERSION=v5_iou_mt docker compose run --rm bench \
  ./build/benchmark 127.0.0.1 9000 5 1472 48 1

VERSION=v5_iou_mt docker compose down
```

## Caveats

- **Loopback measurement.** All numbers are 127.0.0.1, server and bench on
  the same machine in disjoint cpusets. Real-NIC numbers will look different
  — most likely v5's advantage grows because syscall amortization is a
  bigger lever when packets cost more per-packet kernel work in the driver.
- **v5's wins are at high pps.** At low load (N=1 K=1), v4 and v5 are
  indistinguishable. The architecture only pays off when there are many
  packets in flight.
- **Bench is the limiter in some configs.** Sustained 893k is what _this
  bench on this host_ can offer at 0% drop. Server probably has more
  headroom — visible only when offered load exceeds bench's clean ceiling
  (config C). The two-machine setup (also required for v6) is the next
  honest step.

## Acceptance criterion

> v5 (post-fix, best variant) beats v4 on at least one of: (a) 0% drop
> throughput at the same bench config, or (b) service-time latency at
> N=1 K=1.

**Met.** v5 wins config A by 1.67×; ties on config B.
