#define _POSIX_C_SOURCE 200809L
#include "gm_platform.h"
#include "gm_hash.h"
#include "gm_internal.h"
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
static_assert(sizeof(off_t) >= 8, "64-bit file offsets required");
static bool io_fail(void) {
#ifdef GM_TESTING
    if (gm_test_fail(&gm_test_io_after)) {
        errno = EIO;
        return true;
    }
#endif
    return false;
}
bool gm_read_at(int fd, void *out, size_t n, off_t offset) {
    uint8_t *p = out;
    while (n) {
        if (io_fail())
            return false;
        size_t part = n > 1048576 ? 1048576 : n;
#ifdef GM_TESTING
        extern size_t gm_test_io_chunk;
        if (gm_test_io_chunk && part > gm_test_io_chunk)
            part = gm_test_io_chunk;
#endif
        ssize_t r = pread(fd, p, part, offset);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        p += (size_t)r;
        n -= (size_t)r;
        offset += r;
    }
    return true;
}
bool gm_write_all(int fd, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        if (io_fail())
            return false;
        size_t part = n > 1048576 ? 1048576 : n;
#ifdef GM_TESTING
        extern size_t gm_test_io_chunk;
        if (gm_test_io_chunk && part > gm_test_io_chunk)
            part = gm_test_io_chunk;
#endif
        ssize_t r = write(fd, p, part);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        p += (size_t)r;
        n -= (size_t)r;
    }
    return true;
}
bool gm_sync(int fd) {
    if (io_fail())
        return false;
    int r;
    do {
        r = fsync(fd);
    } while (r < 0 && errno == EINTR);
    return r == 0;
}
static bool same_file(const struct stat *a, const struct stat *b) {
#if defined(__APPLE__)
    return a->st_size == b->st_size && a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}
enum gm_status gm_file_read(const char *path, size_t limit, char **out, size_t *len) {
    *out = nullptr;
    *len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return GM_E_IO;
    struct stat a, b;
    enum gm_status s = GM_E_IO;
    char *p = nullptr;
    if (fstat(fd, &a) != 0 || !S_ISREG(a.st_mode) || a.st_size < 0)
        goto done;
    if ((uint64_t)a.st_size > limit) {
        s = GM_E_TOO_LONG;
        goto done;
    }
    size_t n = (size_t)a.st_size, bytes;
    if (ckd_add(&bytes, n, 1u)) {
        s = GM_E_TOO_LONG;
        goto done;
    }
    p = gm_alloc(bytes);
    if (!p) {
        s = GM_E_OOM;
        goto done;
    }
    if (!gm_read_at(fd, p, n, 0) || fstat(fd, &b) != 0 || !same_file(&a, &b))
        goto done;
    p[n] = 0;
    *out = p;
    *len = n;
    p = nullptr;
    s = GM_OK;
done:
    free(p);
    close(fd);
    return s;
}
enum gm_status gm_file_digest(const char *path, uint8_t hash[32]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return GM_E_IO;
    struct stat a, b;
    enum gm_status s = GM_E_IO;
    if (fstat(fd, &a) != 0 || !S_ISREG(a.st_mode) || a.st_size < 0 ||
        (uint64_t)a.st_size > UINT64_MAX / 8)
        goto done;
    struct gm_hash h;
    gm_hash_init(&h);
    uint8_t buf[65536];
    for (off_t off = 0; off < a.st_size;) {
        size_t n =
            (uint64_t)(a.st_size - off) > sizeof buf ? sizeof buf : (size_t)(a.st_size - off);
        if (!gm_read_at(fd, buf, n, off))
            goto done;
        gm_hash_update(&h, n, buf);
        off += (off_t)n;
    }
    if (fstat(fd, &b) != 0 || !same_file(&a, &b))
        goto done;
    gm_hash_final(&h, hash);
    s = GM_OK;
done:
    close(fd);
    return s;
}

bool gm_move(int dirfd, const char *from, const char *to) {
    return !io_fail() && renameat(dirfd, from, dirfd, to) == 0;
}
bool gm_remove(int dirfd, const char *name) {
    return !io_fail() && unlinkat(dirfd, name, 0) == 0;
}
bool gm_truncate(int fd, off_t size) {
    return !io_fail() && ftruncate(fd, size) == 0;
}
