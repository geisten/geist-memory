#ifndef GEIST_MEMORY_EMBEDDER_H
#define GEIST_MEMORY_EMBEDDER_H
/* Embedder contract. geist-memory calls these four functions and nothing else
 * of an engine; exactly one implementation is linked per binary. The bundled
 * adapter is libgeist_memory_geist.a; a consumer may link its own instead.
 *
 * The link-time contract cannot be checked by the compiler, so the rules the
 * core relies on are stated here:
 *
 *  - open: `path` is the model_path handed to gm_open. The core has already
 *    read and hashed that file for the store's model fingerprint, so an
 *    implementation must accept a readable file path. `omit_bos`/`omit_eos`
 *    tell the implementation whether to frame token windows with BOS/EOS;
 *    the core never adds framing tokens itself. On success `*dim` is the
 *    embedding width: nonzero, a multiple of 8, at most GM_DIM_MAX.
 *    On failure `*out` is nullptr and nothing is leaked.
 *  - tokenize: writes at most `capacity` ids. The core passes limit+1 and
 *    treats limit+1 written ids as too long, so an implementation may either
 *    return GM_E_TOO_LONG or silently truncate at `capacity`.
 *  - embed: `n` is 1..GM_WINDOW. `*out` points to `dim` floats owned by the
 *    embedder and valid until the next call on the same embedder.
 *  - close: accepts nullptr.
 *  - One embedder belongs to one struct gm; no thread safety is required.
 */
#include "geist_memory.h"
enum { GM_WINDOW = 256 };
struct gm_embedder;
[[nodiscard]] enum gm_status gm_embedder_open(const char *path, bool omit_bos, bool omit_eos,
                                              struct gm_embedder **out, size_t *dim);
void gm_embedder_close(struct gm_embedder *e);
[[nodiscard]] enum gm_status gm_embedder_tokenize(struct gm_embedder *e, const char *text,
                                                  size_t capacity, int32_t *ids, size_t *n);
[[nodiscard]] enum gm_status gm_embedder_embed(struct gm_embedder *e, size_t n, const int32_t *ids,
                                               size_t dim, const float **out);
#endif
