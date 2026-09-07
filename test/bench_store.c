#define _POSIX_C_SOURCE 200809L
#include "test_support.h"
#include "gm_store.h"
#include <time.h>
#include <sys/resource.h>
static double now(void) {
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
int main(int argc, char **argv) {
    size_t count = argc == 2 ? (size_t)strtoul(argv[1], nullptr, 10) : 100000;
    CHECK(count > 0 && count <= 1000000);
    char dir[64];
    test_dir(dir);
    struct gm_store *st = nullptr;
    CHECK(gm_store_open_limited(dir, 1024, 7, 512u * 1024u * 1024u, &st) == GM_OK);
    uint8_t *v = malloc(count * 128u);
    CHECK(v);
    uint32_t r = 1;
    for (size_t i = 0; i < count * 128u; ++i) {
        r = r * 1664525u + 1013904223u;
        v[i] = (uint8_t)(r >> 24);
    }
    double start = now();
    CHECK(gm_store_replace(st, "benchmark", count, v) == GM_OK);
    double write = now() - start;
    struct gm_hit hits[5];
    size_t n = 0;
    double samples[101];
    for (size_t i = 0; i < 101; ++i) {
        start = now();
        CHECK(gm_store_scan(st, 5, v + (i % count) * 128u, hits, &n) == GM_OK && n > 0);
        samples[i] = (now() - start) * 1000;
    }
    qsort(samples, 101, sizeof *samples, compare);
    struct gm_stats s;
    CHECK(gm_store_stats(st, &s) == GM_OK);
    gm_store_close(st);
    start = now();
    CHECK(gm_store_open_limited(dir, 1024, 7, 512u * 1024u * 1024u, &st) == GM_OK);
    double reopen = now() - start;
    CHECK(gm_store_replace(st, "benchmark", 1, v) == GM_OK);
    start = now();
    CHECK(gm_store_compact(st) == GM_OK);
    double compact = now() - start;
    struct rusage usage;
    CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
#if defined(__APPLE__)
    double rss = (double)usage.ru_maxrss / 1048576;
#else
    double rss = (double)usage.ru_maxrss / 1024;
#endif
    printf("chunks=%zu dimension=1024 store_memory=%zu store_disk=%llu peak_process_rss_mib=%.2f\n",
           count, s.memory_bytes, (unsigned long long)s.disk_bytes, rss);
    printf("write_s=%.4f reopen_s=%.4f scan_p50_ms=%.3f scan_p95_ms=%.3f compact_to_one_s=%.4f\n",
           write, reopen, samples[50], samples[95], compact);
    free(v);
    gm_store_close(st);
    test_clean(dir);
    return 0;
}
