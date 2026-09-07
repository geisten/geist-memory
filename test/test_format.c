#include "gm_store.h"
#include "gm_platform.h"
#include "gm_hash.h"
#include "test_support.h"
#include "fixtures/v2_64.h"
#include <fcntl.h>
#include <unistd.h>

static const char *names[] = {"vectors.gm", "chunks.gm", "docs.gm"};
static void save(const char *dir, size_t file, size_t size, const void *bytes) {
    char path[128];
    snprintf(path, sizeof path, "%s/%s", dir, names[file]);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0 && gm_write_all(fd, bytes, size) && close(fd) == 0);
}
static void equal_file(const char *dir, size_t file, size_t length, const void *bytes) {
    char path[128], *actual = nullptr;
    size_t n;
    snprintf(path, sizeof path, "%s/%s", dir, names[file]);
    CHECK(gm_file_read(path, 65536, &actual, &n) == GM_OK);
    CHECK(n == length && !memcmp(bytes, actual, n));
    free(actual);
}
static void golden_and_corruption(void) {
    const uint8_t *data[] = {fixture_vectors, fixture_chunks, fixture_docs};
    const size_t sizes[] = {sizeof fixture_vectors, sizeof fixture_chunks, sizeof fixture_docs};
    char dir[64];
    test_dir(dir);
    struct gm_store *st = nullptr;
    CHECK(gm_store_open(dir, 64, 7, &st) == GM_OK);
    uint8_t bits[] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(gm_store_replace(st, "fixture", 1, bits) == GM_OK);
    gm_store_close(st);
    for (size_t f = 0; f < 3; ++f)
        equal_file(dir, f, sizes[f], data[f]);
    uint8_t changed[sizeof fixture_docs];
    for (size_t f = 0; f < 3; ++f) {
        for (size_t i = 0; i < sizes[f]; ++i) {
            memcpy(changed, data[f], sizes[f]);
            changed[i] ^= 1;
            save(dir, f, sizes[f], changed);
            CHECK(gm_store_open(dir, 64, 7, &st) == GM_E_FORMAT && !st);
            for (size_t j = 0; j < 3; ++j)
                equal_file(dir, j, sizes[j], j == f ? changed : data[j]);
        }
        save(dir, f, sizes[f], data[f]);
    }
    /* Valid checksums cannot make impossible counts or references valid. */
    memcpy(changed, fixture_vectors, sizeof fixture_vectors);
    struct gm_file_header h;
    CHECK(gm_header_decode(changed, &h));
    h.count = UINT64_MAX;
    gm_header_encode(&h, changed);
    save(dir, 0, sizeof fixture_vectors, changed);
    CHECK(gm_store_open(dir, 64, 7, &st) == GM_E_FORMAT && !st);
    save(dir, 0, sizeof fixture_vectors, fixture_vectors);
    memcpy(changed, fixture_chunks, sizeof fixture_chunks);
    struct gm_chunk_rec invalid = {.doc = 9, .generation = 1};
    gm_record_encode(GM_CHUNKS, 64, 7, 0, &invalid, changed + GM_HEADER_BYTES);
    save(dir, 1, sizeof fixture_chunks, changed);
    CHECK(gm_store_open(dir, 64, 7, &st) == GM_E_FORMAT && !st);
    save(dir, 1, sizeof fixture_chunks, fixture_chunks);
    CHECK(gm_store_open(dir, 64, 7, &st) == GM_OK);
    gm_store_close(st);
    test_clean(dir);
}
static void position_binding_and_recovery(void) {
    char dir[64], path[128];
    test_dir(dir);
    struct gm_store *st = nullptr;
    uint8_t bits[16] = {0};
    CHECK(gm_store_open(dir, 64, 7, &st) == GM_OK);
    CHECK(gm_store_replace(st, "doc", 2, bits) == GM_OK);
    gm_store_close(st);
    snprintf(path, sizeof path, "%s/vectors.gm", dir);
    char *original = nullptr;
    size_t n;
    CHECK(gm_file_read(path, 4096, &original, &n) == GM_OK);
    CHECK(n == GM_HEADER_BYTES + 2 * 40);
    uint8_t changed[GM_HEADER_BYTES + 2 * 40];
    memcpy(changed, original, n);
    memcpy(changed + GM_HEADER_BYTES, original + GM_HEADER_BYTES + 40, 40);
    memcpy(changed + GM_HEADER_BYTES + 40, original + GM_HEADER_BYTES, 40);
    save(dir, 0, n, changed);
    CHECK(gm_store_open(dir, 64, 7, &st) == GM_E_FORMAT && !st);
    save(dir, 0, n, original);
    free(original);
    CHECK(gm_store_open(dir, 64, 7, &st) == GM_OK);
    uint8_t replacement[8] = {255};
    gm_test_io_after = 6;
    CHECK(gm_store_replace(st, "doc", 1, replacement) == GM_E_UNCERTAIN);
    gm_test_io_after = -1;
    gm_store_close(st);
    CHECK(gm_file_read(path, 4096, &original, &n) == GM_OK);
    original[GM_HEADER_BYTES] ^= 1;
    save(dir, 0, n, original);
    /* Valid undo marker, but a corrupt retained vector: recovery must stop
     * before overwriting even the first header or truncating the append. */
    CHECK(gm_store_open(dir, 64, 7, &st) == GM_E_FORMAT && !st);
    equal_file(dir, 0, n, original);
    original[GM_HEADER_BYTES] ^= 1;
    save(dir, 0, n, original);
    free(original);
    CHECK(gm_store_open(dir, 64, 7, &st) == GM_OK && gm_store_live_chunks(st) == 2);
    CHECK(gm_store_replace(st, "doc", 1, replacement) == GM_OK);
    gm_store_close(st);
    CHECK(gm_file_read(path, 4096, &original, &n) == GM_OK);
    original[GM_HEADER_BYTES] ^= 1; /* obsolete data is checked too */
    save(dir, 0, n, original);
    CHECK(gm_store_open(dir, 64, 7, &st) == GM_E_FORMAT && !st);
    free(original);
    test_clean(dir);
}
static void batch_boundary(void) {
    char dir[64];
    test_dir(dir);
    struct gm_store *st = nullptr;
    size_t bytes = GM_DIM_MAX / 8;
    uint8_t *v = calloc(9, bytes);
    CHECK(v);
    v[8 * bytes] = 255;
    CHECK(gm_store_open(dir, GM_DIM_MAX, 7, &st) == GM_OK);
    CHECK(gm_store_replace(st, "batch", 9, v) == GM_OK);
    gm_store_close(st);
    CHECK(gm_store_open(dir, GM_DIM_MAX, 7, &st) == GM_OK);
    struct gm_hit hit;
    size_t n;
    CHECK(gm_store_scan(st, 1, v + 8 * bytes, &hit, &n) == GM_OK && n == 1 && hit.chunk == 8);
    CHECK(gm_store_replace(st, "batch", 1, v + 8 * bytes) == GM_OK);
    CHECK(gm_store_compact(st) == GM_OK);
    gm_store_close(st);
    CHECK(gm_store_open(dir, GM_DIM_MAX, 7, &st) == GM_OK);
    CHECK(gm_store_scan(st, 1, v + 8 * bytes, &hit, &n) == GM_OK && n == 1 && hit.distance == 0);
    gm_store_close(st);
    free(v);
    test_clean(dir);
}
int main(void) {
    golden_and_corruption();
    position_binding_and_recovery();
    batch_boundary();
    puts("PASS: v2 golden bytes, all single-byte corruptions, position binding, recovery and "
         "batching");
}
