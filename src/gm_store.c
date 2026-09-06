#define _POSIX_C_SOURCE 200809L

#include "gm_store.h"

#include "base/checked.h"
#include "base/heap.h"

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

    /* Path -> document index. Open addressing, linear probing, power-of-two
     * capacity held at or above 2x n_docs so the table stays under half
     * full. A slot holds a document index; GM_SLOT_EMPTY marks it free and
     * the path is compared against docs[idx].path on a hit, so no hash is
     * stored beside it.
     *
     * It replaces a linear strcmp scan that ran once per indexed document:
     * 4.9 ms over 1k documents, 615 ms over 20k -- quadratic in a tree pass,
     * against a README that calls re-running over a tree cheap. Rebuilt from
     * docs[] on growth and at open, never persisted: a derived index on disk
     * is a second thing that can be wrong. */
    uint32_t *index;
    size_t    index_cap; /* power of two, or 0 while empty */
};

#define GM_SLOT_EMPTY UINT32_MAX

static const char *const VECS_NAME   = "vectors.gm";
static const char *const CHUNKS_NAME = "chunks.gm";
static const char *const DOCS_NAME   = "docs.gm";

/* dir[] is bounded by the caller's check in gm_store_open; the longest name
 * above plus a separator is 11 bytes. */
enum { GM_DIR_MAX = 512u, GM_PATHBUF = GM_DIR_MAX + 16u };

uint64_t gm_fnv1a(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *) data;
    uint64_t       h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

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

/* Defined below with the rest of the path index; gm_store_open builds the
 * table once the document array is in place. */
[[nodiscard]] static enum gm_status index_reserve(struct gm_store *st, size_t n_docs);

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
    /* h.count comes off disk, so this product is untrusted input times a
     * size (AGENT.md §3). A wrap here would under-allocate and then copy the
     * full length into it. */
    size_t need = 0;
    if (ckd_mul(&need, (size_t) h.count, rec_size) || need > len - sizeof h) {
        return GM_E_FORMAT; /* truncated, or a count no allocation could hold */
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
    /* validate_header already proved this product does not overflow. */
    const size_t bytes = *out_count * rec_size;
    if (bytes > 0) {
        void *dst = heap_alloc_aligned(bytes, OPTIMAL_ALIGNMENT);
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
    s              = index_reserve(st, st->n_docs);
    if (s != GM_OK) {
        gm_store_close(st);
        return s;
    }
    *out = st;
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
    void *v = st->vecs, *c = st->chunks, *d = st->docs, *ix = st->index;
    safe_free(&v);
    safe_free(&c);
    safe_free(&d);
    safe_free(&ix);
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

/* Probe sequence for `path`: the slot holding it, or the first free slot if
 * it is absent. One walk answers both "where is it" and "where does it go",
 * which is why lookup and insert share it. */
static size_t index_slot(const struct gm_store *st, const char *path, uint64_t h) {
    const size_t mask = st->index_cap - 1u;
    size_t       i    = (size_t) h & mask;
    while (st->index[i] != GM_SLOT_EMPTY) {
        if (strcmp(st->docs[st->index[i]].path, path) == 0) {
            return i;
        }
        i = (i + 1u) & mask;
    }
    return i;
}

/* Rebuild at `cap` slots (a power of two) from docs[]. Rebuilding rather
 * than migrating keeps one insertion path, and the table is small: four
 * bytes a slot, so 20k documents is 256 KB. */
[[nodiscard]] static enum gm_status index_rebuild(struct gm_store *st, size_t cap) {
    size_t bytes = 0;
    if (ckd_mul(&bytes, cap, sizeof *st->index)) {
        return GM_E_OOM;
    }
    uint32_t *table = heap_alloc_aligned(bytes, OPTIMAL_ALIGNMENT);
    if (table == nullptr) {
        return GM_E_OOM;
    }
    memset(table, 0xff, bytes); /* GM_SLOT_EMPTY is all-ones */

    void *old = st->index;
    safe_free(&old);
    st->index     = table;
    st->index_cap = cap;

    for (size_t i = 0; i < st->n_docs; i++) {
        const char    *p = st->docs[i].path;
        const uint64_t h = gm_fnv1a(p, strlen(p));
        st->index[index_slot(st, p, h)] = (uint32_t) i;
    }
    return GM_OK;
}

/* Hold the table at or above 2x the document count so probes stay short. */
[[nodiscard]] static enum gm_status index_reserve(struct gm_store *st, size_t n_docs) {
    size_t want = 64u;
    while (want < n_docs * 2u) {
        want *= 2u;
    }
    return want > st->index_cap ? index_rebuild(st, want) : GM_OK;
}

static size_t find_doc(const struct gm_store *st, const char *path) {
    if (st->index_cap == 0u) {
        return SIZE_MAX;
    }
    const size_t slot = index_slot(st, path, gm_fnv1a(path, strlen(path)));
    return st->index[slot] == GM_SLOT_EMPTY ? SIZE_MAX : (size_t) st->index[slot];
}

/* Grow a heap.h-allocated block. realloc is NOT an option on one: heap.h
 * hands back over-aligned memory and has no realloc of its own -- AGENT.md
 * §3 calls that a gap, not a licence to mix the two allocators. Growth is
 * allocate-copy-free, so ownership stays uniform across every array in the
 * store, which is worth more than the one avoided copy. */
[[nodiscard]] static enum gm_status
grow_block(void **p, size_t used_bytes, size_t want_bytes) {
    void *fresh = heap_alloc_aligned(want_bytes, OPTIMAL_ALIGNMENT);
    if (fresh == nullptr) {
        return GM_E_OOM;
    }
    if (used_bytes > 0) {
        memcpy(fresh, *p, used_bytes);
    }
    safe_free(p);
    *p = fresh;
    return GM_OK;
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
    const size_t next     = next_cap(st->cap_chunks, need);
    size_t       vec_bytes = 0, rec_bytes = 0;
    if (ckd_mul(&vec_bytes, next, st->bytes) ||
        ckd_mul(&rec_bytes, next, sizeof(struct gm_chunk_rec))) {
        return GM_E_OOM;
    }
    enum gm_status s = grow_block((void **) &st->vecs, st->n_chunks * st->bytes, vec_bytes);
    if (s != GM_OK) {
        return s;
    }
    s = grow_block((void **) &st->chunks, st->n_chunks * sizeof(struct gm_chunk_rec), rec_bytes);
    if (s != GM_OK) {
        return s; /* vecs keeps the larger block; cap_chunks is not advanced,
                   * so the next attempt retries both. */
    }
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
        const size_t next  = next_cap(st->cap_docs, st->n_docs + 1u);
        size_t       bytes = 0;
        if (ckd_mul(&bytes, next, sizeof *st->docs)) {
            return GM_E_OOM;
        }
        const enum gm_status g =
                grow_block((void **) &st->docs, st->n_docs * sizeof *st->docs, bytes);
        if (g != GM_OK) {
            return g;
        }
        st->cap_docs = next;
    }
    /* Grow the index BEFORE the record exists: a rebuild walks docs[], so it
     * must not see a half-written entry. */
    const enum gm_status ir = index_reserve(st, st->n_docs + 1u);
    if (ir != GM_OK) {
        return ir;
    }
    struct gm_doc_rec *d = &st->docs[st->n_docs];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    d->mtime      = mtime;
    d->size       = size;
    d->generation = 1;

    const enum gm_status s = append_rec(&st->doc_f, d, sizeof *d, (uint64_t) (st->n_docs + 1u));
    if (s != GM_OK) {
        return s; /* the record is not counted, so the index stays consistent */
    }
    st->index[index_slot(st, d->path, gm_fnv1a(d->path, strlen(d->path)))] = (uint32_t) st->n_docs;
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
