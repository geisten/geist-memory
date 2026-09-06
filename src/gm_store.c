#define _POSIX_C_SOURCE 200809L

#include "gm_store.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct gm_chunk_rec {
    uint32_t doc;
    uint32_t chunk;      /* ordinal within the document */
    uint32_t generation; /* must equal the doc's, else the chunk is dead */
    uint32_t reserved;   /* keeps the stride 16; not read, not written */
};

struct gm_doc_rec {
    char     path[GM_PATH_MAX];
    int64_t  mtime;
    uint64_t size;
    uint32_t generation; /* bumped on re-index; older chunks stop matching */
    uint32_t reserved;
};

/* One append target: the handle stays open for the life of the store, so an
 * append is a seek and two writes rather than an open/close pair. */
struct gm_file {
    FILE  *fh;
    size_t rec_size;
};

struct gm_store {
    size_t   dim;   /* bits */
    size_t   bytes; /* dim / 8 */
    uint64_t model_fp;

    struct gm_file vec_f, chunk_f, doc_f;

    uint8_t             *vecs;   /* n_chunks × bytes */
    struct gm_chunk_rec *chunks; /* n_chunks */
    struct gm_doc_rec   *docs;   /* n_docs */
    size_t               n_chunks, cap_chunks;
    size_t               n_docs, cap_docs;
};

static const char *const VECS_NAME   = "vectors.gm";
static const char *const CHUNKS_NAME = "chunks.gm";
static const char *const DOCS_NAME   = "docs.gm";

/* dir[] is bounded by the caller's check in gm_store_open; the longest name
 * above plus a separator is 11 bytes. */
enum { GM_DIR_MAX = 512u, GM_PATHBUF = GM_DIR_MAX + 16u };

enum gm_status gm_read_file(const char *path, uint8_t **out, size_t *out_len) {
    *out     = nullptr;
    *out_len = 0;
    FILE *f  = fopen(path, "rb");
    if (f == nullptr) {
        return errno == ENOENT ? GM_OK : GM_E_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return GM_E_IO;
    }
    const long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return GM_E_IO;
    }
    /* One byte over, always NUL: the store path ignores it and the text
     * path gets a C string without a second reader. */
    uint8_t *buf = malloc((size_t) n + 1u);
    if (buf == nullptr) {
        fclose(f);
        return GM_E_OOM;
    }
    if (n > 0 && fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        fclose(f);
        return GM_E_IO;
    }
    fclose(f);
    buf[n]   = '\0';
    *out     = buf;
    *out_len = (size_t) n;
    return GM_OK;
}

/* Validate a loaded file's header and report how many records follow. */
static enum gm_status validate_header(
        const uint8_t *buf, size_t len, size_t rec_size, size_t dim, uint64_t model_fp,
        size_t *out_count) {
    if (len < sizeof(struct gm_file_header)) {
        return GM_E_FORMAT;
    }
    struct gm_file_header h;
    memcpy(&h, buf, sizeof h);
    if (h.magic != GM_MAGIC || h.version != GM_VERSION || h.rec_size != rec_size) {
        return GM_E_FORMAT;
    }
    if (h.dim != dim || h.model_fp != model_fp) {
        return GM_E_MODEL;
    }
    if ((len - sizeof h) / rec_size < h.count) {
        return GM_E_FORMAT; /* truncated: the header promises more than exists */
    }
    *out_count = (size_t) h.count;
    return GM_OK;
}

/* Open one of the three files, creating it with a fresh header when absent,
 * and hand back its records copied into a growable array. */
static enum gm_status open_file(const char     *dir,
                                const char     *name,
                                size_t          rec_size,
                                size_t          dim,
                                uint64_t        model_fp,
                                struct gm_file *out_file,
                                void          **out_recs,
                                size_t         *out_count) {
    char path[GM_PATHBUF];
    snprintf(path, sizeof path, "%s/%s", dir, name);

    *out_recs         = nullptr;
    *out_count        = 0;
    out_file->fh      = nullptr;
    out_file->rec_size = rec_size;

    uint8_t       *buf = nullptr;
    size_t         len = 0;
    enum gm_status s   = gm_read_file(path, &buf, &len);
    if (s != GM_OK) {
        return s;
    }
    if (buf == nullptr) {
        const struct gm_file_header h = {.magic    = GM_MAGIC,
                                         .version  = GM_VERSION,
                                         .dim      = (uint32_t) dim,
                                         .rec_size = (uint32_t) rec_size,
                                         .count    = 0,
                                         .model_fp = model_fp};
        FILE                       *f = fopen(path, "w+b");
        if (f == nullptr) {
            return GM_E_IO;
        }
        if (fwrite(&h, sizeof h, 1, f) != 1 || fflush(f) != 0) {
            fclose(f);
            return GM_E_IO;
        }
        out_file->fh = f;
        return GM_OK;
    }

    s = validate_header(buf, len, rec_size, dim, model_fp, out_count);
    if (s != GM_OK) {
        free(buf);
        return s;
    }
    const size_t bytes = *out_count * rec_size;
    if (bytes > 0) {
        void *dst = malloc(bytes);
        if (dst == nullptr) {
            free(buf);
            return GM_E_OOM;
        }
        memcpy(dst, buf + sizeof(struct gm_file_header), bytes);
        *out_recs = dst;
    }
    free(buf);

    out_file->fh = fopen(path, "r+b");
    return out_file->fh != nullptr ? GM_OK : GM_E_IO;
}

/* Append `len` bytes and update the header's count in place. Both writes
 * are flushed before returning: a crash between them loses the last record,
 * which re-indexing restores, whereas a count ahead of its data would make
 * the store unreadable. */
static enum gm_status append_rec(struct gm_file *f, const void *rec, size_t len, uint64_t count) {
    bool ok = fseek(f->fh, 0, SEEK_END) == 0 && fwrite(rec, 1, len, f->fh) == len &&
              fflush(f->fh) == 0;
    if (ok) {
        ok = fseek(f->fh, (long) offsetof(struct gm_file_header, count), SEEK_SET) == 0 &&
             fwrite(&count, sizeof count, 1, f->fh) == 1 && fflush(f->fh) == 0;
    }
    return ok ? GM_OK : GM_E_IO;
}

/* Rewrite record `idx` where it already sits. */
static enum gm_status write_rec_at(struct gm_file *f, size_t idx, const void *rec) {
    const long off = (long) (sizeof(struct gm_file_header) + idx * f->rec_size);
    const bool ok  = fseek(f->fh, off, SEEK_SET) == 0 &&
                    fwrite(rec, f->rec_size, 1, f->fh) == 1 && fflush(f->fh) == 0;
    return ok ? GM_OK : GM_E_IO;
}

enum gm_status gm_store_open(const char *dir, size_t dim, uint64_t model_fp, struct gm_store **out) {
    *out = nullptr;
    if (dir == nullptr || dim == 0 || dim % 8u != 0u) {
        return GM_E_INVALID_ARG;
    }
    if (strlen(dir) >= GM_DIR_MAX) {
        return GM_E_TOO_LONG;
    }
    struct gm_store *st = calloc(1, sizeof *st);
    if (st == nullptr) {
        return GM_E_OOM;
    }
    st->dim      = dim;
    st->bytes    = dim / 8u;
    st->model_fp = model_fp;

    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        gm_store_close(st);
        return GM_E_IO;
    }

    size_t         n_vecs = 0;
    void          *vecs = nullptr, *chunks = nullptr, *docs = nullptr;
    enum gm_status s =
            open_file(dir, VECS_NAME, st->bytes, dim, model_fp, &st->vec_f, &vecs, &n_vecs);
    if (s == GM_OK) {
        s = open_file(dir, CHUNKS_NAME, sizeof(struct gm_chunk_rec), dim, model_fp, &st->chunk_f,
                      &chunks, &st->n_chunks);
    }
    if (s == GM_OK) {
        s = open_file(dir, DOCS_NAME, sizeof(struct gm_doc_rec), dim, model_fp, &st->doc_f, &docs,
                      &st->n_docs);
    }
    if (s == GM_OK && n_vecs != st->n_chunks) {
        s = GM_E_FORMAT; /* vectors and records disagree */
    }
    if (s != GM_OK) {
        free(vecs);
        free(chunks);
        free(docs);
        gm_store_close(st);
        return s;
    }
    st->vecs       = vecs;
    st->chunks     = chunks;
    st->docs       = docs;
    st->cap_chunks = st->n_chunks;
    st->cap_docs   = st->n_docs;
    *out           = st;
    return GM_OK;
}

void gm_store_close(struct gm_store *st) {
    if (st == nullptr) {
        return;
    }
    struct gm_file *files[] = {&st->vec_f, &st->chunk_f, &st->doc_f};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        if (files[i]->fh != nullptr) {
            fclose(files[i]->fh);
        }
    }
    free(st->vecs);
    free(st->chunks);
    free(st->docs);
    free(st);
}

size_t gm_store_dim(const struct gm_store *st) {
    return st->dim;
}

size_t gm_store_bytes(const struct gm_store *st) {
    return st->bytes;
}

bool gm_store_path_too_long(const char *path) {
    return strlen(path) >= GM_PATH_MAX;
}

const char *gm_store_doc_path(const struct gm_store *st, uint32_t doc) {
    return doc < st->n_docs ? st->docs[doc].path : nullptr;
}

/* A chunk counts only while its document has not been re-indexed under it.
 * The predicate lives here, next to the two arrays it joins, so no caller
 * can forget it and read a dead vector as a real hit. */
static bool chunk_live(const struct gm_store *st, size_t i) {
    const struct gm_chunk_rec *rec = &st->chunks[i];
    return rec->generation == st->docs[rec->doc].generation;
}

size_t gm_store_live_chunks(const struct gm_store *st) {
    size_t n = 0;
    for (size_t i = 0; i < st->n_chunks; i++) {
        n += chunk_live(st, i) ? 1u : 0u;
    }
    return n;
}

static size_t find_doc(const struct gm_store *st, const char *path) {
    /* ponytail: linear strcmp. O(n_docs) per document, so indexing a tree is
     * quadratic in the number of files — measured 4.9 ms at 1k docs, 615 ms
     * at 20k. Add a path hash map when a tree pass is visibly slow; one
     * lookup per document (not two) is what makes that threshold reachable. */
    for (size_t i = 0; i < st->n_docs; i++) {
        if (strcmp(st->docs[i].path, path) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t next_cap(size_t cap, size_t need) {
    size_t next = cap ? cap * 2u : 64u;
    while (next < need) {
        next *= 2u;
    }
    return next;
}

/* The vector array and the chunk-record array are indexed in lockstep, so
 * they share one capacity and grow together, and the capacity advances only
 * once both reallocs have succeeded. Two separate grows against two
 * capacities is exactly the desync this shape prevents. */
[[nodiscard]] static enum gm_status reserve_chunks(struct gm_store *st, size_t need) {
    if (need <= st->cap_chunks) {
        return GM_OK;
    }
    const size_t next = next_cap(st->cap_chunks, need);
    uint8_t     *v    = realloc(st->vecs, next * st->bytes);
    if (v == nullptr) {
        return GM_E_OOM;
    }
    st->vecs               = v;
    struct gm_chunk_rec *c = realloc(st->chunks, next * sizeof *c);
    if (c == nullptr) {
        return GM_E_OOM; /* vecs keeps the larger block; cap_chunks is not
                          * advanced, so the next attempt retries both. */
    }
    st->chunks     = c;
    st->cap_chunks = next;
    return GM_OK;
}

enum gm_status gm_store_put_doc(struct gm_store   *st,
                                const char        *path,
                                int64_t            mtime,
                                uint64_t           size,
                                size_t            *out_doc,
                                enum gm_doc_state *out_state) {
    if (gm_store_path_too_long(path)) {
        return GM_E_TOO_LONG;
    }
    const size_t found = find_doc(st, path);
    if (found != SIZE_MAX) {
        struct gm_doc_rec *d = &st->docs[found];
        *out_doc             = found;
        if (d->mtime == mtime && d->size == size) {
            *out_state = GM_DOC_UNCHANGED;
            return GM_OK;
        }
        /* Re-index: bump the generation so the old chunks stop matching, and
         * rewrite the record where it sits. ponytail: their vectors stay on
         * disk as dead weight. Compact when a store is visibly bloated — a
         * rewrite of three append-only files, not a migration. */
        d->generation++;
        d->mtime   = mtime;
        d->size    = size;
        *out_state = GM_DOC_REINDEXED;
        return write_rec_at(&st->doc_f, found, d);
    }

    if (st->n_docs + 1u > st->cap_docs) {
        const size_t       next = next_cap(st->cap_docs, st->n_docs + 1u);
        struct gm_doc_rec *d    = realloc(st->docs, next * sizeof *d);
        if (d == nullptr) {
            return GM_E_OOM;
        }
        st->docs     = d;
        st->cap_docs = next;
    }
    struct gm_doc_rec *d = &st->docs[st->n_docs];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    d->mtime      = mtime;
    d->size       = size;
    d->generation = 1;

    const enum gm_status s = append_rec(&st->doc_f, d, sizeof *d, (uint64_t) (st->n_docs + 1u));
    if (s != GM_OK) {
        return s;
    }
    *out_doc   = st->n_docs++;
    *out_state = GM_DOC_NEW;
    return GM_OK;
}

enum gm_status
gm_store_put_chunk(struct gm_store *st, size_t doc, uint32_t chunk, const uint8_t *bits) {
    if (doc >= st->n_docs) {
        return GM_E_INVALID_ARG;
    }
    enum gm_status s = reserve_chunks(st, st->n_chunks + 1u);
    if (s != GM_OK) {
        return s;
    }
    const struct gm_chunk_rec rec = {.doc        = (uint32_t) doc,
                                     .chunk      = chunk,
                                     .generation = st->docs[doc].generation};

    const uint64_t next = (uint64_t) (st->n_chunks + 1u);
    s                   = append_rec(&st->vec_f, bits, st->bytes, next);
    if (s == GM_OK) {
        s = append_rec(&st->chunk_f, &rec, sizeof rec, next);
    }
    if (s != GM_OK) {
        return s;
    }
    memcpy(st->vecs + st->n_chunks * st->bytes, bits, st->bytes);
    st->chunks[st->n_chunks] = rec;
    st->n_chunks++;
    return GM_OK;
}

enum gm_status gm_store_scan(struct gm_store *st,
                             size_t           k,
                             const uint8_t   *query_bits,
                             struct gm_hit    out[static k],
                             size_t          *n_out) {
    *n_out = 0;
    if (k == 0 || query_bits == nullptr) {
        return GM_E_INVALID_ARG;
    }
    /* ponytail: brute-force popcount scan, no index. 1024-bit vectors are
     * 128 B, so 100k chunks is 12.8 MB streamed once — about 1 ms, and it is
     * exact. Reach for an ANN index when a scan measurably exceeds 100 ms. */
    const uint64_t *q     = (const uint64_t *) (const void *) query_bits;
    const size_t    words = st->bytes / 8u;

    size_t n = 0;
    for (size_t i = 0; i < st->n_chunks; i++) {
        if (!chunk_live(st, i)) {
            continue;
        }
        const uint64_t *v = (const uint64_t *) (const void *) (st->vecs + i * st->bytes);
        uint32_t        d = 0;
        for (size_t w = 0; w < words; w++) {
            d += (uint32_t) __builtin_popcountll(q[w] ^ v[w]);
        }
        /* Insertion into the top-k. k is a handful, so this beats sorting
         * the whole store and allocates nothing. */
        if (n < k || d < out[n - 1].distance) {
            size_t pos = (n < k) ? n : k - 1;
            while (pos > 0 && out[pos - 1].distance > d) {
                out[pos] = out[pos - 1];
                pos--;
            }
            out[pos] = (struct gm_hit) {
                    .doc = st->chunks[i].doc, .chunk = st->chunks[i].chunk, .distance = d};
            if (n < k) {
                n++;
            }
        }
    }
    *n_out = n;
    return GM_OK;
}
