#ifndef UTILS_H
#define UTILS_H

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Parse a base-10 int from str. Returns parsed value on success, fallback on
 * any failure (NULL, empty, non-numeric trailing chars, out-of-int-range).
 * Replaces atoi() — atoi silently returns 0 for "abc" and ignores overflow,
 * which clang-tidy flags as bugprone-unchecked-string-to-number-conversion. */
static inline int parse_int_or(const char *str, int fallback) {
    if (str == NULL || *str == '\0') return fallback;
    errno         = 0;
    char *end     = NULL;
    long  v       = strtol(str, &end, 10);
    if (errno != 0 || end == str || *end != '\0') return fallback;
    if (v < INT_MIN || v > INT_MAX) return fallback;
    return (int)v;
}

/* Returns current time in nanoseconds from CLOCK_MONOTONIC.
 * CLOCK_MONOTONIC: always increases, unaffected by wall clock changes (NTP, DST, etc.)
 *   — the right clock for measuring elapsed time between two points on the same machine.
 * timespec stores seconds + nanoseconds separately; we combine into a single uint64_t
 *   by multiplying seconds by 1e9 and adding nanoseconds. */
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Comparator for qsort() — must take void* since qsort is type-agnostic.
 * (x > y) - (x < y) is a branchless way to return -1, 0, or 1:
 *   x < y → 0 - 1 = -1  (a before b)
 *   x = y → 0 - 0 =  0  (equal)
 *   x > y → 1 - 0 =  1  (a after b)
 * Avoids naive (x - y) which would overflow for large uint64_t values. */
static inline int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Returns the p-th percentile value from a pre-sorted array using the nearest-rank method.
 * p is 0–100 (e.g. 99.9 for p99.9); n is the array length.
 *
 * nearest-rank: rank = ceil(p/100 * n), clamped to [1, n]; index = rank - 1.
 * The floor+bump avoids pulling in math.h for ceil():
 *   if rank is an exact integer it stays as-is; if fractional it rounds up.
 *
 * vs. the previous floor-based formula: floor(0.99 * 100) = 99 → sorted[99] (the max!);
 * nearest-rank gives ceil(99.0) = 99 → sorted[98], the correct p99 value. */
static inline uint64_t percentile(uint64_t *sorted, size_t n, double p) {
    double rank = p / 100.0 * (double)n;
    size_t idx  = (size_t)rank;    /* floor */
    if (rank > (double)idx) idx++; /* ceil: bump if not an exact integer */
    if (idx == 0) idx = 1;         /* minimum rank is 1 */
    if (idx > n) idx = n;          /* clamp to array length */
    return sorted[idx - 1];        /* convert rank (1-based) to index (0-based) */
}

/* Pin the calling thread to the nth CPU within the current affinity mask.
 *
 * "Current mask" = whatever sched_getaffinity reports for this thread — typically
 * the docker cpuset (e.g. logical 0-7), or the host's full set if launched without
 * any external pinning. We pick the nth set bit from that mask, modulo its size,
 * and clamp the calling thread to that single CPU.
 *
 * Why nth-set-bit rather than just CPU = n: the allowed mask may not start at 0
 * (host taskset, k8s cpuset, etc.) and may have holes. Indexing by set-bit makes
 * worker N always land on the Nth *available* CPU — works whether siblings are
 * adjacent (e.g. {0,1,2,3} on a 2-physical/SMT box) or one-per-physical-core
 * (e.g. {0,2,4,6}). Caller doesn't need to know the topology.
 *
 * Logs the chosen cpu + size of the allowed set — if the operator forgot to set
 * cpuset/taskset, "allowed=16 cpus" being printed when only 8 workers exist is
 * the visible cue. */
static inline int pin_to_nth_allowed_cpu(int tid, int n) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        perror("sched_getaffinity");
        return -errno;
    }

    int total = CPU_COUNT(&allowed);
    if (total == 0) return -ENODEV;

    /* Modulo so callers with n >= total wrap rather than fail. Matches "round robin
     * across the allowed set" semantics that callers usually want. */
    int target = n % total;

    int chosen = -1;
    int seen   = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        if (seen == target) {
            chosen = cpu;
            break;
        }
        seen++;
    }

    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(chosen, &one);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(one), &one);
    if (rc != 0) {
        fprintf(stderr, "pthread_setaffinity_np cpu=%d: %s\n", chosen, strerror(rc));
        return -rc;
    }

    printf("thread %d: pinned to cpu %d (allowed=%d cpus)\n", tid, chosen, total);
    return 0;
}

#endif /* UTILS_H */
