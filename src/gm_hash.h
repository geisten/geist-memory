#ifndef GM_HASH_H
#define GM_HASH_H
#include <stddef.h>
#include <stdint.h>
struct gm_hash {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
};
void gm_hash_init(struct gm_hash *s);
void gm_hash_update(struct gm_hash *s, size_t n, const void *data);
void gm_hash_final(struct gm_hash *s, uint8_t out[32]);
void gm_digest(size_t n, const void *data, uint8_t out[32]);
#endif
