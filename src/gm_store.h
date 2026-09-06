/*
 * gm_store.h — the on-disk store. Three fixed-stride files, no parser.
 *
 *   vectors.gm   header + count × (dim/8) bytes, one packed sign-bit vector
 *                per chunk. Index i is at header_size + i * stride.
 *   chunks.gm    header + count × struct gm_chunk_rec
 *   docs.gm      header + count × struct gm_doc_rec
 *
 * Fixed strides mean the array index IS the file offset: no index file, no
 * parser, no format version to negotiate beyond the magic.
 *
 * The representation is PRIVATE. Everything that walks it — the stride
 * arithmetic, the liveness join, the top-k scan — lives in gm_store.c, so
 * the format can change without touching a line of gm.c.
 *
 * ponytail: whole store resident. 1024-bit vectors = 128 B/chunk, so 100k
 * chunks is 12.8 MB of vectors plus 1.6 MB of records. Move to mmap when a
 * store stops fitting in the RAM you want to spend.
 */
#ifndef GM_STORE_H
#define GM_STORE_H

#include "geist_memory.h"

#include <stdint.h>

/* The identity block every one of the three files starts with. Public
 * because it IS the format -- a reader outside this library can tell one of
 * our stores from anything else, and which model it belongs to, from these
 * 32 bytes alone. The record layouts behind it, and the in-memory store,
 * stay private. */
#define GM_MAGIC   0x314D47EDu /* "\xedGM1" */
#define GM_VERSION 1u

struct gm_file_header {
    uint32_t magic;
    uint32_t version;
    uint32_t dim;      /* embedding width in bits */
    uint32_t rec_size; /* stride of the records that follow */
    uint64_t count;
    uint64_t model_fp; /* the model this store belongs to */
};

/* Opaque: everything that walks the records lives in gm_store.c. */
struct gm_store;

/* What gm_store_put_doc did, so the caller need not look the document up
 * first. Deciding this from the store's one lookup is what keeps "is this
 * document new, unchanged or changed?" a single question with a single
 * answer. */
enum gm_doc_state {
    GM_DOC_NEW = 0,
    GM_DOC_UNCHANGED, /* same mtime and size — nothing to re-index */
    GM_DOC_REINDEXED, /* generation bumped; the old chunks are now dead */
};

/* Open `dir` (creating it and the three files if absent) for a model of
 * `dim` bits and fingerprint `model_fp`. An existing store built for a
 * different model or width is refused with GM_E_MODEL. */
[[nodiscard]] enum gm_status gm_store_open(const char       *dir,
                                           size_t            dim,
                                           uint64_t          model_fp,
                                           struct gm_store **out);

void gm_store_close(struct gm_store *st);

size_t gm_store_dim(const struct gm_store *st);   /* bits */
size_t gm_store_bytes(const struct gm_store *st); /* dim / 8 */

/* Chunks whose document generation still matches — what a scan will
 * consider. Dead chunks from re-indexed documents are excluded. */
size_t gm_store_live_chunks(const struct gm_store *st);

/* The path or id a document index refers to, or nullptr when out of range.
 * Owned by the store. */
const char *gm_store_doc_path(const struct gm_store *st, uint32_t doc);

/* True when `path` is longer than a document record can hold. Checked
 * before any work: the limit is the store's, so the test is too. */
bool gm_store_path_too_long(const char *path);

/* Record a document and say what that meant. On GM_DOC_UNCHANGED the caller
 * should not re-index; on GM_DOC_REINDEXED the previous chunks are already
 * dead. Writes the document index to *out_doc in every non-error case. */
[[nodiscard]] enum gm_status gm_store_put_doc(struct gm_store   *st,
                                              const char        *path,
                                              int64_t            mtime,
                                              uint64_t           size,
                                              size_t            *out_doc,
                                              enum gm_doc_state *out_state);

/* Append one chunk: `bits` is gm_store_bytes(st) bytes of packed signs. */
[[nodiscard]] enum gm_status gm_store_put_chunk(struct gm_store *st,
                                                size_t           doc,
                                                uint32_t         chunk,
                                                const uint8_t   *bits);

/* Nearest `k` live chunks to `query_bits` by Hamming distance, best first.
 * Writes the count to *n_out, which is less than k when the store holds
 * fewer live chunks.
 *
 * The scan lives here because it is the only thing that needs the layout —
 * the stride arithmetic and the liveness join are the store's business, and
 * a caller that re-derived them could read a dead vector as a real hit. */
[[nodiscard]] enum gm_status gm_store_scan(struct gm_store *st,
                                           size_t           k,
                                           const uint8_t   *query_bits,
                                           struct gm_hit    out[static k],
                                           size_t          *n_out);

/* Read a whole file into a NUL-terminated heap buffer. `*out_len` excludes
 * the terminator. A missing file yields nullptr, length 0 and GM_OK — which
 * is how gm_store_open tells "new store" from "broken store". Shared so the
 * library has one policy for reading a file, not two. */
[[nodiscard]] enum gm_status gm_read_file(const char *path, uint8_t **out, size_t *out_len);

#endif /* GM_STORE_H */
