#define _POSIX_C_SOURCE 200809L
#include "gm_store.h"
#include "test_support.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static void write_at(const char *path, long offset, size_t n, const void *p) {
    FILE *f = fopen(path, "r+b");
    CHECK(f);
    CHECK(fseek(f, offset, SEEK_SET) == 0);
    CHECK(fwrite(p, 1, n, f) == n);
    CHECK(fclose(f) == 0);
}
static void seed(struct gm_store *st) {
    uint8_t bits[18] = {0};
    bits[17] = 255;
    CHECK(gm_store_replace(st, "doc", 2, bits) == GM_OK);
}
static void search(void) {
    char dir[64];
    test_dir(dir);
    struct gm_store *st = nullptr, *other = nullptr;
    CHECK(gm_store_open(dir, 72, 7, &st) == GM_OK);
    seed(st);
    CHECK(gm_store_open(dir, 72, 7, &other) == GM_E_BUSY && !other);
    for (unsigned value = 0; value < 256; ++value) {
        uint8_t q[10] = {0};
        q[9] = (uint8_t)value;
        struct gm_hit hits[4];
        size_t n = 9;
        CHECK(gm_store_scan(st, 4, q + 1, hits, &n) == GM_OK && n == 2);
        unsigned d = 0;
        for (unsigned b = 0; b < 8; ++b)
            d += (value >> b) & 1u;
        CHECK(hits[0].distance == (d < 8 - d ? d : 8 - d));
        CHECK(hits[1].distance == (d > 8 - d ? d : 8 - d));
        CHECK(hits[0].chunk == (d <= 4 ? 0u : 1u));
    }
    gm_store_close(st);
    CHECK(gm_store_open(dir, 72, 7, &st) == GM_OK);
    CHECK(gm_store_live_chunks(st) == 2);
    gm_store_close(st);
    test_clean(dir);
}
static void invalid_files(void) {
    for (int mode = 0; mode < 6; ++mode) {
        char dir[64], path[128];
        test_dir(dir);
        struct gm_store *st = nullptr;
        CHECK(gm_store_open(dir, 72, 7, &st) == GM_OK);
        seed(st);
        gm_store_close(st);
        snprintf(path, sizeof path, "%s/%s", dir,
                 mode == 0                ? "vectors.gm"
                 : mode == 1 || mode == 4 ? "docs.gm"
                                          : "chunks.gm");
        if (mode == 0) {
            FILE *f = fopen(path, "ab");
            CHECK(f);
            CHECK(fputc(1, f) != EOF);
            CHECK(fclose(f) == 0);
        }
        if (mode == 1) {
            struct gm_doc_rec doc = {.generation = 1};
            memset(doc.path, 'x', sizeof doc.path);
            uint8_t data[GM_DOC_PAYLOAD + GM_CHECKSUM_BYTES];
            gm_record_encode(GM_DOCS, 72, 7, 0, &doc, data);
            write_at(path, GM_HEADER_BYTES, sizeof data, data);
        }
        if (mode == 2) {
            struct gm_chunk_rec chunk = {.doc = 9, .generation = 1};
            uint8_t data[GM_CHUNK_PAYLOAD + GM_CHECKSUM_BYTES];
            gm_record_encode(GM_CHUNKS, 72, 7, 0, &chunk, data);
            write_at(path, GM_HEADER_BYTES, sizeof data, data);
        }
        if (mode == 3)
            CHECK(unlink(path) == 0);
        if (mode == 4) {
            FILE *f = fopen(path, "wb");
            CHECK(f);
            CHECK(fclose(f) == 0);
        }
        if (mode == 5) {
            struct gm_file_header header = {.magic = GM_MAGIC,
                                            .version = GM_VERSION,
                                            .dim = 72,
                                            .rec_size = GM_CHUNK_PAYLOAD + GM_CHECKSUM_BYTES,
                                            .count = UINT64_MAX,
                                            .model_fp = 7};
            uint8_t data[GM_HEADER_BYTES];
            gm_header_encode(&header, data);
            write_at(path, 0, sizeof data, data);
        }
        CHECK(gm_store_open(dir, 72, 7, &st) == GM_E_FORMAT && !st);
        if (mode == 3)
            CHECK(access(path, F_OK) != 0); /* rejected, never recreated */
        test_clean(dir);
    }
}
static void special_files(void) {
    for (int mode = 0; mode < 3; ++mode) {
        char dir[64], path[128], vectors[128];
        test_dir(dir);
        struct gm_store *st = nullptr;
        CHECK(gm_store_open(dir, 72, 7, &st) == GM_OK);
        seed(st);
        gm_store_close(st);
        snprintf(path, sizeof path, "%s/%s", dir, mode == 0 ? "undo.gm" : "undo.tmp");
        snprintf(vectors, sizeof vectors, "%s/vectors.gm", dir);
        if (mode == 2)
            CHECK(link(vectors, path) == 0);
        else
            CHECK(mkfifo(path, 0600) == 0);
        enum gm_status s = gm_store_open(dir, 72, 7, &st);
        if (mode == 0) {
            CHECK(s == GM_E_FORMAT && !st);
        } else {
            CHECK(s == GM_OK);
            uint8_t bits[9] = {255};
            CHECK(gm_store_replace(st, "doc", 1, bits) == GM_OK);
            gm_store_close(st);
            CHECK(gm_store_open(dir, 72, 7, &st) == GM_OK);
            CHECK(gm_store_live_chunks(st) == 1);
        }
        gm_store_close(st);
        test_clean(dir);
    }
}
static void oom(void) {
    for (long fail = 0; fail < 8; ++fail) {
        char dir[64];
        test_dir(dir);
        struct gm_store *st = nullptr;
        CHECK(gm_store_open(dir, 72, 7, &st) == GM_OK);
        seed(st);
        gm_store_close(st);
        gm_test_alloc_after = fail;
        enum gm_status s = gm_store_open(dir, 72, 7, &st);
        gm_test_alloc_after = -1;
        CHECK(s == GM_OK || s == GM_E_OOM);
        if (s == GM_OK)
            CHECK(gm_store_live_chunks(st) == 2);
        else
            CHECK(!st);
        gm_store_close(st);
        test_clean(dir);
    }
}
static void growth_and_compaction_oom(void) {
    for (long fail = 0; fail <= 4; ++fail) {
        char dir[64];
        test_dir(dir);
        struct gm_store *st = nullptr;
        CHECK(gm_store_open(dir, 64, 7, &st) == GM_OK);
        uint8_t bits[8] = {0};
        for (size_t i = 0; i < 64; ++i) {
            char id[32];
            snprintf(id, sizeof id, "doc%zu", i);
            CHECK(gm_store_replace(st, id, 1, bits) == GM_OK);
        }
        /* The 65th document grows vector, chunk and document arrays, then
         * allocates transaction metadata. Fail each allocation in turn. */
        gm_test_alloc_after = fail;
        enum gm_status result = gm_store_replace(st, "new", 1, bits);
        gm_test_alloc_after = -1;
        CHECK(result == (fail < 4 ? GM_E_OOM : GM_OK));
        CHECK(gm_store_live_chunks(st) == (fail < 4 ? 64u : 65u));
        CHECK(!strcmp(gm_store_doc_path(st, 0), "doc0"));
        gm_store_close(st);
        CHECK(gm_store_open(dir, 64, 7, &st) == GM_OK);
        CHECK(gm_store_live_chunks(st) == (fail < 4 ? 64u : 65u));
        gm_store_close(st);
        test_clean(dir);
    }
    for (long fail = 0; fail <= 2; ++fail) {
        char dir[64];
        test_dir(dir);
        struct gm_store *st = nullptr;
        CHECK(gm_store_open(dir, 72, 7, &st) == GM_OK);
        seed(st);
        uint8_t bits[9] = {255};
        CHECK(gm_store_replace(st, "doc", 1, bits) == GM_OK);
        gm_test_alloc_after = fail;
        enum gm_status result = gm_store_compact(st);
        gm_test_alloc_after = -1;
        CHECK(result == (fail < 2 ? GM_E_OOM : GM_OK));
        gm_store_close(st);
        CHECK(gm_store_open(dir, 72, 7, &st) == GM_OK);
        struct gm_stats stats;
        CHECK(gm_store_stats(st, &stats) == GM_OK);
        CHECK(stats.live_chunks == 1 && stats.obsolete_chunks == (fail < 2 ? 2u : 0u));
        struct gm_hit hit;
        size_t count;
        CHECK(gm_store_scan(st, 1, bits, &hit, &count) == GM_OK && count == 1 && hit.distance == 0);
        gm_store_close(st);
        test_clean(dir);
    }
}
int main(void) {
    CHECK(gm_store_open("/tmp/unused", 64, 7, nullptr) == GM_E_INVALID_ARG);
    search();
    invalid_files();
    oom();
    special_files();
    growth_and_compaction_oom();
    puts("PASS: store validation, unaligned/tail search, writer lock, OOM "
         "cleanup");
    return 0;
}
