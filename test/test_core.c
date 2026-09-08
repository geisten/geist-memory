#define _POSIX_C_SOURCE 200809L
#include "gm_internal.h"
#include "gm_hash.h"
#include "gm_platform.h"
#include "gm_store.h"
#include "test_support.h"
#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static void write_bytes(const char *path, size_t n, const void *p) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    CHECK(gm_write_all(fd, p, n));
    CHECK(close(fd) == 0);
}
static struct gm_store *store(const char *dir, size_t dim) {
    struct gm_store *st = nullptr;
    CHECK(gm_store_open(dir, dim, 7, &st) == GM_OK);
    return st;
}
static uint32_t reference(size_t n, const uint8_t *a, const uint8_t *b) {
    uint32_t d = 0;
    for (size_t i = 0; i < n; ++i)
        for (unsigned j = 0; j < 8; ++j)
            d += ((unsigned)(a[i] ^ b[i]) >> j) & 1u;
    return d;
}
static void hash_tests(void) {
    const char *vectors[] = {"", "abc", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"};
    const char *expected[] = {"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"};
    for (size_t i = 0; i < 3; ++i) {
        uint8_t h[32];
        char hex[65];
        gm_digest(strlen(vectors[i]), vectors[i], h);
        for (size_t j = 0; j < 32; ++j)
            snprintf(hex + j * 2, 3, "%02x", h[j]);
        CHECK(!strcmp(hex, expected[i]));
        struct gm_hash s;
        gm_hash_init(&s);
        for (size_t j = 0; j < strlen(vectors[i]); ++j)
            gm_hash_update(&s, 1, vectors[i] + j);
        uint8_t split[32];
        gm_hash_final(&s, split);
        CHECK(!memcmp(h, split, 32));
    }
    float v[8] = {-1, 0, 1, -0.0f, 2, -2, 3, 4};
    uint8_t b = 0;
    CHECK(gm_pack(8, v, &b) == GM_OK && b == 212);
    v[0] = NAN;
    CHECK(gm_pack(8, v, &b) == GM_E_ENGINE);
    v[0] = INFINITY;
    CHECK(gm_pack(8, v, &b) == GM_E_ENGINE);
}
static void search_tests(void) {
    uint32_t rng = 12345;
    const size_t dims[] = {8, 16, 56, 64, 72, 120, 128, 1024};
    for (size_t d = 0; d < sizeof dims / sizeof *dims; ++d) {
        char dir[64];
        test_dir(dir);
        struct gm_store *st = store(dir, dims[d]);
        uint8_t vectors[37 * 128], q[128];
        size_t bytes = dims[d] / 8;
        for (size_t i = 0; i < 37 * bytes; ++i) {
            rng = rng * 1664525u + 1013904223u;
            vectors[i] = (uint8_t)(rng >> 24);
        }
        for (size_t i = 0; i < bytes; ++i) {
            rng = rng * 1664525u + 1013904223u;
            q[i] = (uint8_t)(rng >> 24);
        }
        CHECK(gm_store_replace(st, "doc", 37, vectors) == GM_OK);
        for (size_t k = 1; k < 45; ++k) {
            struct gm_hit hits[45];
            size_t n = 99;
            CHECK(gm_store_scan(st, k, q, hits, &n) == GM_OK);
            CHECK(n == (k < 37 ? k : 37));
            bool seen[37] = {false};
            for (size_t i = 0; i < n; ++i) {
                uint32_t best = UINT32_MAX;
                size_t index = 37;
                for (size_t j = 0; j < 37; ++j)
                    if (!seen[j]) {
                        uint32_t x = reference(bytes, q, vectors + j * bytes);
                        if (x < best) {
                            best = x;
                            index = j;
                        }
                    }
                CHECK(hits[i].chunk == index && hits[i].distance == best);
                seen[index] = true;
            }
        }
        gm_store_close(st);
        st = store(dir, dims[d]);
        struct gm_stats s;
        CHECK(gm_store_stats(st, &s) == GM_OK && s.live_chunks == 37);
        gm_store_close(st);
        test_clean(dir);
    }
}
static void api_tests(void) {
    char dir[64], model[128];
    test_dir(dir);
    snprintf(model, sizeof model, "%s/model", dir);
    write_bytes(model, 3, "abc");
    struct gm *m = nullptr;
    CHECK(gm_open(dir, model, nullptr, &m) == GM_OK);
    CHECK(gm_remember_text(m, "doc", "AA") == GM_OK);
    struct gm_stats before, after;
    CHECK(gm_get_stats(m, &before) == GM_OK);
    CHECK(gm_remember_text(m, "doc", "AA") == GM_OK);
    CHECK(gm_get_stats(m, &after) == GM_OK && after.disk_bytes == before.disk_bytes);
    CHECK(gm_remember_text(m, "doc", "BB") == GM_OK);
    struct gm_hit h[3];
    size_t n = 0;
    CHECK(gm_recall(m, 3, "BB", h, &n) == GM_OK && n == 1 && h[0].distance == 0);
    mock_fail_after = 0;
    CHECK(gm_remember_text(m, "doc", "CC") == GM_E_ENGINE);
    CHECK(gm_recall(m, 3, "BB", h, &n) == GM_OK && n == 1 && h[0].distance == 0);
    CHECK(gm_remember_text(m, "doc", "") == GM_OK && gm_chunk_count(m) == 0);
    CHECK(gm_remember_text_n(m, "doc", 2, "X\0") == GM_E_INVALID_ARG);
    CHECK(gm_remember_text_n(m, "doc", 0, nullptr) == GM_OK);
    CHECK(gm_remember_text(m, "doc", "AA") == GM_OK);
    char saved[GM_PATH_MAX];
    CHECK(gm_doc_path_copy(m, 0, sizeof saved, saved) == GM_OK);
    for (size_t i = 0; i < 70; ++i) {
        char id[32];
        snprintf(id, sizeof id, "id%zu", i);
        CHECK(gm_remember_text(m, id, "BB") == GM_OK);
    }
    CHECK(!strcmp(saved, "doc"));
    CHECK(!strcmp(gm_doc_path(m, 0), "doc"));
    char tiny[1] = {'x'};
    CHECK(gm_doc_path_copy(m, 0, 1, tiny) == GM_E_TOO_LONG && !tiny[0]);
    char query[GM_QUERY_MAX + 2];
    memset(query, 'A', sizeof query);
    query[sizeof query - 1] = 0;
    n = 77;
    CHECK(gm_recall(m, 1, query, h, &n) == GM_E_TOO_LONG && n == 0);
    query[GM_WINDOW + 1] = 0;
    CHECK(gm_recall(m, 1, query, h, &n) == GM_E_TOO_LONG);
    CHECK(gm_recall(m, 0, "A", h, &n) == GM_E_INVALID_ARG && n == 0);
    CHECK(gm_recall(m, 1, "A", nullptr, &n) == GM_E_INVALID_ARG);
    char path[128];
    snprintf(path, sizeof path, "%s/note", dir);
    write_bytes(path, 2, "AA");
    CHECK(gm_remember_file(m, path) == GM_OK);
    write_bytes(path, 2, "BB");
    CHECK(gm_remember_file(m, path) == GM_OK);
    CHECK(gm_remember_file(m, dir) == GM_E_IO);
    gm_close(m);
    CHECK(gm_open(dir, model, nullptr, &m) == GM_OK && gm_chunk_count(m) == 72);
    gm_close(m);
    write_bytes(model, 3, "xyz");
    CHECK(gm_open(dir, model, nullptr, &m) == GM_E_MODEL && m == nullptr);
    test_clean(dir);
}
static void transaction_tests(void) {
    for (long fail = 0; fail < 160; ++fail) {
        char dir[64];
        test_dir(dir);
        struct gm_store *st = store(dir, 72), *other = nullptr;
        CHECK(gm_store_open(dir, 72, 7, &other) == GM_E_BUSY && !other);
        uint8_t old[18] = {0}, replacement[27];
        memset(replacement, 255, sizeof replacement);
        CHECK(gm_store_replace(st, "one", 2, old) == GM_OK);
        gm_test_io_chunk = 17;
        gm_test_io_after = fail;
        enum gm_status result = gm_store_replace(st, "one", 3, replacement);
        gm_test_io_after = -1;
        gm_test_io_chunk = 0;
        CHECK(result == GM_OK || result == GM_E_IO || result == GM_E_UNCERTAIN);
        struct gm_hit hits[4];
        size_t n = 9;
        if (result == GM_E_UNCERTAIN) {
            CHECK(gm_store_scan(st, 4, old, hits, &n) == GM_E_UNCERTAIN && n == 0);
            CHECK(gm_store_replace(st, "two", 2, old) == GM_E_UNCERTAIN);
        }
        gm_store_close(st);
        st = store(dir, 72);
        CHECK(gm_store_scan(st, 4, old, hits, &n) == GM_OK);
        bool is_old = n == 2 && hits[0].distance == 0 && hits[1].distance == 0;
        bool is_new =
            n == 3 && hits[0].distance == 72 && hits[1].distance == 72 && hits[2].distance == 72;
        CHECK(is_old || is_new);
        if (result == GM_E_IO)
            CHECK(is_old);
        if (result == GM_OK)
            CHECK(is_new);
        gm_store_close(st);
        test_clean(dir);
    }
}
static void crash_tests(void) {
    for (long point = 0; point < 30; ++point) {
        char dir[64];
        test_dir(dir);
        struct gm_store *st = store(dir, 72);
        uint8_t old[18] = {0}, replacement[27];
        memset(replacement, 255, sizeof replacement);
        CHECK(gm_store_replace(st, "doc", 2, old) == GM_OK);
        gm_store_close(st);
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            st = store(dir, 72);
            gm_test_crash_after = point;
            enum gm_status s = gm_store_replace(st, "doc", 3, replacement);
            _exit(s == GM_OK ? 0 : 1);
        }
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 86);
        st = store(dir, 72);
        struct gm_hit hits[4];
        size_t n = 0;
        CHECK(gm_store_scan(st, 4, old, hits, &n) == GM_OK);
        CHECK((n == 2 && hits[0].distance == 0 && hits[1].distance == 0) ||
              (n == 3 && hits[0].distance == 72 && hits[2].distance == 72));
        gm_store_close(st);
        test_clean(dir);
    }
}
static void compact_tests(void) {
    for (int crash = 0; crash < 2; ++crash)
        for (long point = 0; point < 34; ++point) {
            char dir[64];
            test_dir(dir);
            struct gm_store *st = store(dir, 72);
            uint8_t old[18] = {0}, now[9];
            memset(now, 255, sizeof now);
            CHECK(gm_store_replace(st, "doc", 2, old) == GM_OK);
            CHECK(gm_store_replace(st, "doc", 1, now) == GM_OK);
            CHECK(gm_store_replace(st, "empty", 0, nullptr) == GM_OK);
            struct gm_stats before;
            CHECK(gm_store_stats(st, &before) == GM_OK);
            gm_store_close(st);
            if (crash) {
                pid_t child = fork();
                CHECK(child >= 0);
                if (child == 0) {
                    st = store(dir, 72);
                    gm_test_crash_after = point;
                    enum gm_status s = gm_store_compact(st);
                    _exit(s == GM_OK ? 0 : 1);
                }
                int status;
                CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
                CHECK(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 86);
            } else {
                st = store(dir, 72);
                gm_test_io_after = point;
                enum gm_status s = gm_store_compact(st);
                gm_test_io_after = -1;
                CHECK(s == GM_OK || s == GM_E_IO || s == GM_E_UNCERTAIN);
                gm_store_close(st);
            }
            st = store(dir, 72);
            struct gm_stats after;
            CHECK(gm_store_stats(st, &after) == GM_OK);
            CHECK(after.live_chunks == 1 && after.documents == 2 &&
                  after.disk_bytes <= before.disk_bytes);
            CHECK(!strcmp(gm_store_doc_path(st, 1), "empty"));
            struct gm_hit hit;
            size_t n = 0;
            CHECK(gm_store_scan(st, 1, now, &hit, &n) == GM_OK && n == 1 && hit.distance == 0);
            CHECK(gm_store_compact(st) == GM_OK);
            CHECK(gm_store_stats(st, &after) == GM_OK && after.obsolete_chunks == 0);
            CHECK(gm_store_replace(st, "doc", 0, nullptr) == GM_OK);
            CHECK(gm_store_compact(st) == GM_OK);
            CHECK(gm_store_live_chunks(st) == 0);
            gm_store_close(st);
            test_clean(dir);
        }
}
static void creation_crashes(void) {
    for (long point = 0; point < 16; ++point) {
        char dir[64];
        test_dir(dir);
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            gm_test_crash_after = point;
            struct gm_store *st = store(dir, 72);
            gm_store_close(st);
            _exit(0);
        }
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 86);
        struct gm_store *st = store(dir, 72);
        CHECK(gm_store_live_chunks(st) == 0);
        gm_store_close(st);
        test_clean(dir);
    }
}
static void journal_corruption(void) {
    char dir[64], path[128];
    test_dir(dir);
    struct gm_store *st = store(dir, 72);
    uint8_t old[9] = {0}, now[9] = {255};
    CHECK(gm_store_replace(st, "doc", 1, old) == GM_OK);
    gm_test_io_after = 4;
    CHECK(gm_store_replace(st, "doc", 1, now) == GM_E_UNCERTAIN);
    gm_test_io_after = -1;
    gm_store_close(st);
    snprintf(path, sizeof path, "%s/undo.gm", dir);
    char *saved = nullptr;
    size_t length = 0;
    CHECK(gm_file_read(path, 4096, &saved, &length) == GM_OK && length == 528);
    for (size_t i = 0; i < length; ++i) {
        saved[i] ^= 1;
        write_bytes(path, length, saved);
        saved[i] ^= 1;
        CHECK(gm_store_open(dir, 72, 7, &st) == GM_E_FORMAT && !st);
    }
    write_bytes(path, length, saved);
    free(saved);
    st = store(dir, 72);
    struct gm_hit hit;
    size_t n = 0;
    CHECK(gm_store_scan(st, 1, old, &hit, &n) == GM_OK && n == 1 && hit.distance == 0);
    gm_store_close(st);
    test_clean(dir);
}
/* Recovery must itself survive interruption, including partially restored
 * compaction renames. A second open completes the same undo record. */
static void recovery_tests(void) {
    for (int compact = 0; compact < 2; ++compact)
        for (int crash = 0; crash < 2; ++crash)
            for (long point = 0; point < 48; ++point) {
                char dir[64];
                test_dir(dir);
                struct gm_store *st = store(dir, 72);
                uint8_t old[18] = {0}, next[9] = {255};
                CHECK(gm_store_replace(st, "doc", 2, old) == GM_OK);
                if (compact)
                    CHECK(gm_store_replace(st, "doc", 1, next) == GM_OK);
                gm_test_io_after = compact ? 15 : 6;
                enum gm_status s =
                    compact ? gm_store_compact(st) : gm_store_replace(st, "doc", 1, next);
                gm_test_io_after = -1;
                CHECK(s == GM_E_UNCERTAIN);
                gm_store_close(st);
                if (crash) {
                    pid_t child = fork();
                    CHECK(child >= 0);
                    if (child == 0) {
                        gm_test_crash_after = point;
                        st = store(dir, 72);
                        gm_store_close(st);
                        _exit(0);
                    }
                    int status;
                    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
                    CHECK(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 86);
                } else {
                    gm_test_io_after = point;
                    s = gm_store_open(dir, 72, 7, &st);
                    gm_test_io_after = -1;
                    CHECK(s == GM_OK || s == GM_E_IO || s == GM_E_UNCERTAIN);
                    if (s != GM_OK)
                        CHECK(!st);
                    gm_store_close(st);
                }
                st = store(dir, 72);
                struct gm_hit hits[3];
                size_t n = 0;
                CHECK(gm_store_scan(st, 3, compact ? next : old, hits, &n) == GM_OK);
                CHECK(n == (compact ? 1u : 2u) && hits[0].distance == 0);
                gm_store_close(st);
                test_clean(dir);
            }
}
static void api_allocation_tests(void) {
    for (long point = 0; point < 12; ++point) {
        char dir[64], model[128];
        test_dir(dir);
        snprintf(model, sizeof model, "%s/model", dir);
        write_bytes(model, 3, "abc");
        struct gm *m = nullptr;
        gm_test_alloc_after = point;
        enum gm_status s = gm_open(dir, model, nullptr, &m);
        gm_test_alloc_after = -1;
        CHECK(s == GM_OK || s == GM_E_OOM);
        if (s != GM_OK)
            CHECK(!m);
        gm_close(m);
        CHECK(gm_open(dir, model, nullptr, &m) == GM_OK);
        char text[601];
        memset(text, 'B', sizeof text - 1);
        text[sizeof text - 1] = 0;
        CHECK(gm_remember_text(m, "doc", "A") == GM_OK);
        gm_test_alloc_after = point;
        s = gm_remember_text(m, "doc", text);
        gm_test_alloc_after = -1;
        CHECK(s == GM_OK || s == GM_E_OOM);
        CHECK(gm_chunk_count(m) == (s == GM_OK ? 3u : 1u));
        CHECK(gm_remember_text(m, "doc", "A") == GM_OK);
        mock_fail_after = 1;
        CHECK(gm_remember_text(m, "doc", text) == GM_E_ENGINE);
        CHECK(gm_chunk_count(m) == 1);
        struct gm_hit hit;
        size_t n;
        CHECK(gm_recall(m, 1, "A", &hit, &n) == GM_OK && n == 1 && hit.distance == 0);
        char *long_text = malloc(GM_TEXT_MAX + 2u);
        CHECK(long_text);
        memset(long_text, 'A', GM_TEXT_MAX + 1u);
        long_text[GM_TEXT_MAX + 1u] = 0;
        CHECK(gm_remember_text(m, "doc", long_text) == GM_E_TOO_LONG);
        long_text[GM_TOKENS + 1u] = 0;
        CHECK(gm_remember_text(m, "doc", long_text) == GM_E_TOO_LONG);
        long_text[GM_TOKENS] = 0;
        CHECK(gm_remember_text(m, "doc", long_text) == GM_OK);
        free(long_text);
        gm_close(m);
        test_clean(dir);
    }
}
static void allocation_tests(void) {
    for (long fail = 0; fail < 7; ++fail) {
        char dir[64];
        test_dir(dir);
        struct gm_store *st = store(dir, 64);
        uint8_t v[8] = {0};
        CHECK(gm_store_replace(st, "old", 1, v) == GM_OK);
        gm_test_alloc_after = fail;
        enum gm_status s = gm_store_replace(st, "new", 1, v);
        gm_test_alloc_after = -1;
        CHECK(s == GM_OK || s == GM_E_OOM);
        struct gm_stats stats;
        CHECK(gm_store_stats(st, &stats) == GM_OK && stats.live_chunks == (s == GM_OK ? 2u : 1u));
        gm_store_close(st);
        test_clean(dir);
    }
    char dir[64];
    test_dir(dir);
    struct gm_store *st = nullptr;
    CHECK(gm_store_open_limited(dir, 64, 7, 32, &st) == GM_OK);
    uint8_t v[8] = {0};
    CHECK(gm_store_replace(st, "x", 1, v) == GM_E_LIMIT);
    gm_store_close(st);
    test_clean(dir);
}
int main(void) {
    hash_tests();
    search_tests();
    api_tests();
    transaction_tests();
    crash_tests();
    compact_tests();
    creation_crashes();
    journal_corruption();
    allocation_tests();
    api_allocation_tests();
    recovery_tests();
    puts("PASS: hashes, packing, reference search, API, failed writes, OOM, "
         "budgets");
    return 0;
}
