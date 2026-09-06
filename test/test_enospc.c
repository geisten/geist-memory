/* Run only on an explicitly provided, disposable filesystem of at most 64 MiB.
 * No fault hooks are enabled: write(2) must actually report ENOSPC. */
#include "gm_store.h"
#include "test_support.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static void fill(const char *path, size_t leave) {
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0);
    uint8_t bytes[65536];
    memset(bytes, 0xa5, sizeof bytes);
    size_t total = 0, chunk = sizeof bytes;
    for (;;) {
        ssize_t n = write(fd, bytes, chunk);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            CHECK(errno == ENOSPC);
            if (chunk > 1) {
                chunk /= 2;
                continue;
            }
            break;
        }
        CHECK(n > 0);
        total += (size_t)n;
        CHECK(total <= 64u * 1024u * 1024u);
    }
    CHECK(total > leave);
    if (leave)
        CHECK(ftruncate(fd, (off_t)(total - leave)) == 0);
    CHECK(close(fd) == 0);
}
static void verify(struct gm_store *st, size_t count, const uint8_t *query) {
    CHECK(gm_store_live_chunks(st) == count);
    struct gm_hit hits[3];
    size_t n;
    CHECK(gm_store_scan(st, 3, query, hits, &n) == GM_OK);
    CHECK(n == (count < 3 ? count : 3));
    if (n)
        CHECK(hits[0].distance == 0);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    struct statvfs fs;
    CHECK(statvfs(argv[1], &fs) == 0);
    CHECK(fs.f_frsize && fs.f_blocks <= (64u * 1024u * 1024u) / fs.f_frsize);
    uint8_t *bits = calloc(16384, 128);
    CHECK(bits);
    for (size_t mode = 0; mode < 4; ++mode) {
        char dir[512], filler[600];
        int len = snprintf(dir, sizeof dir, "%s/gm-full-XXXXXX", argv[1]);
        CHECK(len > 0 && (size_t)len < sizeof dir && mkdtemp(dir));
        snprintf(filler, sizeof filler, "%s/filler", dir);
        struct gm_store *st = nullptr;
        memset(bits, 0, 16384 * 128);
        if (mode) {
            CHECK(gm_store_open(dir, 1024, 7, &st) == GM_OK);
            CHECK(gm_store_replace(st, "doc", 4096, bits) == GM_OK);
            if (mode == 3) {
                memset(bits, 255, 16384 * 128);
                CHECK(gm_store_replace(st, "doc", 4096, bits) == GM_OK);
            }
        }
        /* Full journal creation, partial append, and partial compacted files. */
        fill(filler, mode >= 2 ? 128u * 1024u : 0);
        enum gm_status s;
        if (!mode)
            s = gm_store_open(dir, 1024, 7, &st);
        else if (mode == 3)
            s = gm_store_compact(st);
        else {
            memset(bits, 255, 16384 * 128);
            s = gm_store_replace(st, "doc", 16384, bits);
        }
        printf("scenario=%zu status=%d\n", mode, (int)s);
        CHECK(s == GM_E_IO || s == GM_E_UNCERTAIN);
        if (mode == 2)
            CHECK(s == GM_E_UNCERTAIN); /* journal fits; appended data does not */
        if (mode && s == GM_E_UNCERTAIN) {
            struct gm_stats stats;
            CHECK(gm_store_stats(st, &stats) == GM_E_UNCERTAIN);
        }
        gm_store_close(st);
        /* Attempt recovery before releasing space; a second attempt after
         * releasing it must still recover the complete original contents. */
        s = gm_store_open(dir, 1024, 7, &st);
        CHECK(s == GM_OK || s == GM_E_IO || s == GM_E_UNCERTAIN);
        gm_store_close(st);
        CHECK(unlink(filler) == 0);
        CHECK(gm_store_open(dir, 1024, 7, &st) == GM_OK);
        memset(bits, mode == 3 ? 255 : 0, 16384 * 128);
        verify(st, mode ? 4096 : 0, bits);
        CHECK(gm_store_replace(st, "doc", 1, bits) == GM_OK);
        CHECK(gm_store_compact(st) == GM_OK);
        gm_store_close(st);
        CHECK(gm_store_open(dir, 1024, 7, &st) == GM_OK);
        verify(st, 1, bits);
        gm_store_close(st);
        test_clean(dir);
        printf("PASS: real ENOSPC scenario %zu, recovery and successful retry\n", mode);
    }
    free(bits);
}
