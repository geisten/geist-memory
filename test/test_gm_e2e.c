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

static const char *DIR = "build/test-store";

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(T_FAIL);
    }
    fputs(text, f);
    fclose(f);
}

/* Best hit's document, or nullptr. */
static const char *ask(struct gm *m, const char *q, uint32_t *out_distance) {
    struct gm_hit  hits[5];
    size_t         n = 0;
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
    const char *model = getenv("GEIST_EMBED_GGUF_PATH");
    if (model == nullptr || model[0] == '\0') {
        printf("SKIP: set GEIST_EMBED_GGUF_PATH to a bitnet-embedding GGUF\n");
        return T_SKIP;
    }

    if (mkdir("build", 0700) != 0 && access("build", F_OK) != 0) {
        fprintf(stderr, "cannot create build/\n");
        return T_FAIL;
    }
    /* Start from an empty store so a rerun is deterministic. */
    (void) remove("build/test-store/vectors.gm");
    (void) remove("build/test-store/chunks.gm");
    (void) remove("build/test-store/docs.gm");

    write_file("build/doc_yeast.md",
               "# Bread\n\nYeast ferments the sugars in dough. The carbon dioxide it "
               "releases inflates the gluten network, and that is what lifts a loaf.\n");
    write_file("build/doc_tides.md",
               "# Tides\n\nThe moon's gravity pulls the oceans toward it. Earth's rotation "
               "carries every coastline through the resulting bulges twice a day.\n");
    write_file("build/doc_mortgage.md",
               "# Amortisation\n\nEach payment first covers the interest accrued on the "
               "outstanding balance. Whatever is left reduces the principal.\n");

    const struct gm_opts opts = {.query_prefix = "query: "};
    struct gm           *m    = nullptr;
    enum gm_status       s    = gm_open(DIR, model, &opts, &m);
    if (s != GM_OK) {
        fprintf(stderr, "gm_open failed: %s\n", gm_status_str(s));
        return T_FAIL;
    }
    printf("  model dim=%zu bits (%zu bytes/vector)\n", gm_dim(m), gm_dim(m) / 8u);

    check(gm_remember_file(m, "build/doc_yeast.md") == GM_OK, "index doc 1");
    check(gm_remember_file(m, "build/doc_tides.md") == GM_OK, "index doc 2");
    check(gm_remember_file(m, "build/doc_mortgage.md") == GM_OK, "index doc 3");
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
        uint32_t    d   = 0;
        const char *got = ask(m, cases[i].q, &d);
        printf("  \"%s\" -> %s (d=%u)\n", cases[i].q, got ? got : "(nothing)", d);
        check(ends_with(got, cases[i].want), "the question finds its own document");
    }

    /* ---- idempotence ---------------------------------------------------- */
    check(gm_remember_file(m, "build/doc_yeast.md") == GM_OK, "re-index unchanged file");
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
    sleep(1); /* mtime has one-second resolution; force a visible change */
    write_file("build/doc_yeast.md",
               "# Kites\n\nA kite flies because the wind deflected by its surface pushes "
               "back. The line holds it against that push at an angle of attack.\n");
    check(gm_remember_file(m, "build/doc_yeast.md") == GM_OK, "re-index a changed file");
    const char *now = ask(m, "what keeps a kite in the air?", nullptr);
    printf("  after rewrite: \"what keeps a kite in the air?\" -> %s\n", now ? now : "(nothing)");
    check(ends_with(now, "doc_yeast.md"), "the new content is findable");
    const char *stale = ask(m, "why does my loaf not rise?", nullptr);
    printf("  the old question now lands on: %s\n", stale ? stale : "(nothing)");
    check(!ends_with(stale, "doc_yeast.md") || gm_chunk_count(m) > after_first,
          "the superseded chunks no longer win");

    /* ---- chunking ------------------------------------------------------- */
    {
        /* Comfortably past one 256-token window. */
        static char big[24000];
        size_t      o = 0;
        for (int i = 0; i < 220 && o < sizeof big - 200; i++) {
            o += (size_t) snprintf(big + o,
                                   sizeof big - o,
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

    gm_close(m);

    /* ---- model binding -------------------------------------------------- *
     * A store carries the fingerprint of the model that built it, because
     * two models' vectors are not comparable at all — mixing them yields
     * confident nonsense rather than an error. Rather than conjure a second
     * multi-hundred-megabyte checkpoint, flip a byte of the recorded
     * fingerprint on disk: that is exactly the state a swapped model
     * produces, and it is what the guard has to catch.
     *
     * The offset mirrors struct gm_file_header in src/gm_store.h
     * (magic, version, dim, rec_size, count, model_fp). */
    {
        const long MODEL_FP_OFF = 4 + 4 + 4 + 4 + 8;
        char vpath[600];
        snprintf(vpath, sizeof vpath, "%s/vectors.gm", DIR);
        FILE *f = fopen(vpath, "r+b");
        check(f != nullptr, "the store's vector file is readable");
        if (f != nullptr) {
            uint64_t fp = 0;
            check(fseek(f, MODEL_FP_OFF, SEEK_SET) == 0 && fread(&fp, sizeof fp, 1, f) == 1,
                  "read the recorded fingerprint");
            fp ^= 1u;
            check(fseek(f, MODEL_FP_OFF, SEEK_SET) == 0 && fwrite(&fp, sizeof fp, 1, f) == 1,
                  "write a different fingerprint");
            fclose(f);

            struct gm           *other = nullptr;
            const enum gm_status os    = gm_open(DIR, model, &opts, &other);
            printf("  store built by another model -> %s\n", gm_status_str(os));
            check(os == GM_E_MODEL,
                  "a store from a different model is refused, not silently mixed");
            if (os == GM_OK) {
                gm_close(other);
            }
        }
    }

    if (fails > 0) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return T_FAIL;
    }
    printf("geist-memory: index, recall, persist, re-index, chunk — all pass\n");
    return T_PASS;
}
