/*
 * gm_store.h — the on-disk store. Three fixed-stride files, no parser.
 *
 *   vectors.gm   header + count × (dim/8) bytes, one packed sign-bit vector
 *                per chunk. Index i is at header_size + i * stride.
 *   chunks.gm    header + count × struct gm_chunk_rec
 *   docs.gm      header + count × struct gm_doc_rec
 *
 * Fixed strides mean the array index IS the file offset: no index file, no
 * parser, no format version to negotiate beyond the magic. Everything is
 * held in memory and appended through to disk; the store is sized for the
 * hundreds of thousands, not the billions.
 *
 * ponytail: whole store resident. 1024-bit vectors = 128 B/chunk, so 100k
 * chunks is 12.8 MB of vectors plus 3.2 MB of records. Move to mmap when a
 * store stops fitting in the RAM you want to spend.
 */
#ifndef GM_STORE_H
#define GM_STORE_H

#include "geist_memory.h"

#include <stdint.h>

#define GM_MAGIC   0x314D47EDu /* "\xedGM1" */
#define GM_VERSION 1u

/* Head of every one of the three files. `count` is the authority on how
 * many records follow; a file longer than that is truncated garbage from an
 * interrupted append and is rejected, not tolerated. */
struct gm_file_header {
    uint32_t magic;
    uint32_t version;
    uint32_t dim;      /* embedding width in bits */
    uint32_t rec_size; /* stride of the records that follow */
    uint64_t count;
    uint64_t model_fp; /* the model this store belongs to */
};

struct gm_chunk_rec {
    uint32_t doc;
    uint32_t chunk;      /* ordinal within the document */
    uint32_t generation; /* must equal the doc's, else the chunk is dead */
    uint32_t n_tokens;
};

struct gm_doc_rec {
    char     path[GM_PATH_MAX];
    int64_t  mtime;
    uint64_t size;
    uint32_t generation; /* bumped on re-index; older chunks stop matching */
    uint32_t n_chunks;
};

struct gm_store {
    char    dir[512];
    size_t  dim;   /* bits */
    size_t  bytes; /* dim / 8 */
    uint64_t model_fp;

    uint8_t             *vecs;   /* n_chunks × bytes */
    struct gm_chunk_rec *chunks; /* n_chunks */
    struct gm_doc_rec   *docs;   /* n_docs */
    size_t               n_chunks, cap_chunks;
    size_t               n_docs, cap_docs;
};

/* Open `dir` (creating it and the three files if absent) for a model of
 * `dim` bits and fingerprint `model_fp`. An existing store built for a
 * different model or width is refused with GM_E_MODEL. */
[[nodiscard]] enum gm_status gm_store_open(struct gm_store *st,
                                           const char      *dir,
                                           size_t           dim,
                                           uint64_t         model_fp);

void gm_store_close(struct gm_store *st);

/* Find a document by path/id. Returns its index, or SIZE_MAX. */
size_t gm_store_find_doc(const struct gm_store *st, const char *path);

/* Append a document record (or bump the generation of an existing one) and
 * write it through. Returns the doc index in *out_doc. */
[[nodiscard]] enum gm_status gm_store_put_doc(struct gm_store *st,
                                              const char      *path,
                                              int64_t          mtime,
                                              uint64_t         size,
                                              size_t          *out_doc);

/* Append one chunk: `bits` is `st->bytes` bytes of packed sign bits. */
[[nodiscard]] enum gm_status gm_store_put_chunk(struct gm_store *st,
                                                size_t           doc,
                                                uint32_t         chunk,
                                                uint32_t         n_tokens,
                                                const uint8_t   *bits);

#endif /* GM_STORE_H */
