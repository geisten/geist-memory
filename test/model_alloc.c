/* GNU ld --wrap instrumentation, test executable only. Single-threaded native GEMM.
 * Tracks requested bytes in project/engine allocations, not libc internals or mmap.
 * A fixed 16-MiB pointer table avoids allocating inside the allocator wrappers. */
#include "model_alloc.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__real_aligned_alloc(size_t, size_t);
int __real_posix_memalign(void **, size_t, size_t);
char *__real_strdup(const char *);
void __real_free(void *);

enum { SLOTS = 1 << 20 };
static struct {
    void *p;
    size_t n;
} entries[SLOTS];
static struct model_alloc_stats stats;
static size_t live, minimum;
static long fail_at;
static bool active;
static void exhausted(void) {
    _exit(99);
}
static size_t slot(void *p, bool insert) {
    size_t start = ((uintptr_t)p >> 4) * (size_t)11400714819323198485ull;
    size_t tomb = SLOTS;
    for (size_t i = 0; i < SLOTS; ++i) {
        size_t s = (start + i) & (SLOTS - 1);
        if (entries[s].p == p)
            return s;
        if (entries[s].p == (void *)(uintptr_t)1 && tomb == SLOTS)
            tomb = s;
        if (!entries[s].p)
            return insert && tomb != SLOTS ? tomb : s;
    }
    if (insert && tomb != SLOTS)
        return tomb;
    exhausted();
    return 0;
}
static void add(void *p, size_t n) {
    if (!p)
        return;
    size_t s = slot(p, true);
    entries[s].p = p;
    entries[s].n = n;
    if (n > SIZE_MAX - live)
        exhausted();
    live += n;
    if (active && live > stats.peak)
        stats.peak = live;
}
static void remove_pointer(void *p) {
    if (!p)
        return;
    size_t s = slot(p, false);
    if (entries[s].p != p)
        return; /* Allocation internal to a shared libc. */
    live -= entries[s].n;
    entries[s].p = (void *)(uintptr_t)1;
    entries[s].n = 0;
}
static bool reject(size_t n) {
    if (!active)
        return false;
    ++stats.calls;
    if (n > stats.largest)
        stats.largest = n;
    if (n < minimum)
        return false;
    size_t index = stats.eligible++;
    if (fail_at < 0 || index != (size_t)fail_at)
        return false;
    ++stats.failed;
    errno = ENOMEM;
    return true;
}
void model_alloc_begin(long at, size_t min) {
    stats = (struct model_alloc_stats){.live = live, .peak = live};
    minimum = min;
    fail_at = at;
    active = true;
}
struct model_alloc_stats model_alloc_end(void) {
    active = false;
    stats.live = live;
    return stats;
}
size_t model_alloc_live(void) {
    return live;
}
void *__wrap_malloc(size_t n) {
    if (reject(n))
        return nullptr;
    void *p = __real_malloc(n);
    add(p, n);
    return p;
}
void *__wrap_calloc(size_t a, size_t b) {
    size_t n;
    if (__builtin_mul_overflow(a, b, &n)) {
        errno = ENOMEM;
        return nullptr;
    }
    if (reject(n))
        return nullptr;
    void *p = __real_calloc(a, b);
    add(p, n);
    return p;
}
void *__wrap_realloc(void *p, size_t n) {
    if (reject(n))
        return nullptr;
    /* Remove before realloc to avoid reading the old pointer after it is freed. */
    size_t s = p ? slot(p, false) : 0;
    size_t old = p && entries[s].p == p ? entries[s].n : 0;
    bool tracked = p && entries[s].p == p;
    remove_pointer(p);
    void *q = __real_realloc(p, n);
    if (q)
        add(q, n);
    else if (n && tracked)
        add(p, old);
    return q;
}
void *__wrap_aligned_alloc(size_t alignment, size_t n) {
    if (reject(n))
        return nullptr;
    void *p = __real_aligned_alloc(alignment, n);
    add(p, n);
    return p;
}
int __wrap_posix_memalign(void **p, size_t alignment, size_t n) {
    if (reject(n))
        return ENOMEM;
    int s = __real_posix_memalign(p, alignment, n);
    if (!s)
        add(*p, n);
    return s;
}
char *__wrap_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    if (reject(n))
        return nullptr;
    char *p = __real_strdup(s);
    add(p, n);
    return p;
}
void __wrap_free(void *p) {
    remove_pointer(p);
    __real_free(p);
}
