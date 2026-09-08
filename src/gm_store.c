#define _POSIX_C_SOURCE 200809L

#include "gm_store.h"
#include "gm_internal.h"
#include "gm_hash.h"
#include "gm_platform.h"
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Open append target; the codec separates disk stride from resident layout. */
struct gm_file {
    int fd;
    size_t rec_size; /* on-disk stride, including checksum */
    enum gm_file_kind kind;
};

struct gm_store {
    size_t dim;   /* bits */
    size_t bytes; /* dim / 8 */
    uint64_t model_fp;
    int lock_fd;
    int dir_fd;
    bool poisoned;
    size_t budget;

    struct gm_file vec_f, chunk_f, doc_f;

    uint8_t *vecs;               /* n_chunks × bytes */
    struct gm_chunk_rec *chunks; /* n_chunks */
    struct gm_doc_rec *docs;     /* n_docs */
    size_t n_chunks, cap_chunks;
    size_t n_docs, cap_docs;
    size_t live;
    /* Path -> document index: open addressing, linear probing, power-of-two
     * capacity kept at or above 2x n_docs. A slot holds a document index or
     * INDEX_EMPTY; the path is compared against docs[slot].path on a hit.
     * Rebuilt from docs[] at open and on growth, never persisted. It replaces
     * a strcmp scan that made a tree pass quadratic. */
    uint32_t *index;
    size_t index_cap;
};
#define INDEX_EMPTY UINT32_MAX

static const char *const VECS_NAME = "vectors.gm";
static const char *const CHUNKS_NAME = "chunks.gm";
static const char *const DOCS_NAME = "docs.gm";

/* dir[] is bounded by the caller's check in gm_store_open; the longest name
 * above plus a separator is 11 bytes. */
enum { GM_DIR_MAX = 512u, GM_PATHBUF = GM_DIR_MAX + 16u };

/* The journal holds only the old headers and one overwritten document.
 * Vector/chunk appends roll back by truncation. It stays durable until all
 * three data files have been synced. No recovery allocation is needed. */
enum {
    UNDO_INDEX = 8 + 3 * GM_HEADER_BYTES,
    UNDO_DOC = UNDO_INDEX + 8,
    UNDO_HASH = UNDO_DOC + GM_DOC_PAYLOAD + GM_CHECKSUM_BYTES,
    UNDO_SIZE = UNDO_HASH + 32
};
static const uint8_t UNDO_MAGIC[8] = {'G', 'M', 'U', 'N', 'D', 'O', '2', '\n'};
static_assert(sizeof(size_t) >= 8, "current platform profiles require 64-bit size_t");

static void old_headers(const struct gm_store *st, struct gm_file_header h[3]) {
    const size_t strides[] = {st->bytes + 32, GM_CHUNK_PAYLOAD + 32, GM_DOC_PAYLOAD + 32};
    for (size_t i = 0; i < 3; ++i) {
        h[i] = (struct gm_file_header){.magic = GM_MAGIC,
                                       .version = GM_VERSION,
                                       .dim = (uint32_t)st->dim,
                                       .rec_size = (uint32_t)strides[i],
                                       .count = i == 2 ? st->n_docs : st->n_chunks,
                                       .model_fp = st->model_fp};
    }
}
/* Never truncate a stale temporary inode: it may be a hard link to data. */
static int create_temp(int dirfd, const char *name) {
    if (unlinkat(dirfd, name, 0) != 0 && errno != ENOENT)
        return -1;
    return openat(dirfd, name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
}
static enum gm_status undo_begin(struct gm_store *st, uint64_t index) {
    uint8_t buf[UNDO_SIZE] = {0};
    struct gm_file_header h[3];
    old_headers(st, h);
    memcpy(buf, UNDO_MAGIC, 8);
    for (size_t i = 0; i < 3; ++i)
        gm_header_encode(&h[i], buf + 8 + i * GM_HEADER_BYTES);
    gm_put64(buf + UNDO_INDEX, index);
    if (index < st->n_docs)
        gm_record_encode(GM_DOCS, st->dim, st->model_fp, index, &st->docs[index], buf + UNDO_DOC);
    gm_digest(UNDO_HASH, buf, buf + UNDO_HASH);
    int fd = create_temp(st->dir_fd, "undo.tmp");
    if (fd < 0)
        return GM_E_IO;
    bool ok = gm_write_all(fd, buf, sizeof buf) && gm_sync(fd);
    if (close(fd) != 0)
        ok = false;
    if (!ok)
        return GM_E_IO; /* no authoritative file has changed */
    if (!gm_move(st->dir_fd, "undo.tmp", "undo.gm"))
        return GM_E_IO;
    if (!gm_sync(st->dir_fd)) {
        st->poisoned = true;
        return GM_E_UNCERTAIN;
    }
    return GM_OK;
}
static enum gm_status undo_finish(struct gm_store *st) {
    struct gm_file *files[] = {&st->vec_f, &st->chunk_f, &st->doc_f};
    for (size_t i = 0; i < 3; ++i) {
        if (!gm_sync(files[i]->fd))
            return GM_E_UNCERTAIN;
    }
    /* Persist data-file renames before deleting the recovery authority. */
    if (!gm_sync(st->dir_fd) || !gm_remove(st->dir_fd, "undo.gm") || !gm_sync(st->dir_fd))
        return GM_E_UNCERTAIN;
    return GM_OK;
}
static enum gm_status read_records(struct gm_store *, struct gm_file *, bool, size_t, void *,
                                   uint64_t);
static enum gm_status undo_recover(struct gm_store *st) {
    int fd = openat(st->dir_fd, "undo.gm", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? GM_OK : GM_E_IO;
    uint8_t buf[UNDO_SIZE], digest[32];
    struct stat sb;
    bool ok = fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size == UNDO_SIZE;
    if (!ok) {
        close(fd);
        return GM_E_FORMAT;
    }
    ok = gm_read_at(fd, buf, sizeof buf, 0);
    close(fd);
    if (!ok)
        return GM_E_IO;
    gm_digest(UNDO_HASH, buf, digest);
    if (memcmp(buf, UNDO_MAGIC, 8) || memcmp(digest, buf + UNDO_HASH, 32))
        return GM_E_FORMAT;
    struct gm_file_header h[3];
    for (size_t i = 0; i < 3; ++i)
        if (!gm_header_decode(buf + 8 + i * GM_HEADER_BYTES, &h[i]))
            return GM_E_FORMAT;
    const size_t strides[] = {st->bytes + 32, GM_CHUNK_PAYLOAD + 32, GM_DOC_PAYLOAD + 32};
    const char *names[] = {VECS_NAME, CHUNKS_NAME, DOCS_NAME};
    uint64_t index = gm_u64(buf + UNDO_INDEX);
    bool creating = index == UINT64_MAX;
    bool compacting = index == UINT64_MAX - 1u;
    bool backups[3] = {false, false, false};
    const char *backup_names[] = {"vectors.bak", "chunks.bak", "docs.bak"};
    uint64_t lengths[3];
    int files[3] = {-1, -1, -1};
    enum gm_status result = GM_E_FORMAT;
    for (size_t i = 0; i < 3; ++i) {
        if (h[i].magic != GM_MAGIC || h[i].version != GM_VERSION || h[i].rec_size != strides[i])
            goto done;
        if (h[i].dim != st->dim || h[i].model_fp != st->model_fp) {
            result = GM_E_MODEL;
            goto done;
        }
        if (h[i].count > (INT64_MAX - GM_HEADER_BYTES) / strides[i])
            goto done;
        lengths[i] = GM_HEADER_BYTES + h[i].count * strides[i];
    }
    if (h[0].count != h[1].count || h[2].count > UINT32_MAX ||
        (!creating && !compacting && index > h[2].count) ||
        (creating && (h[0].count || h[2].count)))
        goto done;
    if (!creating && index < h[2].count) {
        struct gm_doc_rec old;
        if (!gm_record_decode(GM_DOCS, st->dim, st->model_fp, index, buf + UNDO_DOC, &old))
            goto done;
        if (!old.path[0] || !memchr(old.path, 0, GM_PATH_MAX) || !old.generation)
            goto done;
    }
    /* Open and validate every destination before modifying the first one. */
    for (size_t i = 0; i < 3; ++i) {
        const char *source = names[i];
        if (compacting) {
            if (fstatat(st->dir_fd, backup_names[i], &sb, AT_SYMLINK_NOFOLLOW) == 0) {
                source = backup_names[i];
                backups[i] = true;
            } else if (errno != ENOENT) {
                result = GM_E_IO;
                goto done;
            }
        }
        files[i] =
            openat(st->dir_fd, source,
                   O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (creating ? O_CREAT : 0), 0600);
        if (files[i] < 0 || fstat(files[i], &sb) != 0) {
            result = GM_E_IO;
            goto done;
        }
        if (!S_ISREG(sb.st_mode) || (!creating && (uint64_t)sb.st_size < lengths[i]))
            goto done;
        if (compacting) {
            uint8_t actual[GM_HEADER_BYTES];
            if (!gm_read_at(files[i], actual, sizeof actual, 0)) {
                result = GM_E_IO;
                goto done;
            }
            if ((uint64_t)sb.st_size != lengths[i] ||
                memcmp(actual, buf + 8 + i * GM_HEADER_BYTES, sizeof actual))
                goto done;
        }
    }
    for (size_t i = 0; i < 3; ++i) {
        struct gm_file f = {.fd = files[i], .rec_size = strides[i], .kind = (enum gm_file_kind)i};
        result = read_records(st, &f, false, (size_t)h[i].count, nullptr,
                              i == GM_DOCS && !compacting ? index : UINT64_MAX);
        if (result != GM_OK)
            goto done;
    }
    result = GM_E_UNCERTAIN;
    for (size_t i = 0; i < 3; ++i) {
        if (compacting && backups[i] && !gm_move(st->dir_fd, backup_names[i], names[i]))
            goto done;
        if (lseek(files[i], 0, SEEK_SET) < 0 ||
            !gm_write_all(files[i], buf + 8 + i * GM_HEADER_BYTES, GM_HEADER_BYTES))
            goto done;
        if (i == 2 && !creating && index < h[2].count) {
            off_t off = (off_t)(GM_HEADER_BYTES + index * (GM_DOC_PAYLOAD + GM_CHECKSUM_BYTES));
            if (lseek(files[i], off, SEEK_SET) < 0 ||
                !gm_write_all(files[i], buf + UNDO_DOC, GM_DOC_PAYLOAD + GM_CHECKSUM_BYTES))
                goto done;
        }
        if (!gm_truncate(files[i], (off_t)lengths[i]) || !gm_sync(files[i]))
            goto done;
    }
    /* Persist data-file renames before deleting the recovery authority. */
    if (!gm_sync(st->dir_fd) || !gm_remove(st->dir_fd, "undo.gm") || !gm_sync(st->dir_fd))
        goto done;
    result = GM_OK;
done:
    for (size_t i = 0; i < 3; ++i)
        if (files[i] >= 0)
            close(files[i]);
    return result;
}

struct doc_check {
    const char *path;
    uint32_t next_chunk;
};
static int compare_paths(const void *a, const void *b) {
    const struct doc_check *x = a, *y = b;
    return strcmp(x->path, y->path);
}
/* Validate cross-record invariants without quadratic duplicate detection. */
static enum gm_status validate_records(struct gm_store *st) {
    if (st->n_docs > UINT32_MAX || (st->n_docs && !st->docs))
        return GM_E_FORMAT;
    size_t extra,
        resident = st->n_chunks * (st->bytes + sizeof *st->chunks) + st->n_docs * sizeof *st->docs;
    if (ckd_mul(&extra, st->n_docs, sizeof(struct doc_check)) || extra > st->budget - resident)
        return GM_E_LIMIT;
    struct doc_check *check = st->n_docs ? gm_alloc(extra) : nullptr;
    if (st->n_docs && !check)
        return GM_E_OOM;
    enum gm_status s = GM_E_FORMAT;
    for (size_t i = 0; i < st->n_docs; ++i) {
        if (!st->docs[i].path[0] || !memchr(st->docs[i].path, 0, GM_PATH_MAX) ||
            !st->docs[i].generation)
            goto done;
        check[i] = (struct doc_check){.path = st->docs[i].path};
    }
    if (st->n_docs > 1)
        qsort(check, st->n_docs, sizeof *check, compare_paths);
    for (size_t i = 1; i < st->n_docs; ++i)
        if (!strcmp(check[i - 1].path, check[i].path))
            goto done;
    /* All counters start at zero; after the duplicate check the path order is
     * irrelevant and each slot can count the corresponding numeric doc id. */
    for (size_t i = 0; i < st->n_chunks; ++i) {
        const struct gm_chunk_rec *r = &st->chunks[i];
        if (r->doc >= st->n_docs || !r->generation || r->generation > st->docs[r->doc].generation)
            goto done;
        if (r->generation == st->docs[r->doc].generation) {
            if (check[r->doc].next_chunk == UINT32_MAX || r->chunk != check[r->doc].next_chunk++)
                goto done;
        }
    }
    s = GM_OK;
done:
    free(check);
    return s;
}

/* Bounded batch buffers amortize system calls without duplicating a whole file. */
static enum gm_status read_records(struct gm_store *st, struct gm_file *f, bool legacy,
                                   size_t count, void *records, uint64_t skip) {
    uint8_t buffer[65536];
    size_t memory_stride = gm_memory_stride(f->kind, st->dim);
    size_t batch = sizeof buffer / f->rec_size;
    size_t header_bytes = legacy ? 32u : GM_HEADER_BYTES;
    for (size_t at = 0; at < count;) {
        size_t n = count - at < batch ? count - at : batch;
        if (!gm_read_at(f->fd, buffer, n * f->rec_size, (off_t)(header_bytes + at * f->rec_size)))
            return GM_E_IO;
        for (size_t i = 0; i < n; ++i) {
            if (at + i == skip)
                continue;
            void *out = records ? (uint8_t *)records + (at + i) * memory_stride : nullptr;
            const uint8_t *in = buffer + i * f->rec_size;
            if (legacy)
                gm_v1_record_decode(f->kind, st->dim, in, out);
            else if (!gm_record_decode(f->kind, st->dim, st->model_fp, at + i, in, out))
                return GM_E_FORMAT;
        }
        at += n;
    }
    return GM_OK;
}
static bool write_header(int fd, const struct gm_file_header *h) {
    uint8_t bytes[GM_HEADER_BYTES];
    gm_header_encode(h, bytes);
    return lseek(fd, 0, SEEK_SET) >= 0 && gm_write_all(fd, bytes, sizeof bytes);
}
static enum gm_status write_records(struct gm_store *st, struct gm_file *f, size_t at, size_t count,
                                    const void *records) {
    if (!count)
        return GM_OK;
    size_t end;
    if (ckd_add(&end, at, count) || end > (INT64_MAX - GM_HEADER_BYTES) / f->rec_size)
        return GM_E_TOO_LONG;
    if (lseek(f->fd, (off_t)(GM_HEADER_BYTES + at * f->rec_size), SEEK_SET) < 0)
        return GM_E_IO;
    uint8_t buffer[65536];
    size_t memory_stride = gm_memory_stride(f->kind, st->dim);
    size_t batch = sizeof buffer / f->rec_size;
    for (size_t done = 0; done < count;) {
        size_t n = count - done < batch ? count - done : batch;
        for (size_t i = 0; i < n; ++i)
            gm_record_encode(f->kind, st->dim, st->model_fp, at + done + i,
                             (const uint8_t *)records + (done + i) * memory_stride,
                             buffer + i * f->rec_size);
        if (!gm_write_all(f->fd, buffer, n * f->rec_size))
            return GM_E_IO;
        done += n;
    }
    return GM_OK;
}

/* Validate headers, exact length and cumulative memory before allocating. */
static enum gm_status open_file(struct gm_store *st, const char *name, enum gm_file_kind kind,
                                bool legacy, bool create, struct gm_file *f, void **records,
                                size_t *count, size_t *resident) {
    *records = nullptr;
    *count = 0;
    f->kind = kind;
    f->rec_size = gm_payload_size(kind, st->dim) + (legacy ? 0u : GM_CHECKSUM_BYTES);
    f->fd = openat(st->dir_fd, name,
                   (legacy ? O_RDONLY : O_RDWR) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (f->fd < 0 && errno == ENOENT && create) {
        f->fd = openat(st->dir_fd, name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        struct gm_file_header h = {.magic = GM_MAGIC,
                                   .version = GM_VERSION,
                                   .dim = (uint32_t)st->dim,
                                   .rec_size = (uint32_t)f->rec_size,
                                   .model_fp = st->model_fp};
        return f->fd >= 0 && write_header(f->fd, &h) ? GM_OK : GM_E_IO;
    }
    if (f->fd < 0)
        return GM_E_IO;
    size_t header_size = legacy ? 32u : GM_HEADER_BYTES;
    struct stat sb;
    if (fstat(f->fd, &sb) != 0)
        return GM_E_IO;
    if (!S_ISREG(sb.st_mode) || sb.st_size < (off_t)header_size)
        return GM_E_FORMAT;
    uint8_t header[GM_HEADER_BYTES];
    if (!gm_read_at(f->fd, header, header_size, 0))
        return GM_E_IO;
    struct gm_file_header h;
    if (!(legacy ? gm_v1_header_decode(header, &h) : gm_header_decode(header, &h)) ||
        h.rec_size != f->rec_size)
        return GM_E_FORMAT;
    if (h.dim != st->dim || h.model_fp != st->model_fp)
        return GM_E_MODEL;
    uint64_t payload = (uint64_t)sb.st_size - header_size;
    if (payload % f->rec_size || payload / f->rec_size != h.count || h.count > SIZE_MAX)
        return GM_E_FORMAT;
    if (legacy && h.count > (INT64_MAX - GM_HEADER_BYTES) / (f->rec_size + GM_CHECKSUM_BYTES))
        return GM_E_TOO_LONG;
    size_t bytes;
    if (ckd_mul(&bytes, (size_t)h.count, gm_memory_stride(kind, st->dim)) ||
        ckd_add(resident, *resident, bytes) || *resident > st->budget)
        return GM_E_LIMIT;
    void *data = h.count ? gm_alloc(bytes) : nullptr;
    if (h.count && !data)
        return GM_E_OOM;
    enum gm_status s = read_records(st, f, legacy, (size_t)h.count, data, UINT64_MAX);
    if (s != GM_OK) {
        free(data);
        return s;
    }
    *records = data;
    *count = (size_t)h.count;
    return GM_OK;
}

/* Only called while a durable journal protects the old headers and lengths. */
static enum gm_status append_rec(struct gm_store *st, struct gm_file *f, const void *records,
                                 size_t added, size_t total) {
    enum gm_status s = write_records(st, f, total - added, added, records);
    struct gm_file_header h = {.magic = GM_MAGIC,
                               .version = GM_VERSION,
                               .dim = (uint32_t)st->dim,
                               .rec_size = (uint32_t)f->rec_size,
                               .count = total,
                               .model_fp = st->model_fp};
    if (s == GM_OK && !write_header(f->fd, &h))
        s = GM_E_IO;
    return s;
}

static bool sync_parent(const char *dir) {
    char parent[GM_DIR_MAX];
    strcpy(parent, dir);
    size_t n = strlen(parent);
    while (n > 1 && parent[n - 1] == '/')
        parent[--n] = 0;
    char *slash = strrchr(parent, '/');
    if (!slash)
        strcpy(parent, ".");
    else if (slash == parent)
        slash[1] = 0;
    else
        *slash = 0;
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    bool ok = fd >= 0 && gm_sync(fd);
    if (fd >= 0)
        close(fd);
    return ok;
}

[[nodiscard]] static enum gm_status index_reserve(struct gm_store *st, size_t n_docs);
enum gm_status gm_store_open_limited(const char *dir, size_t dim, uint64_t model_fp, size_t budget,
                                     struct gm_store **out) {
    if (out != nullptr)
        *out = nullptr;
    if (out == nullptr || dir == nullptr || dim == 0 || dim % 8u != 0u || dim > GM_DIM_MAX) {
        return GM_E_INVALID_ARG;
    }
    if (strlen(dir) >= GM_DIR_MAX) {
        return GM_E_TOO_LONG;
    }
    struct gm_store *st = gm_zero(sizeof *st);
    if (st == nullptr) {
        return GM_E_OOM;
    }
    st->vec_f.fd = st->chunk_f.fd = st->doc_f.fd = -1;
    st->lock_fd = -1;
    st->dir_fd = -1;
    st->dim = dim;
    st->bytes = dim / 8u;
    st->model_fp = model_fp;
    st->budget = budget ? budget : 256u * 1024u * 1024u;

    bool created = mkdir(dir, 0700) == 0;
    if (!created && errno != EEXIST) {
        gm_store_close(st);
        return GM_E_IO;
    }

    st->dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (st->dir_fd < 0) {
        gm_store_close(st);
        return GM_E_IO;
    }
    if (created && !sync_parent(dir)) {
        gm_store_close(st);
        return GM_E_IO;
    }
    /* Check the complete set before creating any file. A partial store is
     * damaged, not a request to silently create missing authorities. */
    char lock_path[GM_PATHBUF];
    snprintf(lock_path, sizeof lock_path, "%s/memory.lock", dir);
    st->lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat lock_stat;
    if (st->lock_fd < 0 || fstat(st->lock_fd, &lock_stat) != 0) {
        gm_store_close(st);
        return GM_E_IO;
    }
    if (!S_ISREG(lock_stat.st_mode)) {
        gm_store_close(st);
        return GM_E_FORMAT;
    }
    if (flock(st->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        const enum gm_status result = errno == EWOULDBLOCK ? GM_E_BUSY : GM_E_IO;
        gm_store_close(st);
        return result;
    }
    enum gm_status recovered = undo_recover(st);
    if (recovered != GM_OK) {
        gm_store_close(st);
        return recovered;
    }
    const char *names[] = {VECS_NAME, CHUNKS_NAME, DOCS_NAME};
    size_t present = 0;
    size_t resident = 0;
    for (size_t i = 0; i < 3; ++i) {
        char path[GM_PATHBUF];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        struct stat sb;
        if (lstat(path, &sb) == 0) {
            if (!S_ISREG(sb.st_mode)) {
                gm_store_close(st);
                return GM_E_FORMAT;
            }
            if (sb.st_size < (off_t)GM_HEADER_BYTES) {
                gm_store_close(st);
                return GM_E_FORMAT;
            }
            ++present;
        } else if (errno != ENOENT) {
            gm_store_close(st);
            return GM_E_IO;
        }
    }
    if (present != 0 && present != 3) {
        gm_store_close(st);
        return GM_E_FORMAT;
    }

    if (present == 0) {
        enum gm_status begun = undo_begin(st, UINT64_MAX);
        if (begun != GM_OK) {
            gm_store_close(st);
            return begun;
        }
    }
    size_t n_vecs = 0;
    void *vecs = nullptr, *chunks = nullptr, *docs = nullptr;
    enum gm_status s = open_file(st, VECS_NAME, GM_VECTORS, false, present == 0, &st->vec_f, &vecs,
                                 &n_vecs, &resident);
    if (s == GM_OK) {
        s = open_file(st, CHUNKS_NAME, GM_CHUNKS, false, present == 0, &st->chunk_f, &chunks,
                      &st->n_chunks, &resident);
    }
    if (s == GM_OK) {
        s = open_file(st, DOCS_NAME, GM_DOCS, false, present == 0, &st->doc_f, &docs, &st->n_docs,
                      &resident);
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
    st->vecs = vecs;
    st->chunks = chunks;
    st->docs = docs;
    s = validate_records(st);
    if (s != GM_OK) {
        gm_store_close(st);
        return s;
    }
    if (present == 0) {
        enum gm_status finished = undo_finish(st);
        if (finished != GM_OK) {
            gm_store_close(st);
            return finished;
        }
    }
    for (size_t i = 0; i < st->n_chunks; ++i)
        st->live += st->chunks[i].generation == st->docs[st->chunks[i].doc].generation;
    st->cap_chunks = st->n_chunks;
    st->cap_docs = st->n_docs;
    if (st->n_docs && (s = index_reserve(st, st->n_docs)) != GM_OK) {
        gm_store_close(st);
        return s;
    }
    *out = st;
    return GM_OK;
}

enum gm_status gm_store_open(const char *dir, size_t dim, uint64_t model_fp,
                             struct gm_store **out) {
    return gm_store_open_limited(dir, dim, model_fp, 0, out);
}

void gm_store_close(struct gm_store *st) {
    if (st == nullptr) {
        return;
    }
    struct gm_file *files[] = {&st->vec_f, &st->chunk_f, &st->doc_f};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        if (files[i]->fd >= 0)
            close(files[i]->fd);
    }
    if (st->lock_fd >= 0)
        close(st->lock_fd);
    if (st->dir_fd >= 0)
        close(st->dir_fd);
    free(st->vecs);
    free(st->chunks);
    free(st->docs);
    free(st->index);
    free(st);
}

bool gm_store_path_too_long(const char *path) {
    return strnlen(path, GM_PATH_MAX) >= GM_PATH_MAX;
}

const char *gm_store_doc_path(const struct gm_store *st, uint32_t doc) {
    return st && !st->poisoned && doc < st->n_docs ? st->docs[doc].path : nullptr;
}

/* A chunk counts only while its document has not been re-indexed under it.
 * The predicate lives here, next to the two arrays it joins, so no caller
 * can forget it and read a dead vector as a real hit. */
static bool chunk_live(const struct gm_store *st, size_t i) {
    const struct gm_chunk_rec *rec = &st->chunks[i];
    return rec->generation == st->docs[rec->doc].generation;
}

size_t gm_store_live_chunks(const struct gm_store *st) {
    return st && !st->poisoned ? st->live : 0;
}

static uint64_t fnv1a(const char *p) {
    uint64_t h = 1469598103934665603ULL;
    for (; *p; ++p)
        h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    return h;
}
/* The slot holding `path`, or the empty slot where it would go. */
static size_t index_slot(const struct gm_store *st, const char *path) {
    const size_t mask = st->index_cap - 1u;
    size_t i = (size_t)fnv1a(path) & mask;
    while (st->index[i] != INDEX_EMPTY && strcmp(st->docs[st->index[i]].path, path) != 0)
        i = (i + 1u) & mask;
    return i;
}
/* Grow the table so that n_docs documents stay under half full. The old
 * table survives an allocation failure. ponytail: not counted against the
 * store budget, at most 16 bytes per document beside a 256-byte record. */
[[nodiscard]] static enum gm_status index_reserve(struct gm_store *st, size_t n_docs) {
    size_t cap = st->index_cap ? st->index_cap : 64u;
    while (cap < 2u * n_docs)
        cap *= 2u;
    if (cap == st->index_cap)
        return GM_OK;
    uint32_t *table = gm_alloc(cap * sizeof *table);
    if (!table)
        return GM_E_OOM;
    memset(table, 0xff, cap * sizeof *table);
    free(st->index);
    st->index = table;
    st->index_cap = cap;
    for (size_t i = 0; i < st->n_docs; ++i)
        st->index[index_slot(st, st->docs[i].path)] = (uint32_t)i;
    return GM_OK;
}
static size_t find_doc(const struct gm_store *st, const char *path) {
    if (!st->index_cap)
        return SIZE_MAX;
    const uint32_t slot = st->index[index_slot(st, path)];
    return slot == INDEX_EMPTY ? SIZE_MAX : slot;
}

static size_t next_cap(size_t cap, size_t need) {
    size_t next = cap ? cap : 64u;
    while (next < need) {
        if (ckd_mul(&next, next, 2u))
            return 0;
    }
    return next;
}

/* Grow both arrays together. Allocation failure retains both original blocks
 * and their capacity; peak memory is checked before either allocation. */
[[nodiscard]] static enum gm_status reserve_chunks(struct gm_store *st, size_t need) {
    if (need <= st->cap_chunks) {
        return GM_OK;
    }
    const size_t next = next_cap(st->cap_chunks, need);
    size_t vector_bytes, record_bytes;
    if (!next || ckd_mul(&vector_bytes, next, st->bytes) ||
        ckd_mul(&record_bytes, next, sizeof *st->chunks))
        return GM_E_TOO_LONG;
    size_t peak, current, doc_bytes;
    if (ckd_mul(&doc_bytes, st->cap_docs, sizeof *st->docs) ||
        ckd_mul(&current, st->cap_chunks, st->bytes + sizeof *st->chunks) ||
        ckd_add(&peak, vector_bytes, record_bytes) || ckd_add(&peak, peak, current) ||
        ckd_add(&peak, peak, doc_bytes) || peak > st->budget)
        return GM_E_LIMIT;
    uint8_t *v = gm_alloc(vector_bytes);
    if (!v)
        return GM_E_OOM;
    struct gm_chunk_rec *c = gm_alloc(record_bytes);
    if (!c) {
        free(v);
        return GM_E_OOM;
    }
    if (st->n_chunks) {
        memcpy(v, st->vecs, st->n_chunks * st->bytes);
        memcpy(c, st->chunks, st->n_chunks * sizeof *c);
    }
    free(st->vecs);
    free(st->chunks);
    st->vecs = v;
    st->chunks = c;
    st->cap_chunks = next;
    return GM_OK;
}

/* Compare the representation actually persisted. Equal-length source text
 * is never a freshness test. Recomputing embeddings avoids another on-disk
 * content-hash field while retaining idempotence for deterministic models. */
static bool same_vectors(const struct gm_store *st, size_t doc, size_t count, const uint8_t *bits) {
    size_t n = 0;
    for (size_t i = 0; i < st->n_chunks; ++i) {
        if (st->chunks[i].doc != doc || !chunk_live(st, i))
            continue;
        if (n >= count || st->chunks[i].chunk != n ||
            memcmp(st->vecs + i * st->bytes, bits + n * st->bytes, st->bytes))
            return false;
        ++n;
    }
    return n == count;
}

enum gm_status gm_store_replace(struct gm_store *st, const char *path, size_t count,
                                const uint8_t *bits) {
    if (!st || !path || !path[0] || (count && !bits))
        return GM_E_INVALID_ARG;
    if (st->poisoned)
        return GM_E_UNCERTAIN;
    if (gm_store_path_too_long(path))
        return GM_E_TOO_LONG;
    size_t next_count, vector_bytes, record_bytes;
    if (count > UINT32_MAX || ckd_add(&next_count, st->n_chunks, count) ||
        ckd_mul(&vector_bytes, count, st->bytes) ||
        ckd_mul(&record_bytes, count, sizeof(struct gm_chunk_rec)))
        return GM_E_TOO_LONG;
    if (next_count > (INT64_MAX - GM_HEADER_BYTES) / (st->bytes + GM_CHECKSUM_BYTES) ||
        next_count > (INT64_MAX - GM_HEADER_BYTES) / (GM_CHUNK_PAYLOAD + GM_CHECKSUM_BYTES))
        return GM_E_TOO_LONG;
    size_t found = find_doc(st, path);
    if (found != SIZE_MAX && same_vectors(st, found, count, bits))
        return GM_OK;
    if (found == SIZE_MAX && st->n_docs >= UINT32_MAX)
        return GM_E_LIMIT;
    size_t old_live = 0;
    if (found != SIZE_MAX)
        for (size_t i = 0; i < st->n_chunks; ++i)
            old_live += st->chunks[i].doc == found && chunk_live(st, i);
    struct gm_doc_rec doc = {0};
    const size_t index = found == SIZE_MAX ? st->n_docs : found;
    /* Copy the borrowed path before reserve_docs can invalidate it. */
    memcpy(doc.path, path, strlen(path) + 1u);
    if (found != SIZE_MAX && st->docs[found].generation == UINT32_MAX)
        return GM_E_LIMIT;
    doc.generation = found == SIZE_MAX ? 1u : st->docs[found].generation + 1u;
    enum gm_status s = found == SIZE_MAX ? index_reserve(st, st->n_docs + 1u) : GM_OK;
    if (s == GM_OK)
        s = reserve_chunks(st, next_count);
    if (s != GM_OK)
        return s;
    if (index == st->cap_docs) {
        size_t next = next_cap(st->cap_docs, index + 1u), bytes, peak, chunk_bytes, old_bytes;
        if (!next || ckd_mul(&bytes, next, sizeof *st->docs) ||
            ckd_mul(&chunk_bytes, st->cap_chunks, st->bytes + sizeof *st->chunks) ||
            ckd_mul(&old_bytes, st->cap_docs, sizeof *st->docs) ||
            ckd_add(&peak, bytes, chunk_bytes) || ckd_add(&peak, peak, old_bytes) ||
            peak > st->budget)
            return GM_E_LIMIT;
        struct gm_doc_rec *p = gm_resize(st->docs, bytes);
        if (!p)
            return GM_E_OOM;
        st->docs = p;
        st->cap_docs = next;
    }
    size_t resident =
        st->cap_docs * sizeof *st->docs + st->cap_chunks * (st->bytes + sizeof *st->chunks);
    if (record_bytes > st->budget - resident)
        return GM_E_LIMIT;
    struct gm_chunk_rec *records = count ? gm_alloc(record_bytes) : nullptr;
    if (count && !records)
        return GM_E_OOM;
    for (size_t i = 0; i < count; ++i) {
        records[i] = (struct gm_chunk_rec){
            .doc = (uint32_t)index, .chunk = (uint32_t)i, .generation = doc.generation};
    }
    /* Journal the old state before modifying any data file. */
    s = undo_begin(st, index);
    if (s != GM_OK) {
        free(records);
        return s;
    }
    if (count) {
        s = append_rec(st, &st->vec_f, bits, count, next_count);
        if (s == GM_OK)
            s = append_rec(st, &st->chunk_f, records, count, next_count);
    }
    if (s == GM_OK) {
        s = found == SIZE_MAX ? append_rec(st, &st->doc_f, &doc, 1, st->n_docs + 1u)
                              : write_records(st, &st->doc_f, index, 1, &doc);
    }
    if (s == GM_OK)
        s = undo_finish(st);
    if (s != GM_OK) {
        free(records);
        st->poisoned = true;
        return GM_E_UNCERTAIN;
    }
    if (count) {
        memcpy(st->vecs + st->n_chunks * st->bytes, bits, vector_bytes);
        memcpy(st->chunks + st->n_chunks, records, record_bytes);
    }
    free(records);
    st->docs[index] = doc;
    if (found == SIZE_MAX) {
        st->index[index_slot(st, doc.path)] = (uint32_t)index;
        ++st->n_docs;
    }
    st->n_chunks = next_count;
    st->live = st->live - old_live + count;
    return GM_OK;
}

enum gm_status gm_store_stats(const struct gm_store *st, struct gm_stats *out) {
    if (out)
        memset(out, 0, sizeof *out);
    if (!st || !out)
        return GM_E_INVALID_ARG;
    if (st->poisoned)
        return GM_E_UNCERTAIN;
    out->documents = st->n_docs;
    out->live_chunks = gm_store_live_chunks(st);
    out->memory_bytes = st->cap_docs * sizeof *st->docs +
                        st->cap_chunks * (st->bytes + sizeof *st->chunks) +
                        st->index_cap * sizeof *st->index;
    out->disk_bytes = 3u * GM_HEADER_BYTES + st->n_docs * (GM_DOC_PAYLOAD + 32u) +
                      st->n_chunks * (st->bytes + GM_CHUNK_PAYLOAD + 64u);
    out->obsolete_chunks = st->n_chunks - out->live_chunks;
    return GM_OK;
}

enum gm_status gm_store_scan(struct gm_store *st, size_t k, const uint8_t *query_bits,
                             struct gm_hit *out, size_t *n_out) {
    if (n_out)
        *n_out = 0;
    if (!st || !n_out || !out || k == 0 || k > SIZE_MAX / sizeof *out || query_bits == nullptr) {
        return GM_E_INVALID_ARG;
    }
    if (st->poisoned)
        return GM_E_UNCERTAIN;
    /* Exact Hamming scan; sign quantization itself is lossy. Benchmark the
     * target machine before adding a separate search index. */
    const size_t words = st->bytes / 8u;

    size_t n = 0;
    for (size_t i = 0; i < st->n_chunks; i++) {
        if (!chunk_live(st, i)) {
            continue;
        }
        const uint8_t *v = st->vecs + i * st->bytes;
        uint32_t d = 0;
        for (size_t w = 0; w < words; w++) {
            uint64_t a, b;
            memcpy(&a, query_bits + w * 8u, sizeof a);
            memcpy(&b, v + w * 8u, sizeof b);
            d += gm_popcount64(a ^ b);
        }
        for (size_t b = words * 8u; b < st->bytes; ++b)
            d += (uint32_t)__builtin_popcount((unsigned)(query_bits[b] ^ v[b]));
        /* Insertion into the top-k. k is a handful, so this beats sorting
         * the whole store and allocates nothing. */
        if (n < k || d < out[n - 1].distance) {
            size_t pos = (n < k) ? n : k - 1;
            while (pos > 0 && out[pos - 1].distance > d) {
                out[pos] = out[pos - 1];
                pos--;
            }
            out[pos] = (struct gm_hit){
                .doc = st->chunks[i].doc, .chunk = st->chunks[i].chunk, .distance = d};
            if (n < k) {
                n++;
            }
        }
    }
    *n_out = n;
    return GM_OK;
}

/* Compaction uses immutable hard-link backups instead of a second journal
 * format. The same undo marker makes all three renames one recoverable change. */
enum gm_status gm_store_compact(struct gm_store *st) {
    if (!st)
        return GM_E_INVALID_ARG;
    if (st->poisoned)
        return GM_E_UNCERTAIN;
    if (st->live == st->n_chunks)
        return GM_OK;
    size_t vb, cb, extra, resident;
    if (ckd_mul(&vb, st->live, st->bytes) || ckd_mul(&cb, st->live, sizeof *st->chunks) ||
        ckd_add(&extra, vb, cb) ||
        ckd_mul(&resident, st->cap_chunks, st->bytes + sizeof *st->chunks) ||
        ckd_add(&resident, resident, st->cap_docs * sizeof *st->docs) ||
        extra > st->budget - resident)
        return GM_E_LIMIT;
    uint8_t *vectors = st->live ? gm_alloc(vb) : nullptr;
    struct gm_chunk_rec *chunks = st->live ? gm_alloc(cb) : nullptr;
    if (st->live && (!vectors || !chunks)) {
        free(vectors);
        free(chunks);
        return GM_E_OOM;
    }
    for (size_t i = 0, n = 0; st->live && i < st->n_chunks; ++i) {
        if (!chunk_live(st, i))
            continue;
        memcpy(vectors + n * st->bytes, st->vecs + i * st->bytes, st->bytes);
        chunks[n++] = st->chunks[i];
    }
    const char *names[] = {VECS_NAME, CHUNKS_NAME, DOCS_NAME};
    const char *fresh[] = {"vectors.new", "chunks.new", "docs.new"};
    const char *backup[] = {"vectors.bak", "chunks.bak", "docs.bak"};
    struct gm_file *files[] = {&st->vec_f, &st->chunk_f, &st->doc_f};
    const void *data[] = {vectors, chunks, st->docs};
    const size_t counts[] = {st->live, st->live, st->n_docs};
    struct gm_file_header h[3];
    old_headers(st, h);
    h[0].count = h[1].count = st->live;
    int fd[3] = {-1, -1, -1};
    enum gm_status s = GM_E_IO;
    for (size_t i = 0; i < 3; ++i) {
        fd[i] = create_temp(st->dir_fd, fresh[i]);
        struct gm_file fresh_file = *files[i];
        fresh_file.fd = fd[i];
        if (fd[i] < 0 || !write_header(fd[i], &h[i]) ||
            write_records(st, &fresh_file, 0, counts[i], data[i]) != GM_OK || !gm_sync(fd[i]))
            goto done;
        if (unlinkat(st->dir_fd, backup[i], 0) != 0 && errno != ENOENT)
            goto done;
        if (linkat(st->dir_fd, names[i], st->dir_fd, backup[i], 0) != 0)
            goto done;
    }
    if (!gm_sync(st->dir_fd))
        goto done;
    s = undo_begin(st, UINT64_MAX - 1u);
    if (s != GM_OK)
        goto done;
    for (size_t i = 0; i < 3; ++i) {
        if (!gm_move(st->dir_fd, fresh[i], names[i])) {
            s = GM_E_UNCERTAIN;
            st->poisoned = true;
            goto done;
        }
    }
    for (size_t i = 0; i < 3; ++i) {
        close(files[i]->fd);
        files[i]->fd = fd[i];
        fd[i] = -1;
    }
    s = undo_finish(st);
    if (s != GM_OK) {
        st->poisoned = true;
        goto done;
    }
    free(st->vecs);
    free(st->chunks);
    st->vecs = vectors;
    st->chunks = chunks;
    vectors = nullptr;
    chunks = nullptr;
    st->n_chunks = st->cap_chunks = st->live;
    /* These aliases have no authority once undo.gm has been durably removed. */
    for (size_t i = 0; i < 3; ++i)
        (void)unlinkat(st->dir_fd, backup[i], 0);
done:
    for (size_t i = 0; i < 3; ++i)
        if (fd[i] >= 0)
            close(fd[i]);
    free(vectors);
    free(chunks);
    return s;
}

/* Import only a quiescent v1 store. Its data files are opened read-only;
 * preserving the fingerprint deliberately does not assert a new model identity. */
enum gm_status gm_import_v1(const char *source, const char *destination, size_t budget) {
    if (!source || !source[0] || !destination || !destination[0])
        return GM_E_INVALID_ARG;
    if (strnlen(source, GM_DIR_MAX) >= GM_DIR_MAX || strnlen(destination, GM_DIR_MAX) >= GM_DIR_MAX)
        return GM_E_TOO_LONG;
    struct gm_store *old = gm_zero(sizeof *old), *fresh = nullptr;
    if (!old)
        return GM_E_OOM;
    old->dir_fd = old->lock_fd = old->vec_f.fd = old->chunk_f.fd = old->doc_f.fd = -1;
    old->budget = budget ? budget : 256u * 1024u * 1024u;
    enum gm_status s = GM_E_IO;
    old->dir_fd = open(source, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (old->dir_fd < 0)
        goto done;
    old->lock_fd = openat(old->dir_fd, "memory.lock",
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat sb;
    if (old->lock_fd < 0 || fstat(old->lock_fd, &sb) != 0)
        goto done;
    if (!S_ISREG(sb.st_mode)) {
        s = GM_E_FORMAT;
        goto done;
    }
    if (flock(old->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        s = errno == EWOULDBLOCK ? GM_E_BUSY : GM_E_IO;
        goto done;
    }
    if (fstatat(old->dir_fd, "undo.gm", &sb, AT_SYMLINK_NOFOLLOW) == 0) {
        s = GM_E_UNCERTAIN; /* recover using the v1 implementation first */
        goto done;
    }
    if (errno != ENOENT)
        goto done;
    int fd = openat(old->dir_fd, VECS_NAME, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        goto done;
    uint8_t bytes[32];
    bool read =
        fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && gm_read_at(fd, bytes, sizeof bytes, 0);
    close(fd);
    if (!read)
        goto done;
    struct gm_file_header h;
    if (!gm_v1_header_decode(bytes, &h) || !h.dim || h.dim % 8 || h.dim > GM_DIM_MAX) {
        s = GM_E_FORMAT;
        goto done;
    }
    old->dim = h.dim;
    old->bytes = h.dim / 8;
    old->model_fp = h.model_fp;
    size_t resident = 0, vectors = 0;
    void *v = nullptr, *c = nullptr, *d = nullptr;
    s = open_file(old, VECS_NAME, GM_VECTORS, true, false, &old->vec_f, &v, &vectors, &resident);
    old->vecs = v;
    if (s == GM_OK)
        s = open_file(old, CHUNKS_NAME, GM_CHUNKS, true, false, &old->chunk_f, &c, &old->n_chunks,
                      &resident);
    old->chunks = c;
    if (s == GM_OK)
        s = open_file(old, DOCS_NAME, GM_DOCS, true, false, &old->doc_f, &d, &old->n_docs,
                      &resident);
    old->docs = d;
    if (s != GM_OK)
        goto done;
    if (vectors != old->n_chunks) {
        s = GM_E_FORMAT;
        goto done;
    }
    s = validate_records(old);
    if (s != GM_OK)
        goto done;
    /* A successful mkdir reserves an absent destination; existing paths are
     * never overwritten. Failures can leave this new destination for inspection. */
    if (mkdir(destination, 0700) != 0 || !sync_parent(destination)) {
        s = GM_E_IO;
        goto done;
    }
    s = gm_store_open_limited(destination, old->dim, old->model_fp, budget, &fresh);
    if (s != GM_OK)
        goto done;
    /* A cooperating writer could have populated the newly created directory
     * before we acquired its lock. Never append to that writer's store. */
    if (fresh->n_docs || fresh->n_chunks) {
        s = GM_E_BUSY;
        goto done;
    }
    /* One transaction preserves physical order, generations and empty IDs.
     * Replacing documents individually would change tie ordering. */
    s = undo_begin(fresh, UINT64_MAX);
    if (s != GM_OK)
        goto done;
    s = append_rec(fresh, &fresh->vec_f, old->vecs, old->n_chunks, old->n_chunks);
    if (s == GM_OK)
        s = append_rec(fresh, &fresh->chunk_f, old->chunks, old->n_chunks, old->n_chunks);
    if (s == GM_OK)
        s = append_rec(fresh, &fresh->doc_f, old->docs, old->n_docs, old->n_docs);
    if (s == GM_OK)
        s = undo_finish(fresh);
    if (s != GM_OK)
        s = GM_E_UNCERTAIN;
done:
    gm_store_close(fresh);
    gm_store_close(old);
    return s;
}
