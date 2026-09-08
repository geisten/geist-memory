/*
 * test_gm_e2e — the whole library, end to end, against a real model.
 *
 * Point GEIST_EMBED_GGUF_PATH at a bitnet-embedding GGUF. Skips without it.
 *
 * The checks are the ones that would actually break:
 *
 *   - RETRIEVAL. Three documents on unrelated subjects, three questions
 *     that name none of their words. Each must find its own document. A
 *     store that indexes and searches without crashing still fails this if
 *     the pooling, the packing or the distance is wrong.
 *   - PERSISTENCE. Close, reopen, ask again: same answers. The store is
 *     the point; an in-memory index that evaporates is not one.
 *   - IDEMPOTENCE. Re-indexing an unchanged file adds no chunks.
 *   - RE-INDEX. A changed file's old chunks stop matching and the new
 *     content is findable, without a compaction pass.
 *   - MODEL BINDING. A store built for one model refuses another.
 *   - CHUNKING. A document past one chunk window produces several chunks
 *     and stays retrievable.
 *
 * No framework: assertions are counted and the exit code carries the
 * verdict. 77 = skip, matching geistlib's runner.
 */
#define _POSIX_C_SOURCE 200809L

#include "geist_memory.h"
#include "gm_store.h"
#include "test_support.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { T_PASS = 0, T_FAIL = 1, T_SKIP = 77 };

static int fails = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

static char DIR[64];
static char yeast[128], tides[128], mortgage[128];

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(T_FAIL);
    }
    CHECK(fputs(text, f) >= 0);
    CHECK(fclose(f) == 0);
}

/* Best hit's document, or nullptr. */
static const char *ask(struct gm *m, const char *q, uint32_t *out_distance) {
    struct gm_hit hits[5];
    size_t n = 0;
    enum gm_status s = gm_recall(m, 5, q, hits, &n);
    if (s != GM_OK) {
        fprintf(stderr, "recall failed: %s\n", gm_status_str(s));
        return nullptr;
    }
    if (n == 0) {
        return nullptr;
    }
    if (out_distance != nullptr) {
        *out_distance = hits[0].distance;
    }
    return gm_doc_path(m, hits[0].doc);
}

static bool ends_with(const char *s, const char *suffix) {
    if (s == nullptr) {
        return false;
    }
    const size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

int main(void) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char *model = getenv("GEIST_EMBED_GGUF_PATH");
    if (model == nullptr || model[0] == '\0') {
        printf("SKIP: set GEIST_EMBED_GGUF_PATH to a bitnet-embedding GGUF\n");
        return T_SKIP;
    }

    test_dir(DIR);
    snprintf(yeast, sizeof yeast, "%s/doc_yeast.md", DIR);
    snprintf(tides, sizeof tides, "%s/doc_tides.md", DIR);
    snprintf(mortgage, sizeof mortgage, "%s/doc_mortgage.md", DIR);

    write_file(yeast, "# Bread\n\nYeast ferments the sugars in dough. The carbon dioxide it "
                      "releases inflates the gluten network, and that is what lifts a loaf.\n");
    write_file(tides, "# Tides\n\nThe moon's gravity pulls the oceans toward it. Earth's rotation "
                      "carries every coastline through the resulting bulges twice a day.\n");
    write_file(mortgage, "# Amortisation\n\nEach payment first covers the interest accrued on the "
                         "outstanding balance. Whatever is left reduces the principal.\n");

    const struct gm_opts opts = {.query_prefix = "query: "};
    struct gm *m = nullptr;
    enum gm_status s = gm_open(DIR, model, &opts, &m);
    if (s != GM_OK) {
        fprintf(stderr, "gm_open failed: %s\n", gm_status_str(s));
        return T_FAIL;
    }
    printf("  model dim=%zu bits (%zu bytes/vector)\n", gm_dim(m), gm_dim(m) / 8u);

    check(gm_remember_file(m, yeast) == GM_OK, "index doc 1");
    check(gm_remember_file(m, tides) == GM_OK, "index doc 2");
    check(gm_remember_file(m, mortgage) == GM_OK, "index doc 3");
    const size_t after_first = gm_chunk_count(m);
    printf("  indexed 3 documents into %zu chunks\n", after_first);
    check(after_first == 3, "three short documents make three chunks");

    /* ---- retrieval: none of these questions shares vocabulary with its
     * answer beyond the odd stopword. */
    struct {
        const char *q;
        const char *want;
    } cases[] = {
        {"why does my loaf not rise?", "doc_yeast.md"},
        {"what makes the sea level change twice a day?", "doc_tides.md"},
        {"how much of my instalment pays down the debt?", "doc_mortgage.md"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t d = 0;
        const char *got = ask(m, cases[i].q, &d);
        printf("  \"%s\" -> %s (d=%u)\n", cases[i].q, got ? got : "(nothing)", d);
        check(ends_with(got, cases[i].want), "the question finds its own document");
    }

    /* ---- idempotence ---------------------------------------------------- */
    check(gm_remember_file(m, yeast) == GM_OK, "re-index unchanged file");
    check(gm_chunk_count(m) == after_first, "an unchanged file adds no chunks");

    /* ---- persistence ---------------------------------------------------- */
    gm_close(m);
    m = nullptr;
    s = gm_open(DIR, model, &opts, &m);
    check(s == GM_OK, "the store reopens");
    if (s != GM_OK) {
        fprintf(stderr, "reopen failed: %s\n", gm_status_str(s));
        return T_FAIL;
    }
    check(gm_chunk_count(m) == after_first, "reopening restores every chunk");
    check(ends_with(ask(m, "why does my loaf not rise?", nullptr), "doc_yeast.md"),
          "answers survive a close/reopen — the store, not the process, holds the memory");

    /* ---- re-index: old content must stop matching ----------------------- */
    write_file(yeast, "# Kites\n\nA kite flies because the wind deflected by its surface pushes "
                      "back. The line holds it against that push at an angle of attack.\n");
    check(gm_remember_file(m, yeast) == GM_OK, "re-index a changed file");
    const char *now = ask(m, "what keeps a kite in the air?", nullptr);
    printf("  after rewrite: \"what keeps a kite in the air?\" -> %s\n", now ? now : "(nothing)");
    check(ends_with(now, "doc_yeast.md"), "the new content is findable");
    const char *stale = ask(m, "why does my loaf not rise?", nullptr);
    printf("  the old question now lands on: %s\n", stale ? stale : "(nothing)");
    /* Ranking the new content for an old query is model-dependent. Assert
     * generation retirement through counts; exact stale-record exclusion is
     * independently tested against the store reference scan. */
    struct gm_stats stats;
    check(gm_get_stats(m, &stats) == GM_OK && stats.obsolete_chunks == 1,
          "the previous generation is obsolete");
    check(gm_chunk_count(m) == after_first,
          "one chunk died and one was born — the live count is unchanged");

    /* ---- chunking ------------------------------------------------------- */
    {
        /* Comfortably past one 256-token window. */
        static char big[24000];
        size_t o = 0;
        for (int i = 0; i < 24 && o < sizeof big - 200; i++) {
            o += (size_t)snprintf(big + o, sizeof big - o,
                                  "Paragraph %d discusses the migration of arctic terns "
                                  "between the two polar summers. ",
                                  i);
        }
        const size_t before = gm_chunk_count(m);
        check(gm_remember_text(m, "terns", big) == GM_OK, "index a long text");
        const size_t added = gm_chunk_count(m) - before;
        printf("  long text -> %zu chunks\n", added);
        check(added > 1, "a document past one window is split");
        check(ends_with(ask(m, "which bird crosses between both polar summers?", nullptr), "terns"),
              "a chunked document is still retrievable");
    }

    struct gm_hit before[5], after[5];
    size_t nb = 0, na = 0;
    check(gm_recall(m, 5, "what keeps a kite in the air?", before, &nb) == GM_OK,
          "recall before compaction");
    check(gm_compact(m) == GM_OK, "compact real embeddings");
    check(gm_get_stats(m, &stats) == GM_OK && stats.obsolete_chunks == 0,
          "compaction removes obsolete vectors");
    gm_close(m);
    m = nullptr;
    check(gm_open(DIR, model, &opts, &m) == GM_OK, "reopen compacted embeddings");
    check(gm_recall(m, 5, "what keeps a kite in the air?", after, &na) == GM_OK && na == nb,
          "recall after compaction");
    for (size_t i = 0; i < na && i < nb; ++i)
        check(before[i].doc == after[i].doc && before[i].chunk == after[i].chunk &&
                  before[i].distance == after[i].distance,
              "compaction preserves ordered hits");
    gm_close(m);

    /* Preserve the header checksum while changing the recorded model identity.
     * The mismatch must be reported as GM_E_MODEL before reading vectors. */
    {
        char vpath[600];
        snprintf(vpath, sizeof vpath, "%s/vectors.gm", DIR);
        FILE *f = fopen(vpath, "r+b");
        check(f != nullptr, "the store's vector file is readable");
        if (f != nullptr) {
            uint8_t header[GM_HEADER_BYTES];
            struct gm_file_header saved;
            CHECK(fseek(f, 0, SEEK_SET) == 0 && fread(header, sizeof header, 1, f) == 1 &&
                  gm_header_decode(header, &saved));
            saved.model_fp ^= 1;
            gm_header_encode(&saved, header);
            CHECK(fseek(f, 0, SEEK_SET) == 0 && fwrite(header, sizeof header, 1, f) == 1);
            fclose(f);

            struct gm *other = nullptr;
            const enum gm_status os = gm_open(DIR, model, &opts, &other);
            printf("  store built by another model -> %s\n", gm_status_str(os));
            check(os == GM_E_MODEL,
                  "a store from a different model is refused, not silently mixed");
            if (os == GM_OK) {
                gm_close(other);
            }
        }
    }

    test_clean(DIR);
    if (fails > 0) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return T_FAIL;
    }
    printf("geist-memory: index, recall, persist, re-index, chunk — all pass\n");
    return T_PASS;
}
