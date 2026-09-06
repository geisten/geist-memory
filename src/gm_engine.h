#ifndef GM_ENGINE_H
#define GM_ENGINE_H
#include "geist_memory.h"
/* This is the entire engine adapter; tests replace this implementation. */
enum { GM_TOKENS = 65536, GM_WINDOW = 256, GM_OVERLAP = 64 };
struct gm_engine;
[[nodiscard]] enum gm_status gm_engine_open(const char *path, bool omit_bos, bool omit_eos,
                                            struct gm_engine **out, size_t *dim);
void gm_engine_close(struct gm_engine *e);
[[nodiscard]] enum gm_status gm_engine_tokenize(struct gm_engine *e, const char *text,
                                                size_t capacity, int32_t *ids, size_t *n);
[[nodiscard]] enum gm_status gm_engine_embed(struct gm_engine *e, size_t n, const int32_t *ids,
                                             size_t dim, const float **out);
[[nodiscard]] enum gm_status gm_pack(size_t dim, const float *vector, uint8_t *bits);
#endif
