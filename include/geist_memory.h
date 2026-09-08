/* geist-memory: single-threaded local semantic memory. C23, experimental API.
 * One handle per directory; a second writer is rejected. Paths are literal
 * (no shell expansion). See docs/FORMAT.md for durability and recovery. */
#ifndef GEIST_MEMORY_H
#define GEIST_MEMORY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum gm_status {
    GM_OK = 0,
    GM_E_INVALID_ARG,
    GM_E_IO,
    GM_E_OOM,
    GM_E_FORMAT,
    GM_E_MODEL,
    GM_E_TOO_LONG,
    GM_E_ENGINE,
    GM_E_BUSY,      /* another handle owns the store */
    GM_E_LIMIT,     /* configured store memory budget exceeded */
    GM_E_UNCERTAIN, /* write/sync failed: close and reopen before any operation */
};
#define GM_PATH_MAX 232u
#define GM_TEXT_MAX 262144u
#define GM_QUERY_MAX 4096u
#define GM_PREFIX_MAX 127u
#define GM_DIM_MAX 65536u

struct gm;
struct gm_opts {
    const char *query_prefix; /* query only; nullptr = empty; at most
                                 GM_PREFIX_MAX bytes */
    size_t max_store_bytes;   /* 0 = 256 MiB; docs + all chunks/vectors and
                               * conservative growth peak. Excludes engine, fixed
                               * scratch, staged embeddings and caller buffers. */
    bool omit_bos;            /* default: wrap with model BOS/EOS when available */
    bool omit_eos;            /* policy is recorded in the store identity */
};
const char *gm_status_str(enum gm_status s);
/* *out is nullptr on failure. Parent directory must already exist. */
[[nodiscard]] enum gm_status gm_open(const char *dir, const char *model_path,
                                     const struct gm_opts *opts, struct gm **out);
void gm_close(struct gm *m); /* accepts nullptr */
/* Embedding/allocation failures preserve old content. GM_E_UNCERTAIN poisons
 * the handle until recovery on reopen. Completed replacements are synced and
 * recoverable as a complete old or new state after interruption. Identical vectors
 * are a no-op; empty text removes the document's live chunks. Original text is not stored. */
[[nodiscard]] enum gm_status gm_remember_file(struct gm *m, const char *path);
[[nodiscard]] enum gm_status gm_remember_text(struct gm *m, const char *id, const char *text);
/* Explicit byte length, no embedded NUL bytes. text may be nullptr only if
 * len=0. */
[[nodiscard]] enum gm_status gm_remember_text_n(struct gm *m, const char *id, size_t len,
                                                const char *text);
struct gm_hit {
    uint32_t doc, chunk, distance;
};
/* out has capacity k; k must be positive. *n_out=0 on error. Results are sorted
 * by distance; ties retain physical insertion order. */
[[nodiscard]] enum gm_status gm_recall(struct gm *m, size_t k, const char *query,
                                       struct gm_hit *out, size_t *n_out);
/* Borrowed until the next mutating call or gm_close. nullptr for invalid doc.
 */
const char *gm_doc_path(const struct gm *m, uint32_t doc);
/* On error, out[0]=0 when out and capacity permit. */
[[nodiscard]] enum gm_status gm_doc_path_copy(const struct gm *m, uint32_t doc, size_t capacity,
                                              char *out);
size_t gm_chunk_count(const struct gm *m); /* live chunks */
size_t gm_dim(const struct gm *m);
struct gm_stats {
    size_t documents, live_chunks, memory_bytes;
    uint64_t disk_bytes, obsolete_chunks; /* three data files, excluding recovery files */
};
[[nodiscard]] enum gm_status gm_get_stats(const struct gm *m, struct gm_stats *out);
[[nodiscard]] enum gm_status gm_compact(struct gm *m);
/* Explicit format-only import. destination must not exist. Source data files
 * remain unchanged; a source lock file may be created. Pending v1 recovery is
 * refused. Fingerprint is preserved, including its historical weaknesses.
 * A failed import can leave the new destination empty or requiring recovery.
 * max_store_bytes=0 uses the default budget; no model or original text needed. */
[[nodiscard]] enum gm_status gm_import_v1(const char *source, const char *destination,
                                          size_t max_store_bytes);
#ifdef __cplusplus
}
#endif
#endif
