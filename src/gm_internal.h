#ifndef GM_INTERNAL_H
#define GM_INTERNAL_H
#include "geist_memory.h"
#include "geist_memory_embedder.h"
#include <stdlib.h>
#include <string.h>
#if __has_include(<stdckdint.h>)
#include <stdckdint.h>
#else
/* GCC/Clang fallback for C23 compilers paired with an older libc. */
#define ckd_add(r, a, b) __builtin_add_overflow((a), (b), (r))
#define ckd_mul(r, a, b) __builtin_mul_overflow((a), (b), (r))
#endif

#if __has_include(<stdbit.h>)
#include <stdbit.h>
#endif
static inline uint32_t gm_popcount64(uint64_t x) {
#ifdef __STDC_VERSION_STDBIT_H__
    return (uint32_t)stdc_count_ones(x);
#else
    return (uint32_t)__builtin_popcountll((unsigned long long)x);
#endif
}

/* Fault hooks exist only in test builds. A zero countdown fails this call. */
#ifdef GM_TESTING
extern long gm_test_alloc_after, gm_test_io_after;
bool gm_test_fail(long *counter);
#endif
static inline void *gm_alloc(size_t n) {
#ifdef GM_TESTING
    if (gm_test_fail(&gm_test_alloc_after))
        return nullptr;
#endif
    return malloc(n ? n : 1);
}
static inline void *gm_resize(void *p, size_t n) {
#ifdef GM_TESTING
    if (gm_test_fail(&gm_test_alloc_after))
        return nullptr;
#endif
    return realloc(p, n);
}
static inline void *gm_zero(size_t n) {
    void *p = gm_alloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}
static inline uint32_t gm_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t gm_u64(const uint8_t *p) {
    return (uint64_t)gm_u32(p) | (uint64_t)gm_u32(p + 4) << 32;
}
static inline void gm_put32(uint8_t *p, uint32_t x) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(x >> (8 * i));
}
static inline void gm_put64(uint8_t *p, uint64_t x) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = (uint8_t)(x >> (8 * i));
}
/* Chunking policy; the model window itself is part of the embedder contract. */
enum { GM_TOKENS = 65536, GM_OVERLAP = 64 };
[[nodiscard]] enum gm_status gm_pack(size_t dim, const float *vector, uint8_t *bits);
#endif
