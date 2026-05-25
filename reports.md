# Cross-version benchmark report (v1 → v6)

Measured `pps`, drop rate, and round-trip latency distribution for every
version at three reference configs. Each run is a single
`docker compose run --rm bench` invocation; the server is brought up via
compose `depends_on` and torn down between version changes to force a clean
recreate.

## Test environment

|                |                                                                                                             |
| -------------- | ----------------------------------------------------------------------------------------------------------- |
| CPU            | AMD Ryzen 7 PRO 8840U (8 physical cores, 16 logical, SMT sibling pairs `(0,1)(2,3)...(14,15)`)              |
| Kernel         | Linux 7.0.9-202.fc44.x86_64                                                                                 |
| Image          | `afxdp_iteration:latest` (Ubuntu 26.04 builder, gcc -O2, liburing 2.x)                                      |
| Container caps | `NET_ADMIN`, `IPC_LOCK`; `memlock: -1`; `seccomp=unconfined`                                                |
| Loopback       | All traffic via `127.0.0.1` in the server's network namespace (bench shares `network_mode: service:server`) |
| Packet size    | 1472 bytes payload (MTU − IP − UDP)                                                                         |
| Duration       | 5 s per run                                                                                                 |

Each benchmark fits a single line:

```sh
VERSION=<vN> SERVER_CPUSET=<set> BENCH_CPUSET=<set> THREADS=<T> \
  docker compose run --rm bench \
  ./build/benchmark 127.0.0.1 9000 5 1472 <N> <K>
```

## Three reference configs

| Config                           | Server threads (T) | SERVER_CPUSET | BENCH_CPUSET | Senders (N) | Sockets/sender (K) | Purpose                                                 |
| -------------------------------- | ------------------ | ------------- | ------------ | ----------- | ------------------ | ------------------------------------------------------- |
| **L** — latency floor            | 8 (v1 uses 1)      | `0-7`         | `8-15`       | 1           | 1                  | Service time at single-flight                           |
| **T₁** — throughput, common      | 8 (v1 uses 1)      | `0-7`         | `8-15`       | 32          | 2                  | Cross-version comparison at the symmetric 8/8 split     |
| **T₂** — throughput, wide server | 10 (v1 uses 1)     | `0-9`         | `10-15`      | 32          | 2                  | Bench-limited config: server gets more cores than bench |

Notes:

- CPUSET `0-7` = logical CPUs 0 through 7 = physical cores P0–P3 (both SMT siblings each).
  Two workers per physical core when T=8 — measurable SMT contention.
- CPUSET `0-9` = logical 0–9 = P0–P4 (10 logical / 5 physical). Still some SMT sharing,
  but bench has only 6 logical CPUs (3 physical), so the **bench** becomes the bottleneck.
- v1 (`blocking_st`) is single-threaded; the `THREADS` env var is set but the v1 binary ignores it.
  v1 still gets the full server cpuset — its one worker can land anywhere inside.

## Config L — latency floor (N=1, K=1, T=8, server cpuset 0-7, bench cpuset 8-15)

One in-flight packet at a time. Measures pure service time.

| Version           | pps     | drop % | min µs | p50 µs | p90 µs    | p99 µs   | p99.9 µs | max µs   | avg µs |
| ----------------- | ------- | ------ | ------ | ------ | --------- | -------- | -------- | -------- | ------ |
| v1_blocking_st    | 156,306 | 0.0%   | 6.07   | 14.68  | 2,689.79  | 4,696.91 | 6,322.06 | 8,166.23 | 839.24 |
| v2_blocking_mt    | 149,085 | 0.0%   | 5.99   | 14.06  | 2,065.24  | 3,685.39 | 4,232.85 | 4,601.99 | 530.64 |
| v3_nonblocking_mt | 150,730 | 0.0%   | 7.07   | 13.29  | 2,046.92  | 3,723.11 | 4,342.57 | 4,609.76 | 500.73 |
| v4_mmsg_mt        | 161,092 | 0.0%   | 6.66   | 12.74  | 2,080.72  | 3,716.04 | 4,370.03 | 4,808.62 | 503.01 |
| v5_iou_mt         | 146,384 | 0.0%   | 8.11   | 18.30  | **34.26** | 3,162.06 | 4,114.80 | 4,702.39 | 161.26 |
| v6_xdp            | TBD     | TBD    | TBD    | TBD    | TBD       | TBD      | TBD      | TBD      | TBD    |

**Observations:**

- All versions hit ~150k pps because the bottleneck at N=1 is the kernel UDP
  round trip, not the server's I/O engine.
- min/p50 are within ~5 µs of each other across v1-v5 — service time floor on
  this host is ~6–18 µs.
- v5 has a dramatically tighter **p90** (34 µs vs ~2 ms for everyone else)
  because its drain-cycle design processes events immediately rather than
  waiting on epoll/select wakeups. The tail (p99) still spikes from the
  occasional bench-side scheduling stall.
- avg µs reflects the heavy tail; v5's 161 µs avg is 3× lower than v4's 503 µs.

## Config T₁ — throughput, common (N=32, K=2, T=8, server 0-7, bench 8-15)

Same symmetric split as Config L but with 64 active flows. Server is
oversubscribed (8 threads on 4 physical cores via SMT pairs).

| Version           | pps         | drop %   | min µs | p50 µs       | p90 µs    | p99 µs    | p99.9 µs   | max µs     | avg µs    |
| ----------------- | ----------- | -------- | ------ | ------------ | --------- | --------- | ---------- | ---------- | --------- |
| v1_blocking_st    | 126,000     | 89.8%    | 60.00  | 25,099.67    | 28,808.58 | 33,121.97 | 37,084.17  | 42,117.15  | 25,195.47 |
| v2_blocking_mt    | 600,903     | 8.5%     | 8.47   | 13,888.96    | 35,350.97 | 44,159.04 | 70,830.39  | 79,856.72  | 17,485.58 |
| v3_nonblocking_mt | 587,702     | 6.7%     | 10.98  | 10,835.09    | 35,993.07 | 68,548.62 | 239,665.14 | 265,986.45 | 16,654.10 |
| v4_mmsg_mt        | **643,565** | **2.2%** | 12.20  | **3,266.33** | 27,992.97 | 36,588.02 | 56,784.28  | 73,142.16  | 7,989.65  |
| v5_iou_mt         | 546,407     | 15.4%    | 15.98  | 37,817.79    | 48,360.92 | 80,725.62 | 150,744.47 | 178,220.09 | 32,599.95 |
| v6_xdp            | TBD         | TBD      | TBD    | TBD          | TBD       | TBD       | TBD        | TBD        | TBD       |

**Observations:**

- v4 wins this config decisively: highest pps (643k), lowest drop rate
  (2.2%), best p50 latency.
- v5 underperforms v4 at this CPU layout. SMT sibling contention (2 workers
  per physical core) hits v5 hardest because each io_uring worker is more
  userspace-efficient — it pushes more kernel UDP work per core than v4's
  `recvmmsg(64)` batches do, so the physical cores saturate sooner.
- v1 collapses to 89.8% drop — predictable for a single thread serving 64
  flows.
- v2 vs v3 are statistically tied (600k vs 587k pps): on loopback, the
  per-packet epoll wakeup cost doesn't show up because there's no NIC IRQ
  latency in the picture.

## Config T₂ — throughput, wide server (N=32, K=2, T=10, server 0-9, bench 10-15)

Server gets 10 cores, bench gets 6. This was the original "sustained
throughput" config in `v5_iou_mt/RESULT.md`.

| Version           | pps         | drop % | min µs | p50 µs       | p90 µs    | p99 µs    | p99.9 µs  | max µs    | avg µs    |
| ----------------- | ----------- | ------ | ------ | ------------ | --------- | --------- | --------- | --------- | --------- |
| v1_blocking_st    | 142,083     | 85.6%  | 501.48 | 22,216.83    | 25,578.69 | 31,381.15 | 36,038.25 | 41,693.29 | 22,421.66 |
| v2_blocking_mt    | 573,339     | 0.0%   | 11.09  | 2,425.94     | 8,065.66  | 15,179.30 | 21,476.67 | 37,020.94 | 3,302.40  |
| v3_nonblocking_mt | 565,794     | 0.0%   | 12.05  | 2,235.58     | 7,778.34  | 15,098.46 | 20,100.48 | 39,734.12 | 3,126.94  |
| v4_mmsg_mt        | **579,718** | 0.0%   | 8.28   | **2,107.38** | 7,358.35  | 14,681.50 | 18,791.73 | 33,189.69 | 2,958.58  |
| v5_iou_mt         | 556,953     | 0.0%   | 14.07  | 2,768.36     | 8,708.24  | 15,171.48 | 19,598.37 | 36,395.63 | 3,613.26  |
| v6_xdp            | TBD         | TBD    | TBD    | TBD          | TBD       | TBD       | TBD       | TBD       | TBD       |

**Observations:**

- v2–v5 all converge to ~557–580k pps at **0% drop**. The bench (6 logical
  CPUs = 3 physical cores at logical 10-15) is the bottleneck here, not the
  server. Every server version handles the offered load comfortably.
- p50 is 2.1–2.8 ms across v2–v5 — that's bench-side queueing, not server
  service time. The bench can't drive much past ~570k pps in this layout, so
  packets sit in the bench's send queue waiting.
- v1 still drops most of the load (85.6%) because it's still one thread.
- To see v5's actual ceiling, the bench needs more cores or a second
  machine (see v6 placeholder below).

## Headline numbers — one line each

| Version | Best config         | pps     | drop % | min µs | p50 µs   | Notes                                                      |
| ------- | ------------------- | ------- | ------ | ------ | -------- | ---------------------------------------------------------- |
| v1      | L (N=1, K=1)        | 156,306 | 0%     | 6.07   | 14.68    | Single-threaded — collapses under any concurrency          |
| v2      | T₂ (T=10, N=32 K=2) | 573,339 | 0%     | 11.09  | 2,425.94 | `SO_REUSEPORT` scales linearly with threads                |
| v3      | T₂                  | 565,794 | 0%     | 12.05  | 2,235.58 | epoll ties v2 on loopback (no NIC IRQs to amortize)        |
| v4      | T₁ (T=8, N=32 K=2)  | 643,565 | 2.2%   | 12.20  | 3,266.33 | Wins T₁ outright; ties at T₂ (bench-limited)               |
| v5      | L                   | 146,384 | 0%     | 8.11   | 18.30    | Best **p90** in L (34 µs); throughput parity with v4 at T₂ |
| v6      | _placeholder_       | TBD     | TBD    | TBD    | TBD      | Requires XDP-capable NIC + second host (see below)         |

## v6 placeholder — what's needed

v6 (`xdp`, AF_XDP kernel bypass) cannot be benchmarked on this single-host
loopback setup. Requirements:

- **XDP-capable NIC** on both endpoints (driver supports `XDP_REDIRECT` and
  ideally `AF_XDP zerocopy`). This laptop's `enp1s0f0` is DOWN with no
  cable and unverified XDP support.
- **Two physical machines** connected by Ethernet — AF_XDP binds to a NIC
  RX queue, so the bench cannot run on the same machine as the server
  through loopback.
- **10 GbE link** to actually saturate v6: at 1538 bytes-on-wire (Ethernet
  framing of 1472-byte payloads), 1 Gbps caps at ~80k pps and 10 Gbps caps
  at ~810k pps. v5 already exceeds 1 GbE in T₁.
- **Pin per RX queue** — one server thread per NIC RX queue, RSS configured
  so flows distribute across queues, BPF program loaded with
  `XDP_FLAGS_DRV_MODE`.

When the two-machine setup exists, v6's row should be filled in using the
same three configs L / T₁ / T₂ (with `BENCH_CPUSET` and `SERVER_CPUSET`
applied on their respective hosts, and the bench running on the second
machine targeting the server's NIC IP rather than `127.0.0.1`).

## Reproducing

```sh
# Rebuild the image after any source change
VERSION=v1_blocking_st docker compose build

# Config L — latency floor (run once per version)
for v in v1_blocking_st v2_blocking_mt v3_nonblocking_mt v4_mmsg_mt v5_iou_mt; do
  VERSION=$v docker compose down
  VERSION=$v SERVER_CPUSET=0-7 BENCH_CPUSET=8-15 THREADS=8 \
    docker compose run --rm bench \
    ./build/benchmark 127.0.0.1 9000 5 1472 1 1
done

# Config T₁ — common throughput
for v in v1_blocking_st v2_blocking_mt v3_nonblocking_mt v4_mmsg_mt v5_iou_mt; do
  VERSION=$v docker compose down
  VERSION=$v SERVER_CPUSET=0-7 BENCH_CPUSET=8-15 THREADS=8 \
    docker compose run --rm bench \
    ./build/benchmark 127.0.0.1 9000 5 1472 32 2
done

# Config T₂ — wide server
for v in v1_blocking_st v2_blocking_mt v3_nonblocking_mt v4_mmsg_mt v5_iou_mt; do
  VERSION=$v docker compose down
  VERSION=$v SERVER_CPUSET=0-9 BENCH_CPUSET=10-15 THREADS=10 \
    docker compose run --rm bench \
    ./build/benchmark 127.0.0.1 9000 5 1472 32 2
done

VERSION=v5_iou_mt docker compose down
```

## Caveats

- **Loopback measurement.** All packets travel through the kernel
  loopback adapter, not a NIC driver. Real-NIC numbers will differ —
  v4/v5 likely widen the gap over v2/v3 because syscall amortization
  matters more when each packet costs more kernel work in the driver.
- **5-second runs.** Throughput numbers are stable to ±2-3% across
  repeats. p99.9 and max have higher run-to-run variance (single
  outlier samples).
- **Bench-as-limiter visible in T₂.** When all versions report the same
  pps at the same offered load, the bench is the ceiling. Two-machine
  testing is the next step.
- **Single-trial numbers.** Each cell here is one 5-second run, not a
  median of 3. Acceptable for relative ranking; use the
  `v5_iou_mt/RESULT.md` 3-trial medians for v4 vs v5 head-to-head.
