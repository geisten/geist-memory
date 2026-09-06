/* Private three-file store, validation, transaction recovery and exact Hamming search.
 * All vectors remain resident until explicit compaction. See docs/FORMAT.md. */
#ifndef GM_STORE_H
#define GM_STORE_H

#include "geist_memory.h"

#include <stdint.h>

#include "gm_format.h"

/* Opaque: everything that walks the records lives in gm_store.c. */
struct gm_store;

/* Open `dir` (creating it and the three files if absent) for a model of
 * `dim` bits and fingerprint `model_fp`. An existing store built for a
 * different model or width is refused with GM_E_MODEL. */
[[nodiscard]] enum gm_status gm_store_open(const char *dir, size_t dim, uint64_t model_fp,
                                           struct gm_store **out);

void gm_store_close(struct gm_store *st);

/* Chunks whose document generation still matches — what a scan will
 * consider. Dead chunks from re-indexed documents are excluded. */
size_t gm_store_live_chunks(const struct gm_store *st);

/* The path or id a document index refers to, or nullptr when out of range.
 * Owned by the store. */
const char *gm_store_doc_path(const struct gm_store *st, uint32_t doc);

/* True when `path` is longer than a document record can hold. Checked
 * before any work: the limit is the store's, so the test is too. */
bool gm_store_path_too_long(const char *path);

/* Prepare all chunks before calling this operation. On allocation failure,
 * existing contents remain visible. GM_E_UNCERTAIN poisons the handle. */
[[nodiscard]] enum gm_status gm_store_replace(struct gm_store *st, const char *path, size_t count,
                                              const uint8_t *bits);
[[nodiscard]] enum gm_status gm_store_stats(const struct gm_store *st, struct gm_stats *out);
[[nodiscard]] enum gm_status gm_store_open_limited(const char *dir, size_t dim, uint64_t model_fp,
                                                   size_t budget, struct gm_store **out);

/* Nearest `k` live chunks to `query_bits` by Hamming distance, best first.
 * Writes the count to *n_out, which is less than k when the store holds
 * fewer live chunks.
 *
 * The scan lives here because it is the only thing that needs the layout —
 * the stride arithmetic and the liveness join are the store's business, and
 * a caller that re-derived them could read a dead vector as a real hit. */
[[nodiscard]] enum gm_status gm_store_scan(struct gm_store *st, size_t k, const uint8_t *query_bits,
                                           struct gm_hit *out, size_t *n_out);

[[nodiscard]] enum gm_status gm_store_compact(struct gm_store *st);

#endif /* GM_STORE_H */
