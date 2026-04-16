#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

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

#endif /* UTILS_H */
