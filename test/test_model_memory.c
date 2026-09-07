#define _POSIX_C_SOURCE 200809L
#include "geist_memory.h"
#include "gm_platform.h"
#include "model_alloc.h"
#include "test_support.h"
#include <limits.h>
#include <errno.h>
#include <sys/resource.h>

static const char *phase;
static long fail_at;
static size_t minimum;
static bool injected;
static void begin(const char *name) {
    model_alloc_begin(!strcmp(phase, name) ? fail_at : -1, minimum);
}
static void end(const char *name, enum gm_status status) {
    struct model_alloc_stats s = model_alloc_end();
    injected |= s.failed != 0;
    printf("phase=%s status=%d live_bytes=%zu peak_bytes=%zu calls=%zu eligible=%zu "
           "largest_bytes=%zu injected=%zu\n",
           name, status, s.live, s.peak, s.calls, s.eligible, s.largest, s.failed);
    CHECK(status == GM_OK || (s.failed && (status == GM_E_OOM || status == GM_E_ENGINE)));
}
static void digest_store(const char *dir, uint8_t digest[3][32]) {
    const char *names[] = {"docs.gm", "chunks.gm", "vectors.gm"};
    for (size_t i = 0; i < 3; ++i) {
        char path[256];
        CHECK(snprintf(path, sizeof path, "%s/%s", dir, names[i]) > 0);
        CHECK(gm_file_digest(path, digest[i]) == GM_OK);
    }
}
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char *model = getenv("GEIST_EMBED_GGUF_PATH");
    CHECK(model && argc == 4);
    phase = argv[1];
    CHECK(!strcmp(phase, "none") || !strcmp(phase, "open") || !strcmp(phase, "remember") ||
          !strcmp(phase, "recall") || !strcmp(phase, "compact"));
    char *tail;
    errno = 0;
    fail_at = strtol(argv[2], &tail, 10);
    CHECK(!errno && !*tail && fail_at >= -1);
    errno = 0;
    unsigned long min = strtoul(argv[3], &tail, 10);
    CHECK(!errno && !*tail && min <= SIZE_MAX);
    minimum = (size_t)min;
    char dir[64];
    test_dir(dir);
    struct gm_opts opts = {.query_prefix = "query: ", .omit_bos = true};
    struct gm *m = nullptr;
    begin("open");
    enum gm_status s = gm_open(dir, model, &opts, &m);
    end("open", s);
    if (s != GM_OK) {
        CHECK(!m);
        goto done;
    }
    CHECK(gm_remember_text(m, "document", "Yeast produces gas that makes bread rise.") == GM_OK);
    uint8_t before[3][32], after[3][32];
    digest_store(dir, before);
    begin("remember");
    s = gm_remember_text(m, "document", "The moon causes ocean tides.");
    end("remember", s);
    if (s != GM_OK) {
        digest_store(dir, after);
        CHECK(!memcmp(before, after, sizeof before));
    }
    /* Successful fallbacks must produce the same packed vectors as a normal retry. */
    if (s == GM_OK)
        digest_store(dir, before);
    CHECK(gm_remember_text(m, "document", "The moon causes ocean tides.") == GM_OK);
    if (s == GM_OK) {
        digest_store(dir, after);
        CHECK(!memcmp(before, after, sizeof before));
    }
    struct gm_hit hit = {0};
    size_t n = 123;
    begin("recall");
    s = gm_recall(m, 1, "What causes tides?", &hit, &n);
    end("recall", s);
    CHECK(s == GM_OK ? n == 1 : n == 0);
    struct gm_hit previous = hit;
    CHECK(gm_recall(m, 1, "What causes tides?", &hit, &n) == GM_OK && n == 1);
    if (s == GM_OK)
        CHECK(hit.doc == previous.doc && hit.chunk == previous.chunk &&
              hit.distance == previous.distance);
    digest_store(dir, before);
    begin("compact");
    s = gm_compact(m);
    end("compact", s);
    if (s != GM_OK) {
        digest_store(dir, after);
        CHECK(!memcmp(before, after, sizeof before));
    }
    gm_close(m);
    m = nullptr;
    CHECK(gm_open(dir, model, &opts, &m) == GM_OK);
    CHECK(gm_recall(m, 1, "What causes tides?", &hit, &n) == GM_OK && n == 1);
    gm_close(m);
    m = nullptr;
done:
    CHECK(fail_at < 0 || injected);
    size_t remaining = model_alloc_live();
    struct rusage usage;
    CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
    printf("after_close_live_bytes=%zu peak_rss_kib=%ld\n", remaining, usage.ru_maxrss);
    CHECK(remaining == 0);
    test_clean(dir);
    puts("PASS: real model lifetime, recovery and requested-allocation cleanup");
}
