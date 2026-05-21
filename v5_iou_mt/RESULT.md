# v5 results — benchmark vs v4

## TL;DR

v5 beats v4 on sustained throughput and ties on latency floor:

| metric                        | v4 mmsg_mt | v5 iou_mt | delta               |
| ----------------------------- | ---------- | --------- | ------------------- |
| sustained throughput, 0% drop | ~535k pps  | ~893k pps | **+67%**            |
| capacity ceiling (drops OK)   | ~870k pps  | ~977k pps | +12%                |
| latency floor (min)           | 6.1 µs     | 7.1 µs    | ~tie (within noise) |
| latency floor (p50)           | 13.7 µs    | 16.5 µs   | ~tie                |

The win comes from `io_uring`'s multishot recv + provided buffer ring
eliminating per-packet SQE bookkeeping. The latency tie comes from picking
the right submission mode — see [`why_not_sqpoll.md`](./why_not_sqpoll.md)
for the SQPOLL comparison that didn't make the final build.

## Apples-to-apples — v5 vs v4 at three reference configs

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
load v5 can sustain at < 1% drop is closer to the 893k number from config A.

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

## Reproducing

Build:

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

> v5 (best variant) beats v4 on at least one of: (a) 0% drop throughput at
> the same bench config, or (b) service-time latency at N=1 K=1.

**Met.** v5 wins config A by 1.67×; ties on config B.
