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

/* Returns the p-th percentile value from a pre-sorted array.
 * p is 0–100 (e.g. 99.9 for p99.9); n is the array length.
 * idx maps p to an array index: p=50 → middle, p=99.9 → near the end.
 * Clamps idx to n-1 to guard against floating point rounding pushing it out of bounds. */
static inline uint64_t percentile(uint64_t *sorted, size_t n, double p) {
    size_t idx = (size_t)(p / 100.0 * (double)n);
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

#endif /* UTILS_H */
