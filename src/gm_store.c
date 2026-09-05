#define _POSIX_C_SOURCE 200809L

#include "gm_store.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The three files, in the order gm_store_open touches them. */
enum { GM_F_VECS = 0, GM_F_CHUNKS, GM_F_DOCS, GM_F_COUNT };
static const char *const GM_FILES[GM_F_COUNT] = {"vectors.gm", "chunks.gm", "docs.gm"};

static void join(char out[static 640], const char *dir, const char *name) {
    snprintf(out, 640, "%s/%s", dir, name);
}

/* Read a whole file into `*out` (malloc'd) and its length into `*out_len`.
 * A missing file is not an error: it yields a null pointer and length 0,
 * which is how gm_store_open tells "new store" from "broken store". */
static enum gm_status slurp(const char *path, uint8_t **out, size_t *out_len) {
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
    uint8_t *buf = (n > 0) ? malloc((size_t) n) : nullptr;
    if (n > 0 && buf == nullptr) {
        fclose(f);
        return GM_E_OOM;
    }
    if (n > 0 && fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        fclose(f);
        return GM_E_IO;
    }
    fclose(f);
    *out     = buf;
    *out_len = (size_t) n;
    return GM_OK;
}

/* Append `len` bytes and update the header's count in place. Both writes
 * are flushed before returning: a crash between them loses the last chunk,
 * which re-indexing restores, whereas a count ahead of its data would make
 * the store unreadable. */
static enum gm_status append_rec(const char *path, const void *rec, size_t len, uint64_t new_count) {
    FILE *f = fopen(path, "r+b");
    if (f == nullptr) {
        return GM_E_IO;
    }
    int ok = fseek(f, 0, SEEK_END) == 0 && fwrite(rec, 1, len, f) == len && fflush(f) == 0;
    if (ok) {
        ok = fseek(f, (long) offsetof(struct gm_file_header, count), SEEK_SET) == 0 &&
             fwrite(&new_count, sizeof new_count, 1, f) == 1;
    }
    if (fclose(f) != 0) {
        ok = 0;
    }
    return ok ? GM_OK : GM_E_IO;
}

static enum gm_status create_file(const char *path, const struct gm_file_header *h) {
    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        return GM_E_IO;
    }
    const int ok = fwrite(h, sizeof *h, 1, f) == 1;
    return (fclose(f) == 0 && ok) ? GM_OK : GM_E_IO;
}

/* Validate a loaded file and hand back a pointer to its record area. */
static enum gm_status take(const uint8_t                *buf,
                           size_t                        len,
                           size_t                        rec_size,
                           size_t                        dim,
                           uint64_t                      model_fp,
                           const uint8_t               **out_recs,
                           size_t                       *out_count) {
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
    if (len - sizeof h < h.count * rec_size) {
        return GM_E_FORMAT; /* truncated: the header promises more than exists */
    }
    *out_recs  = buf + sizeof h;
    *out_count = (size_t) h.count;
    return GM_OK;
}

enum gm_status
gm_store_open(struct gm_store *st, const char *dir, size_t dim, uint64_t model_fp) {
    if (st == nullptr || dir == nullptr || dim == 0 || dim % 8u != 0u) {
        return GM_E_INVALID_ARG;
    }
    if (strlen(dir) >= sizeof st->dir) {
        return GM_E_TOO_LONG;
    }
    memset(st, 0, sizeof *st);
    snprintf(st->dir, sizeof st->dir, "%s", dir);
    st->dim      = dim;
    st->bytes    = dim / 8u;
    st->model_fp = model_fp;

    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        return GM_E_IO;
    }

    const size_t rec_size[GM_F_COUNT] = {
            st->bytes, sizeof(struct gm_chunk_rec), sizeof(struct gm_doc_rec)};

    for (int i = 0; i < GM_F_COUNT; i++) {
        char path[640];
        join(path, dir, GM_FILES[i]);

        uint8_t       *buf = nullptr;
        size_t         len = 0;
        enum gm_status s   = slurp(path, &buf, &len);
        if (s != GM_OK) {
            gm_store_close(st);
            return s;
        }
        if (buf == nullptr) {
            const struct gm_file_header h = {.magic    = GM_MAGIC,
                                             .version  = GM_VERSION,
                                             .dim      = (uint32_t) dim,
                                             .rec_size = (uint32_t) rec_size[i],
                                             .count    = 0,
                                             .model_fp = model_fp};
            s                             = create_file(path, &h);
            if (s != GM_OK) {
                gm_store_close(st);
                return s;
            }
            continue;
        }
        const uint8_t *recs  = nullptr;
        size_t         count = 0;
        s = take(buf, len, rec_size[i], dim, model_fp, &recs, &count);
        if (s != GM_OK) {
            free(buf);
            gm_store_close(st);
            return s;
        }
        /* Copy out of the file image into the growable array, then drop the
         * image — the arrays are what append writes into. */
        void  *dst   = nullptr;
        size_t bytes = count * rec_size[i];
        if (count > 0) {
            dst = malloc(bytes);
            if (dst == nullptr) {
                free(buf);
                gm_store_close(st);
                return GM_E_OOM;
            }
            memcpy(dst, recs, bytes);
        }
        free(buf);
        switch (i) {
        case GM_F_VECS:
            st->vecs       = dst;
            st->n_chunks   = count;
            st->cap_chunks = count;
            break;
        case GM_F_CHUNKS:
            if (count != st->n_chunks) {
                free(dst);
                gm_store_close(st);
                return GM_E_FORMAT; /* vectors and records disagree */
            }
            st->chunks = dst;
            break;
        default:
            st->docs     = dst;
            st->n_docs   = count;
            st->cap_docs = count;
            break;
        }
    }
    return GM_OK;
}

void gm_store_close(struct gm_store *st) {
    if (st == nullptr) {
        return;
    }
    free(st->vecs);
    free(st->chunks);
    free(st->docs);
    st->vecs   = nullptr;
    st->chunks = nullptr;
    st->docs   = nullptr;
}

size_t gm_store_find_doc(const struct gm_store *st, const char *path) {
    for (size_t i = 0; i < st->n_docs; i++) {
        if (strcmp(st->docs[i].path, path) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static enum gm_status grow(void **p, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap) {
        return GM_OK;
    }
    size_t next = *cap ? *cap * 2u : 64u;
    while (next < need) {
        next *= 2u;
    }
    void *q = realloc(*p, next * elem);
    if (q == nullptr) {
        return GM_E_OOM;
    }
    *p   = q;
    *cap = next;
    return GM_OK;
}

/* The vector array and the chunk-record array are indexed in lockstep, so
 * they share one capacity and grow together. Two separate grows with two
 * separate capacities is exactly the bug this replaced: the second call saw
 * the capacity the first had already raised and allocated nothing. */
[[nodiscard]] static enum gm_status reserve_chunks(struct gm_store *st, size_t need) {
    if (need <= st->cap_chunks) {
        return GM_OK;
    }
    size_t next = st->cap_chunks ? st->cap_chunks * 2u : 64u;
    while (next < need) {
        next *= 2u;
    }
    uint8_t *v = realloc(st->vecs, next * st->bytes);
    if (v == nullptr) {
        return GM_E_OOM;
    }
    st->vecs               = v;
    struct gm_chunk_rec *c = realloc(st->chunks, next * sizeof *c);
    if (c == nullptr) {
        return GM_E_OOM; /* st->vecs keeps the larger block; cap_chunks is
                          * not advanced, so the next attempt retries both. */
    }
    st->chunks     = c;
    st->cap_chunks = next;
    return GM_OK;
}

enum gm_status gm_store_put_doc(
        struct gm_store *st, const char *path, int64_t mtime, uint64_t size, size_t *out_doc) {
    if (strlen(path) >= GM_PATH_MAX) {
        return GM_E_TOO_LONG;
    }
    char file[640];
    join(file, st->dir, GM_FILES[GM_F_DOCS]);

    const size_t found = gm_store_find_doc(st, path);
    if (found != SIZE_MAX) {
        /* Re-index: bump the generation so the old chunks stop matching and
         * rewrite the record in place. ponytail: their vectors stay on disk
         * as dead weight. Compact when the store is visibly bloated — a
         * rewrite of three append-only files, not a migration. */
        struct gm_doc_rec *d = &st->docs[found];
        d->generation++;
        d->mtime    = mtime;
        d->size     = size;
        d->n_chunks = 0;
        FILE *f     = fopen(file, "r+b");
        if (f == nullptr) {
            return GM_E_IO;
        }
        const long off = (long) (sizeof(struct gm_file_header) + found * sizeof *d);
        const int  ok  = fseek(f, off, SEEK_SET) == 0 && fwrite(d, sizeof *d, 1, f) == 1;
        if (fclose(f) != 0 || !ok) {
            return GM_E_IO;
        }
        *out_doc = found;
        return GM_OK;
    }

    enum gm_status s = grow((void **) &st->docs, &st->cap_docs, st->n_docs + 1u,
                            sizeof(struct gm_doc_rec));
    if (s != GM_OK) {
        return s;
    }
    struct gm_doc_rec *d = &st->docs[st->n_docs];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    d->mtime      = mtime;
    d->size       = size;
    d->generation = 1;
    d->n_chunks   = 0;

    s = append_rec(file, d, sizeof *d, (uint64_t) (st->n_docs + 1u));
    if (s != GM_OK) {
        return s;
    }
    *out_doc = st->n_docs++;
    return GM_OK;
}

enum gm_status gm_store_put_chunk(
        struct gm_store *st, size_t doc, uint32_t chunk, uint32_t n_tokens, const uint8_t *bits) {
    if (doc >= st->n_docs) {
        return GM_E_INVALID_ARG;
    }
    enum gm_status s = reserve_chunks(st, st->n_chunks + 1u);
    if (s != GM_OK) {
        return s;
    }

    struct gm_chunk_rec rec = {.doc        = (uint32_t) doc,
                               .chunk      = chunk,
                               .generation = st->docs[doc].generation,
                               .n_tokens   = n_tokens};

    char vfile[640], cfile[640];
    join(vfile, st->dir, GM_FILES[GM_F_VECS]);
    join(cfile, st->dir, GM_FILES[GM_F_CHUNKS]);
    const uint64_t next = (uint64_t) (st->n_chunks + 1u);
    s                   = append_rec(vfile, bits, st->bytes, next);
    if (s == GM_OK) {
        s = append_rec(cfile, &rec, sizeof rec, next);
    }
    if (s != GM_OK) {
        return s;
    }
    memcpy(st->vecs + st->n_chunks * st->bytes, bits, st->bytes);
    st->chunks[st->n_chunks] = rec;
    st->n_chunks++;
    st->docs[doc].n_chunks++;
    return GM_OK;
}
