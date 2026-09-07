#define _POSIX_C_SOURCE 200809L

#include "gm_store.h"

#include "gm_hash.h"
#include "gm_internal.h"
#include "gm_platform.h"
#include <math.h>

#include <stdlib.h>
#include <string.h>

/* Chunking is in TOKENS, not characters: the model's window is a token
 * budget, and a character split lands mid-word in exactly the languages
 * where that costs the most. The overlap keeps a sentence that straddles a
 * boundary retrievable from either side. */
/* Independent byte/token admission limits bound work before allocation.
 * Their ratio is a resource policy, not an assumption about tokenization. */
static constexpr size_t GM_MAX_DOC_TOKENS = GM_TOKENS;
static constexpr size_t GM_MAX_DOC_BYTES = GM_TEXT_MAX;

struct gm {
    struct gm_store *store;
    struct gm_embedder *engine;
    char query_prefix[128];

    size_t dim;    /* embedding width in bits, = the store's */
    uint8_t *bits; /* [dim/8] its packed form */
    int32_t *ids;  /* [GM_MAX_DOC_TOKENS] whole-document tokens */
};

const char *gm_status_str(enum gm_status s) {
    switch (s) {
    case GM_OK:
        return "ok";
    case GM_E_INVALID_ARG:
        return "invalid argument";
    case GM_E_IO:
        return "I/O error";
    case GM_E_OOM:
        return "out of memory";
    case GM_E_FORMAT:
        return "store is corrupt or not a geist-memory store";
    case GM_E_MODEL:
        return "store belongs to a different embedding model";
    case GM_E_TOO_LONG:
        return "path or document exceeds the limit";
    case GM_E_ENGINE:
        return "the geist engine refused";
    case GM_E_BUSY:
        return "store already open";
    case GM_E_LIMIT:
        return "store memory budget exceeded";
    case GM_E_UNCERTAIN:
        return "I/O outcome uncertain; close and reopen";
    }
    return "unknown";
}

/* Pack finite float signs; this lossy representation uses 1/32 of float storage. */
enum gm_status gm_pack(size_t dim, const float *v, uint8_t *out) {
    if (!v || !out || !dim || dim % 8u || dim > GM_DIM_MAX)
        return GM_E_INVALID_ARG;
    memset(out, 0, dim / 8u);
    for (size_t i = 0; i < dim; i++) {
        if (!isfinite(v[i]))
            return GM_E_ENGINE;
        if (v[i] > 0.0f) {
            out[i >> 3] |= (uint8_t)(1u << (i & 7u));
        }
    }
    return GM_OK;
}

/* The adapter applies the explicit BOS/EOS policy selected at open. */
static enum gm_status embed_window(struct gm *m, size_t n, const int32_t *ids) {
    const float *vector = nullptr;
    enum gm_status s = gm_embedder_embed(m->engine, n, ids, m->dim, &vector);
    return s == GM_OK ? gm_pack(m->dim, vector, m->bits) : s;
}

/* The one funnel every document takes, and therefore the one place its
 * limits are enforced — before the file is read, tokenized or embedded. */
static enum gm_status index_text(struct gm *m, const char *id, const char *text, size_t len) {
    if (!m || !id || !id[0] || (!text && len))
        return GM_E_INVALID_ARG;
    if (gm_store_path_too_long(id) || len > GM_MAX_DOC_BYTES)
        return GM_E_TOO_LONG;
    if (len && memchr(text, 0, len))
        return GM_E_INVALID_ARG;
    struct gm_stats stats;
    enum gm_status s = gm_store_stats(m->store, &stats);
    if (s != GM_OK)
        return s;
    char label[GM_PATH_MAX];
    memcpy(label, id, strlen(id) + 1u);
    char *terminated = gm_alloc(len + 1u);
    if (!terminated)
        return GM_E_OOM;
    if (len)
        memcpy(terminated, text, len);
    terminated[len] = 0;
    size_t n_ids = 0;
    s = gm_embedder_tokenize(m->engine, terminated, GM_MAX_DOC_TOKENS + 1u, m->ids, &n_ids);
    free(terminated);
    if (s != GM_OK)
        return s;
    if (n_ids > GM_MAX_DOC_TOKENS)
        return GM_E_TOO_LONG;
    const size_t stride = GM_WINDOW - GM_OVERLAP;
    const size_t count = n_ids == 0           ? 0
                         : n_ids <= GM_WINDOW ? 1
                                              : 1u + (n_ids - GM_WINDOW + stride - 1u) / stride;
    size_t bytes;
    if (ckd_mul(&bytes, count, m->dim / 8u))
        return GM_E_TOO_LONG;
    uint8_t *vectors = count ? gm_alloc(bytes) : nullptr;
    if (count && !vectors)
        return GM_E_OOM;
    for (size_t i = 0, off = 0; i < count; ++i, off += stride) {
        const size_t take = n_ids - off < GM_WINDOW ? n_ids - off : GM_WINDOW;
        s = embed_window(m, take, m->ids + off);
        if (s != GM_OK)
            break;
        memcpy(vectors + i * (m->dim / 8u), m->bits, m->dim / 8u);
    }
    if (s == GM_OK)
        s = gm_store_replace(m->store, label, count, vectors);
    free(vectors);
    return s;
}

enum gm_status gm_open(const char *dir, const char *model_path, const struct gm_opts *opts,
                       struct gm **out) {
    if (out)
        *out = nullptr;
    if (!dir || !dir[0] || !model_path || !model_path[0] || !out)
        return GM_E_INVALID_ARG;
    const char *prefix = opts && opts->query_prefix ? opts->query_prefix : "";
    if (strnlen(prefix, GM_PREFIX_MAX + 1u) > GM_PREFIX_MAX)
        return GM_E_TOO_LONG;
    uint8_t digest[32];
    enum gm_status s = gm_file_digest(model_path, digest);
    if (s != GM_OK)
        return s;
    struct gm *m = gm_zero(sizeof *m);
    if (!m)
        return GM_E_OOM;
    strcpy(m->query_prefix, prefix);
    const bool omit_bos = opts && opts->omit_bos, omit_eos = opts && opts->omit_eos;
    s = gm_embedder_open(model_path, omit_bos, omit_eos, &m->engine, &m->dim);
    if (s != GM_OK)
        goto fail;
    if (!m->dim || m->dim % 8u || m->dim > GM_DIM_MAX) {
        s = GM_E_ENGINE;
        goto fail;
    }
    m->ids = gm_alloc((GM_MAX_DOC_TOKENS + 1u) * sizeof *m->ids);
    m->bits = gm_alloc(m->dim / 8u);
    if (!m->ids || !m->bits) {
        s = GM_E_OOM;
        goto fail;
    }
    /* Persist the content-derived identity in the existing 64-bit field.
     * Version/policy separation makes old timestamp-based stores mismatch. */
    struct gm_hash hash;
    gm_hash_init(&hash);
    gm_hash_update(&hash, sizeof digest, digest);
    const uint8_t policy[] = {2, 0, 1, 64, (uint8_t)omit_bos, (uint8_t)omit_eos};
    gm_hash_update(&hash, sizeof policy, policy);
    gm_hash_update(&hash, strlen(prefix), prefix);
    gm_hash_final(&hash, digest);
    uint64_t fingerprint = gm_u64(digest);
    s = gm_store_open_limited(dir, m->dim, fingerprint, opts ? opts->max_store_bytes : 0,
                              &m->store);
    if (s != GM_OK)
        goto fail;
    *out = m;
    return GM_OK;
fail:
    gm_close(m);
    return s;
}

void gm_close(struct gm *m) {
    if (m == nullptr) {
        return;
    }
    gm_store_close(m->store);
    gm_embedder_close(m->engine);
    free(m->bits);
    free(m->ids);
    free(m);
}

enum gm_status gm_remember_file(struct gm *m, const char *path) {
    if (!m || !path || !path[0])
        return GM_E_INVALID_ARG;
    if (gm_store_path_too_long(path))
        return GM_E_TOO_LONG;
    char *text = nullptr;
    size_t len = 0;
    enum gm_status s = gm_file_read(path, GM_MAX_DOC_BYTES, &text, &len);
    if (s == GM_OK)
        s = index_text(m, path, text, len);
    free(text);
    return s;
}

enum gm_status gm_remember_text_n(struct gm *m, const char *id, size_t len, const char *text) {
    return index_text(m, id, text, len);
}

enum gm_status gm_remember_text(struct gm *m, const char *id, const char *text) {
    if (!text)
        return GM_E_INVALID_ARG;
    return index_text(m, id, text, strnlen(text, GM_MAX_DOC_BYTES + 1u));
}

enum gm_status gm_recall(struct gm *m, size_t k, const char *query, struct gm_hit *out,
                         size_t *n_out) {
    if (n_out)
        *n_out = 0;
    if (!m || !k || !query || !out || !n_out || k > SIZE_MAX / sizeof *out)
        return GM_E_INVALID_ARG;
    struct gm_stats stats;
    enum gm_status s = gm_store_stats(m->store, &stats);
    if (s != GM_OK)
        return s;
    size_t len = strnlen(query, GM_QUERY_MAX + 1u), prefix = strlen(m->query_prefix);
    if (len > GM_QUERY_MAX || prefix > GM_QUERY_MAX - len)
        return GM_E_TOO_LONG;
    if (!len)
        return GM_OK;
    char buf[GM_QUERY_MAX + 1u];
    memcpy(buf, m->query_prefix, prefix);
    memcpy(buf + prefix, query, len + 1u);
    size_t n_ids = 0;
    s = gm_embedder_tokenize(m->engine, buf, GM_WINDOW + 1u, m->ids, &n_ids);
    if (s != GM_OK)
        return s;
    if (n_ids > GM_WINDOW)
        return GM_E_TOO_LONG;
    if (!n_ids)
        return GM_OK;
    s = embed_window(m, n_ids, m->ids);
    return s == GM_OK ? gm_store_scan(m->store, k, m->bits, out, n_out) : s;
}

const char *gm_doc_path(const struct gm *m, uint32_t doc) {
    return m != nullptr ? gm_store_doc_path(m->store, doc) : nullptr;
}

size_t gm_chunk_count(const struct gm *m) {
    return m != nullptr ? gm_store_live_chunks(m->store) : 0u;
}

size_t gm_dim(const struct gm *m) {
    return m != nullptr ? m->dim : 0u;
}

enum gm_status gm_doc_path_copy(const struct gm *m, uint32_t doc, size_t capacity, char *out) {
    if (out && capacity)
        out[0] = 0;
    if (!out || !capacity)
        return GM_E_INVALID_ARG;
    struct gm_stats stats;
    enum gm_status s = gm_get_stats(m, &stats);
    if (s != GM_OK)
        return s;
    const char *path = gm_doc_path(m, doc);
    if (!path)
        return GM_E_INVALID_ARG;
    size_t n = strlen(path) + 1u;
    if (n > capacity)
        return GM_E_TOO_LONG;
    memcpy(out, path, n);
    return GM_OK;
}

enum gm_status gm_get_stats(const struct gm *m, struct gm_stats *out) {
    return gm_store_stats(m ? m->store : nullptr, out);
}

enum gm_status gm_compact(struct gm *m) {
    return gm_store_compact(m ? m->store : nullptr);
}
