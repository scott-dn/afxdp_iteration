# Why v5 doesn't use `IORING_SETUP_SQPOLL`

`io_uring` ships with a "kernel-side submission poller" mode that, on paper,
should be strictly better than what v5 actually does: zero syscalls on the
submit path. We tried it. It lost on every dimension that mattered for this
workload — latency floor, throughput, and CPU usage. This doc records the
measurement and the three reasons.

## What SQPOLL is supposed to do

Without SQPOLL (the shipping v5):

```
  app             kernel
   │               │
   │  build SQE    │
   ├──memory write─┤   (SQ tail advanced in userspace)
   │               │
   │  submit()     │
   ├──syscall─────►│   io_uring_enter: kernel reads SQ, runs op
   │               │
   │◄──CQE─────────┤
```

With SQPOLL, the kernel spawns a kthread per ring that busy-polls the SQ
tail. The userspace `submit()` becomes a pure memory write:

```
  app             kernel kthread (always running)
   │               │
   │  build SQE    │
   ├──memory write─┤   (SQ tail advanced)
   │               │   poller notices on next spin, runs op
   │               │
   │◄──CQE─────────┤
```

In return for that "free" submit, you pay:

- One kthread per ring, busy-spinning a CPU.
- Polling latency: the kthread sees your submit on its next iteration, not
  instantly.
- A cold-start cost: after `sq_thread_idle` ms of inactivity the kthread
  parks; the next submit needs an `io_uring_enter` to wake it.

That tradeoff is workload-dependent. Here's how it played out for UDP echo.

## The measurement

Same binary, same hardware (Ryzen 7 PRO 8840U, 8 physical / 16 logical, with
workers pinned to a `cpuset`). The only variable is whether
`IORING_SETUP_SQPOLL + sq_thread_idle=2000ms` is passed to
`io_uring_queue_init_params`.

### Latency floor (T=8 cpuset 0-7, B=8-15, N=1 K=1)

One packet in flight at a time — measures pure service time.

| variant     | pps  | min µs  | p50 µs  | p99    |
| ----------- | ---- | ------- | ------- | ------ |
| SQPOLL on   | 202k | **547** | 1920    | ~3 ms  |
| SQPOLL off  | 168k | **7.1** | **16.5**| 35 µs  |

**SQPOLL injects ~540 µs into the single-flight round trip.** That's not a
measurement artifact; it's the polling delay made visible. The poller wakes
on its own clock, not on yours.

### Sustained throughput (T=10 cpuset 0-9, B=10-15, N=32 K=2)

Many flows in flight, server kept busy.

| variant     | pps median | drop % | min µs | p50    |
| ----------- | ---------- | ------ | ------ | ------ |
| SQPOLL on   | 847k       | 0%     | 38     | 2.9 ms |
| SQPOLL off  | **893k**   | 0–1%   | 8.7    | 2.7 ms |

The "zero-syscall submit" advantage doesn't show up. SQPOLL is +0%
throughput — actually slightly _worse_ than dropping it — and worse on
every latency stat.

### Capacity ceiling (T=8 cpuset 0-7, B=8-15, N=48 K=1)

Offered load above the bench's clean ceiling — measures what the server can
absorb before drops compound.

| variant     | pps median | drop % | p50   |
| ----------- | ---------- | ------ | ----- |
| SQPOLL on   | ~950k      | 22%    | 26 ms |
| SQPOLL off  | 977k       | 21%    | 24 ms |

Essentially the same. Capacity at saturation is set by the kernel UDP stack,
not by submit overhead.

## Why SQPOLL loses here — three reasons

### 1. Polling delay is paid every round trip

The kthread doesn't service submits instantly. It polls the SQ tail on its
own schedule — every spin loop iteration, gated by whatever else is on the
CPU. Empirically that turns into anywhere from tens of microseconds (hot
poller, dedicated CPU) to hundreds of microseconds (poller sharing a CPU
with workers) of added latency on every submit.

When there's exactly one packet in flight (`N=1 K=1`), the entire round
trip is `wait_cqe → drain → submit → wait_cqe`. With SQPOLL, the submit-to-
sendmsg gap is the polling delay. That's why the latency floor jumped from
~7 µs to ~547 µs.

At sustained high pps the delay is hidden — there's always more work
queued, so the poller is never "idle and slow to respond." But it doesn't
make submits any faster either.

### 2. The submit syscall it eliminates is already cheap

The v5 hot loop batches submissions:

```c
while (1) {
    wait_cqe();                  // io_uring_enter only if CQ empty
    for_each_cqe { ... }         // build N send SQEs in memory
    cq_advance();
    io_uring_submit();           // one syscall covers all N sends
}
```

One drain cycle processes hundreds of CQEs and posts hundreds of SQEs. The
single `io_uring_submit` covering all of them costs one `io_uring_enter`
per cycle — a tiny fraction of the work done in that cycle. SQPOLL would
turn that "one syscall per hundreds of packets" into "zero syscalls per
hundreds of packets." The difference is invisible in `perf`.

SQPOLL pays off when you submit one SQE per packet — when the syscall
amortization isn't there. We have it, so the savings are zero.

### 3. CPU oversubscription

Each SQPOLL ring spawns a kthread that consumes one full CPU while
spinning. At T=8 workers that's 8 SQPOLL kthreads on top of 8 worker
threads = 16 active threads competing for 8 logical CPUs in the server's
cpuset.

After pinning workers, the workers don't migrate. But the SQPOLL kthreads
do — and they each want a full CPU's worth of time. The kernel ends up
scheduling them against the worker pinned to the same core, which steals
worker cycles. Throughput suffers exactly where SQPOLL was supposed to
help.

The fix would be `sq_thread_cpu + IORING_SETUP_SQ_AFF` to pin each kthread
to a dedicated CPU outside the worker cpuset — but that requires reserving
real CPUs for the pollers, which only makes sense if (1) above were also
true (i.e. SQPOLL was actually saving meaningful syscall cost).

## When SQPOLL would actually pay off

SQPOLL is the right tool when **all three** of these hold:

- **Submits cannot batch.** One SQE per event, no `submit()` amortization.
  E.g. latency-bound RPC where you can't wait for more requests before
  submitting.
- **The poller has its own CPU.** Either via `sq_thread_cpu` pinning or via
  `IORING_SETUP_ATTACH_WQ` to share one poller across many rings, with the
  shared poller pinned to a CPU outside the worker set.
- **Single-flight latency doesn't matter.** Either you're sustained at
  high pps (poller always hot, polling delay hidden behind queued work),
  or your latency budget is loose enough that 25–500 µs of polling delay
  doesn't blow your SLO.

For UDP echo on this host, none of those hold. Submits batch naturally,
the cpuset is fully occupied by workers, and the headline number in the
project README is the single-flight latency floor.

So v5 ships without SQPOLL. The one `io_uring_enter` per drain cycle is
cheaper than the alternative, both in measurable latency and in keeping
the implementation simple.

## Reproducing the comparison

The shipped `v5_iou_mt/server.c` doesn't have a SQPOLL toggle — it would
just be dead code. To reproduce the comparison, patch the params block:

```c
struct io_uring_params params = {0};
params.flags          = IORING_SETUP_SQPOLL;
params.sq_thread_idle = 2000;
```

…rebuild, and run the three reference configs from
[`RESULT.md`](./RESULT.md). The numbers above were taken with this exact
diff plus pinned workers, so the comparison isolates SQPOLL.

> ⚠️  If you try SQPOLL without `IPC_LOCK` capability and `memlock=unlimited`
> the ring init will fail at `io_uring_queue_init_params` with ENOMEM —
> the poller pins memory at registration. The `compose.yaml` in this repo
> already sets both.
