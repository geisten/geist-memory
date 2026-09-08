#include "gm_store.h"
#include "gm_platform.h"
#include "test_support.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Construct legacy bytes explicitly, independently of the production codec. */
static const char *names[] = {"vectors.gm", "chunks.gm", "docs.gm"};
static const size_t strides[] = {8, 16, 256};
static void put(uint8_t *p, uint64_t n, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        p[i] = (uint8_t)n;
        n >>= 8;
    }
}
static void fixture(const char *dir) {
    CHECK(mkdir(dir, 0700) == 0);
    for (size_t f = 0; f < 3; ++f) {
        uint8_t data[32 + 3 * 256] = {0};
        put(data, 0x314d47ed, 4);
        put(data + 4, 1, 4);
        put(data + 8, 64, 4);
        put(data + 12, strides[f], 4);
        put(data + 16, 3, 8);
        put(data + 24, 7, 8);
        for (size_t i = 0; i < 3; ++i) {
            uint8_t *r = data + 32 + i * strides[f];
            if (f == 1) {
                put(r, i == 0 ? 1 : 0, 4);
                put(r + 8, i == 2 ? 2 : 1, 4);
            } else if (f == 2) {
                const char *ids[] = {"replaced", "first-in-tie", "empty"};
                memcpy(r, ids[i], strlen(ids[i]));
                put(r + 232, 123, 8); /* legacy fields are intentionally ignored */
                put(r + 240, 456, 8);
                put(r + 248, i == 0 ? 2 : 1, 4);
            }
        }
        char path[256];
        snprintf(path, sizeof path, "%s/%s", dir, names[f]);
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        CHECK(fd >= 0 && gm_write_all(fd, data, 32 + 3 * strides[f]));
        CHECK(close(fd) == 0);
    }
}
static void hashes(const char *dir, uint8_t out[3][32]) {
    for (size_t f = 0; f < 3; ++f) {
        char path[256];
        snprintf(path, sizeof path, "%s/%s", dir, names[f]);
        CHECK(gm_file_digest(path, out[f]) == GM_OK);
    }
}
static void unchanged(const char *dir, const uint8_t before[3][32]) {
    uint8_t after[3][32];
    hashes(dir, after);
    CHECK(!memcmp(before, after, sizeof after));
}
static void verify(const char *dir, bool complete) {
    struct gm_store *st = nullptr;
    CHECK(gm_store_open(dir, 64, 7, &st) == GM_OK);
    struct gm_stats stats;
    CHECK(gm_store_stats(st, &stats) == GM_OK);
    CHECK(stats.documents == 0 || stats.documents == 3);
    if (complete)
        CHECK(stats.documents == 3);
    if (stats.documents) {
        CHECK(stats.live_chunks == 2 && stats.obsolete_chunks == 1);
        CHECK(!strcmp(gm_store_doc_path(st, 0), "replaced"));
        CHECK(!strcmp(gm_store_doc_path(st, 2), "empty"));
        uint8_t query[8] = {0};
        struct gm_hit hits[3];
        size_t n;
        CHECK(gm_store_scan(st, 3, query, hits, &n) == GM_OK && n == 2);
        CHECK(hits[0].doc == 1 && hits[1].doc == 0);
        CHECK(gm_store_compact(st) == GM_OK);
        gm_store_close(st);
        CHECK(gm_store_open(dir, 64, 7, &st) == GM_OK);
        CHECK(gm_store_scan(st, 3, query, hits, &n) == GM_OK && n == 2);
        CHECK(hits[0].doc == 1 && hits[1].doc == 0);
    } else
        CHECK(stats.live_chunks == 0 && stats.obsolete_chunks == 0);
    gm_store_close(st);
    CHECK(gm_store_open(dir, 64, 8, &st) == GM_E_MODEL && !st);
}
static void paths(char root[64], char source[128], char dest[128]) {
    test_dir(root);
    snprintf(source, 128, "%s/source", root);
    snprintf(dest, 128, "%s/destination", root);
    fixture(source);
}
static void cleanup(const char *root, const char *source, const char *dest) {
    test_clean(source);
    if (access(dest, F_OK) == 0)
        test_clean(dest);
    test_clean(root);
}
static void basic(void) {
    char root[64], source[128], dest[128], lock[256];
    paths(root, source, dest);
    uint8_t before[3][32];
    hashes(source, before);
    struct gm_store *st = nullptr;
    CHECK(gm_store_open(source, 64, 7, &st) == GM_E_FORMAT && !st);
    unchanged(source, before);
    CHECK(gm_import_v1(source, source, 0) == GM_E_IO);
    CHECK(gm_import_v1(source, dest, 1) == GM_E_LIMIT);
    CHECK(access(dest, F_OK) != 0);
    snprintf(lock, sizeof lock, "%s/memory.lock", source);
    int fd = open(lock, O_RDWR);
    CHECK(fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0);
    CHECK(gm_import_v1(source, dest, 0) == GM_E_BUSY);
    CHECK(close(fd) == 0);
    CHECK(gm_import_v1(source, dest, 0) == GM_OK);
    uint8_t imported[3][32];
    hashes(dest, imported);
    CHECK(gm_import_v1(source, dest, 0) == GM_E_IO);
    unchanged(dest, imported);
    unchanged(source, before);
    verify(dest, true);
    cleanup(root, source, dest);
}
static void invalid_sources(void) {
    for (size_t fault = 0; fault < 6; ++fault) {
        char root[64], source[128], dest[128], path[256];
        paths(root, source, dest);
        snprintf(path, sizeof path, "%s/%s", source,
                 fault == 0   ? "undo.gm"
                 : fault == 1 ? "docs.gm"
                              : "chunks.gm");
        int fd = open(path, O_WRONLY | O_CREAT, 0600);
        CHECK(fd >= 0);
        uint8_t field[8] = {0};
        off_t offset = 0;
        size_t size = 4;
        if (fault == 1) { /* duplicate path, preserve generation */
            CHECK(lseek(fd, 32 + 256, SEEK_SET) >= 0);
            CHECK(gm_write_all(fd, "replaced\0\0\0\0", 13));
        } else if (fault != 0) {
            if (fault == 2) {
                offset = 32;
                put(field, 9, 4);
            }
            if (fault == 3) {
                offset = 32 + 8;
            } /* zero generation */
            if (fault == 4) {
                offset = 32 + 4;
                put(field, 1, 4);
            } /* first ordinal */
            if (fault == 5) {
                offset = 16;
                size = 8;
                put(field, UINT64_MAX, 8);
            }
            CHECK(lseek(fd, offset, SEEK_SET) >= 0 && gm_write_all(fd, field, size));
        }
        CHECK(close(fd) == 0);
        uint8_t before[3][32];
        hashes(source, before);
        CHECK(gm_import_v1(source, dest, 0) == (fault == 0 ? GM_E_UNCERTAIN : GM_E_FORMAT));
        unchanged(source, before);
        CHECK(access(dest, F_OK) != 0);
        cleanup(root, source, dest);
    }
}
static void empty_and_missing(void) {
    char root[64], source[128], dest[128], path[256], missing[256];
    paths(root, source, dest);
    uint8_t before[3][32];
    hashes(source, before);
    snprintf(path, sizeof path, "%s/chunks.gm", source);
    snprintf(missing, sizeof missing, "%s/chunks.saved", source);
    CHECK(rename(path, missing) == 0);
    CHECK(gm_import_v1(source, dest, 0) == GM_E_IO);
    CHECK(access(dest, F_OK) != 0);
    CHECK(rename(missing, path) == 0);
    unchanged(source, before);
    for (size_t f = 0; f < 3; ++f) {
        snprintf(path, sizeof path, "%s/%s", source, names[f]);
        int fd = open(path, O_WRONLY);
        uint8_t zero[8] = {0};
        CHECK(fd >= 0 && ftruncate(fd, 32) == 0 && lseek(fd, 16, SEEK_SET) == 16);
        CHECK(gm_write_all(fd, zero, sizeof zero) && close(fd) == 0);
    }
    hashes(source, before);
    CHECK(gm_import_v1(source, dest, 0) == GM_OK);
    unchanged(source, before);
    struct gm_store *st = nullptr;
    struct gm_stats stats;
    CHECK(gm_store_open(dest, 64, 7, &st) == GM_OK);
    CHECK(gm_store_stats(st, &stats) == GM_OK && !stats.documents && !stats.live_chunks);
    gm_store_close(st);
    cleanup(root, source, dest);
}
static void failure_points(void) {
    /* Split I/O exercises partial headers, records and journal writes. Exhaust
     * each hook sequence plus successful runs beyond its final operation. */
    for (size_t mode = 0; mode < 3; ++mode) {
        long limit = mode == 0 ? 14 : mode == 1 ? 260 : 80;
        size_t successes = 0, failures = 0;
        for (long point = 0; point < limit; ++point) {
            char root[64], source[128], dest[128];
            paths(root, source, dest);
            uint8_t before[3][32];
            hashes(source, before);
            bool complete = false;
            if (mode == 2) {
                pid_t pid = fork();
                CHECK(pid >= 0);
                if (pid == 0) {
                    gm_test_crash_after = point;
                    enum gm_status s = gm_import_v1(source, dest, 0);
                    _exit(s == GM_OK ? 0 : 1);
                }
                int status;
                CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
                CHECK(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 86);
                complete = WEXITSTATUS(status) == 0;
            } else {
                gm_test_io_chunk = mode == 1 ? 17 : 0;
                if (mode == 0)
                    gm_test_alloc_after = point;
                else
                    gm_test_io_after = point;
                enum gm_status s = gm_import_v1(source, dest, 0);
                gm_test_alloc_after = gm_test_io_after = -1;
                gm_test_io_chunk = 0;
                CHECK(s == GM_OK || s == GM_E_OOM || s == GM_E_IO || s == GM_E_UNCERTAIN);
                complete = s == GM_OK;
            }
            successes += complete;
            failures += !complete;
            unchanged(source, before);
            if (access(dest, F_OK) == 0)
                verify(dest, complete);
            else
                CHECK(!complete);
            cleanup(root, source, dest);
        }
        CHECK(successes && failures);
    }
}
int main(void) {
    CHECK(gm_import_v1(nullptr, "target", 0) == GM_E_INVALID_ARG);
    CHECK(gm_import_v1("source", "", 0) == GM_E_INVALID_ARG);
    basic();
    empty_and_missing();
    invalid_sources();
    failure_points();
    puts("PASS: v1 import preserves source, IDs and ordering; malformed input, OOM, I/O and crash "
         "recovery");
}
