#define _POSIX_C_SOURCE 200809L
#include "test_support.h"
#include "gm_store.h"
#include "gm_platform.h"
#include <unistd.h>
/* Packet: four little-endian byte lengths, followed by the three data files
 * and optional undo journal. Files never contain caller-controlled paths. */
static char directory[64];
static const char *names[] = {"vectors.gm", "chunks.gm", "docs.gm", "undo.gm"};
static void cleanup(void) {
    test_clean(directory);
}
static void initialize(void) {
    if (!directory[0]) {
        test_dir(directory);
        CHECK(atexit(cleanup) == 0);
    }
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 16 || size > 65536)
        return 0;
    size_t lengths[4], offset = 16;
    for (size_t i = 0; i < 4; ++i) {
        lengths[i] = gm_u32(data + i * 4);
        if (lengths[i] > size - offset)
            return 0;
        offset += lengths[i];
    }
    if (offset != size)
        return 0;
    initialize();
    offset = 16;
    for (size_t i = 0; i < 4; ++i) {
        char path[128];
        snprintf(path, sizeof path, "%s/%s", directory, names[i]);
        if (i == 3 && !lengths[i]) {
            (void)unlink(path);
            continue;
        }
        FILE *f = fopen(path, "wb");
        CHECK(f);
        CHECK(fwrite(data + offset, 1, lengths[i], f) == lengths[i]);
        CHECK(fclose(f) == 0);
        offset += lengths[i];
    }
    struct gm_store *st = nullptr;
    enum gm_status result = gm_store_open_limited(directory, 64, 7, 1024u * 1024u, &st);
    if (result == GM_OK) {
        uint8_t query[8] = {0};
        struct gm_hit hits[3];
        size_t n = 0;
        CHECK(gm_store_scan(st, 3, query, hits, &n) == GM_OK && n <= 3);
        for (size_t i = 0; i < n; ++i)
            CHECK(gm_store_doc_path(st, hits[i].doc) != nullptr);
    } else
        CHECK(st == nullptr);
    gm_store_close(st);
    return 0;
}
#ifdef GM_FUZZ_STANDALONE
int main(int argc, char **argv) {
    initialize();
    struct gm_store *st = nullptr;
    CHECK(gm_store_open(directory, 64, 7, &st) == GM_OK);
    uint8_t vector[24] = {1, 2, 3};
    CHECK(gm_store_replace(st, "fixture", 3, vector) == GM_OK);
    vector[0] = 9;
    gm_test_io_after = 4;
    CHECK(gm_store_replace(st, "fixture", 3, vector) == GM_E_UNCERTAIN);
    gm_test_io_after = -1;
    gm_store_close(st);
    uint8_t seed[4096];
    size_t size = 16;
    for (size_t i = 0; i < 4; ++i) {
        char path[128];
        snprintf(path, sizeof path, "%s/%s", directory, names[i]);
        char *bytes = nullptr;
        size_t n = 0;
        CHECK(gm_file_read(path, 4096, &bytes, &n) == GM_OK);
        CHECK(n <= sizeof seed - size);
        gm_put32(seed + i * 4, (uint32_t)n);
        memcpy(seed + size, bytes, n);
        size += n;
        free(bytes);
    }
    if (argc == 2 && !strcmp(argv[1], "--seed")) {
        CHECK(fwrite(seed, 1, size, stdout) == size);
        return 0;
    }
    size_t rounds = argc == 2 ? (size_t)strtoul(argv[1], nullptr, 10) : 3000;
    uint32_t rng = 0x12345678;
    uint8_t input[4096];
    LLVMFuzzerTestOneInput(seed, size);
    for (size_t i = 0; i < rounds; ++i) {
        memcpy(input, seed, size);
        rng = rng * 1664525u + 1013904223u;
        size_t at = 16u + (rng % (size - 16u));
        input[at] ^= (uint8_t)(1u << ((rng >> 16) % 8));
        if (i % 8 == 0) {
            rng = rng * 1664525u + 1013904223u;
            at = rng % 16u;
            input[at] ^= (uint8_t)(rng >> 24);
        }
        LLVMFuzzerTestOneInput(input, size);
    }
    printf("PASS: %zu deterministic corrupted-store inputs\n", rounds);
    return 0;
}
#endif
