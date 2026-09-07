#define _POSIX_C_SOURCE 200809L
#include "gm_engine.h"
#include "gm_platform.h"
#include "quality.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#ifndef GM_RETRIEVAL_HEADER
#define GM_RETRIEVAL_HEADER "retrieval_cases.h"
#endif
#include GM_RETRIEVAL_HEADER
#ifndef RETRIEVAL_CORPUS
#define RETRIEVAL_CORPUS "geist-memory-retrieval-v1"
#endif
static size_t truncated_inputs;
static bool truncate_inputs;

static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int compare_time(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static enum gm_status embed(struct gm_engine *engine, size_t dim, const char *text,
                            const float **vector, size_t *tokens) {
    static int32_t ids[GM_TOKENS + 1];
    enum gm_status s = gm_engine_tokenize(engine, text, GM_TOKENS + 1, ids, tokens);
    if (s != GM_OK)
        return s;
    if (*tokens > GM_TOKENS || (*tokens > GM_WINDOW && !truncate_inputs))
        return GM_E_TOO_LONG;
    if (*tokens > GM_WINDOW)
        ++truncated_inputs;
    return gm_engine_embed(engine, *tokens > GM_WINDOW ? GM_WINDOW : *tokens, ids, dim, vector);
}
struct metrics {
    size_t queries, float_at_1, float_at_3, binary_at_1, binary_at_3;
    double float_rr, binary_rr, overlap;
};
static void record(struct metrics *m, const struct quality_result *r) {
    ++m->queries;
    m->float_at_1 += r->float_rank == 1;
    m->float_at_3 += r->float_rank <= 3;
    m->binary_at_1 += r->binary_rank == 1;
    m->binary_at_3 += r->binary_rank <= 3;
    m->float_rr += 1.0 / (double)r->float_rank;
    m->binary_rr += 1.0 / (double)r->binary_rank;
    m->overlap += r->overlap_at_3;
}
static void report(const char *language, const struct metrics *m) {
    if (!m->queries)
        return;
    double n = (double)m->queries;
    printf("language=%s queries=%zu float_recall_at_1=%.4f float_recall_at_3=%.4f "
           "float_mrr=%.4f binary_recall_at_1=%.4f binary_recall_at_3=%.4f "
           "binary_mrr=%.4f top3_overlap=%.4f\n",
           language, m->queries, (double)m->float_at_1 / n, (double)m->float_at_3 / n,
           m->float_rr / n, (double)m->binary_at_1 / n, (double)m->binary_at_3 / n,
           m->binary_rr / n, m->overlap / n);
}
int main(void) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char *truncate = getenv("GM_TRUNCATE");
    if (truncate && strcmp(truncate, "0") && strcmp(truncate, "1"))
        return 2;
    truncate_inputs = truncate && !strcmp(truncate, "1");
    const char *path = getenv("GEIST_EMBED_GGUF_PATH");
    const char *prefix = getenv("GM_QUERY_PREFIX");
    const char *bos = getenv("GM_OMIT_BOS"), *eos = getenv("GM_OMIT_EOS");
    prefix = prefix ? prefix : "query: ";
    if (!path || !path[0] || strlen(prefix) > GM_PREFIX_MAX ||
        (bos && strcmp(bos, "0") && strcmp(bos, "1")) ||
        (eos && strcmp(eos, "0") && strcmp(eos, "1"))) {
        fputs("Set GEIST_EMBED_GGUF_PATH; optional GM_QUERY_PREFIX and GM_OMIT_BOS/EOS=0|1\n",
              stderr);
        return 2;
    }
    bool omit_bos = bos && !strcmp(bos, "1"), omit_eos = eos && !strcmp(eos, "1");
    struct gm_engine *engine = nullptr;
    float *documents = nullptr;
    uint8_t digest[32];
    double start = now();
    enum gm_status s = gm_file_digest(path, digest);
    double hash_s = now() - start;
    if (s != GM_OK)
        goto done;
    size_t dim = 0;
    start = now();
    s = gm_engine_open(path, omit_bos, omit_eos, &engine, &dim);
    double open_s = now() - start;
    if (s != GM_OK)
        goto done;
    constexpr size_t count = sizeof retrieval_docs / sizeof *retrieval_docs;
    constexpr size_t queries = sizeof retrieval_queries / sizeof *retrieval_queries;
    static_assert(count <= QUALITY_DOC_MAX);
    if (!dim || dim % 8 || dim > GM_DIM_MAX) {
        s = GM_E_ENGINE;
        goto done;
    }
    /* count <= 512 and dim <= 65536 bound this allocation to 128 MiB. */
    documents = malloc(count * dim * sizeof *documents);
    if (!documents) {
        s = GM_E_OOM;
        goto done;
    }
#ifdef GM_MODEL_MOCK
    puts("model_source=mock (harness test; not retrieval evidence)");
#else
    puts("model_source=GGUF");
#endif
    printf("model_sha256=");
    for (size_t i = 0; i < sizeof digest; ++i)
        printf("%02x", digest[i]);
    printf("\ncorpus=%s documents=%zu dimension=%zu "
           "window=%d overlap=%d omit_bos=%d omit_eos=%d\nquery_prefix=%s\n",
           RETRIEVAL_CORPUS, count, dim, GM_WINDOW, GM_OVERLAP, omit_bos, omit_eos, prefix);
    double doc_start = now();
    size_t doc_tokens = 0, query_tokens = 0, processed_doc_tokens = 0, processed_query_tokens = 0;
    for (size_t i = 0; i < count; ++i) {
        const float *v = nullptr;
        size_t n = 0;
        s = embed(engine, dim, retrieval_docs[i], &v, &n);
        if (s != GM_OK)
            goto done;
        memcpy(documents + i * dim, v, dim * sizeof *v);
        doc_tokens += n;
        processed_doc_tokens += n > GM_WINDOW ? GM_WINDOW : n;
        if ((i + 1) % 16 == 0)
            printf("embedded_documents=%zu/%zu\n", i + 1, count);
    }
    double docs_s = now() - doc_start, times[queries];
    struct metrics metrics[3] = {0}; /* all, de, en */
    for (size_t i = 0; i < queries; ++i) {
        char query[GM_QUERY_MAX + 1];
        int len = snprintf(query, sizeof query, "%s%s", prefix, retrieval_queries[i].text);
        if (len < 0 || (size_t)len >= sizeof query) {
            s = GM_E_TOO_LONG;
            goto done;
        }
        const float *v = nullptr;
        size_t n = 0;
        start = now();
        s = embed(engine, dim, query, &v, &n);
        times[i] = now() - start;
        if (s != GM_OK)
            goto done;
        query_tokens += n;
        processed_query_tokens += n > GM_WINDOW ? GM_WINDOW : n;
        struct quality_result r;
        if (!quality_compare(count, dim, documents, v, retrieval_queries[i].relevant, &r)) {
            s = GM_E_ENGINE;
            goto done;
        }
        printf("query=%zu language=%s relevant=%zu float_rank=%zu binary_rank=%zu\n", i,
               retrieval_queries[i].language, retrieval_queries[i].relevant, r.float_rank,
               r.binary_rank);
        record(&metrics[0], &r);
        record(&metrics[!strcmp(retrieval_queries[i].language, "de") ? 1 : 2], &r);
    }
    printf("processed_document_tokens=%zu processed_query_tokens=%zu\n", processed_doc_tokens,
           processed_query_tokens);
    printf("truncation=%s truncated_inputs=%zu input_tokens_include_truncated=1\n",
           truncate_inputs ? "first-256-content-tokens" : "reject", truncated_inputs);
    report("all", &metrics[0]);
    report("de", &metrics[1]);
    report("en", &metrics[2]);
    double query_s = 0;
    for (size_t i = 0; i < queries; ++i)
        query_s += times[i];
    qsort(times, queries, sizeof *times, compare_time);
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        s = GM_E_IO;
        goto done;
    }
#if defined(__APPLE__)
    double rss_mib = (double)usage.ru_maxrss / 1048576;
#else
    double rss_mib = (double)usage.ru_maxrss / 1024;
#endif
    printf("hash_s=%.6f engine_open_s=%.6f document_embed_s=%.6f document_tokens=%zu "
           "query_embed_s=%.6f query_tokens=%zu query_p50_ms=%.3f query_p95_ms=%.3f "
           "peak_process_rss_mib=%.2f\n",
           hash_s, open_s, docs_s, doc_tokens, query_s, query_tokens,
           times[(queries - 1) / 2] * 1000, times[(queries * 95 + 99) / 100 - 1] * 1000, rss_mib);
done:
    free(documents);
    gm_engine_close(engine);
    if (s != GM_OK)
        fprintf(stderr, "model benchmark failed: %s\n", gm_status_str(s));
    return s == GM_OK ? 0 : 1;
}
