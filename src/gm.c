#define _POSIX_C_SOURCE 200809L

#include "gm_store.h"

#include <geist.h>
#include <geist_util.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Chunking is in TOKENS, not characters: the model's window is a token
 * budget, and a character split lands mid-word in exactly the languages
 * where that costs the most. The overlap keeps a sentence that straddles a
 * boundary retrievable from either side. */
enum { GM_CHUNK_TOKENS = 256u, GM_CHUNK_OVERLAP = 64u };

/* One document admission limit, in tokens, because that is the buffer that
 * actually bounds the work. The byte cap is derived from it at four bytes
 * per token — comfortably above the ratio any real text reaches — and a
 * document that still tokenizes to the full budget is refused rather than
 * silently indexed short (AGENT.md §5: no silent truncation).
 *
 * ponytail: the whole document's tokens are held at once. Stream the
 * tokenizer if documents past a megabyte ever matter. */
enum {
    GM_MAX_DOC_TOKENS = 65536u,
    GM_MAX_DOC_BYTES  = GM_MAX_DOC_TOKENS * 4u,
};

struct gm {
    struct gm_store      *store;
    struct geist_backend *be;
    struct geist_model   *model;
    struct geist_session *sess;
    char                  query_prefix[128];

    size_t         dim;    /* embedding width in bits, = the store's */
    float         *vec;    /* [dim] scratch for one embedding */
    uint8_t       *bits;   /* [dim/8] its packed form */
    geist_token_t *ids;    /* [GM_MAX_DOC_TOKENS] whole-document tokens */
    geist_token_t *window; /* [GM_CHUNK_TOKENS + 2] one chunk plus BOS/EOS */
};

const char *gm_status_str(enum gm_status s) {
    switch (s) {
    case GM_OK:            return "ok";
    case GM_E_INVALID_ARG: return "invalid argument";
    case GM_E_IO:          return "I/O error";
    case GM_E_OOM:         return "out of memory";
    case GM_E_FORMAT:      return "store is corrupt or not a geist-memory store";
    case GM_E_MODEL:       return "store belongs to a different embedding model";
    case GM_E_TOO_LONG:    return "path or document exceeds the limit";
    case GM_E_ENGINE:      return "the geist engine refused";
    }
    return "unknown";
}

/* Identify the model well enough to catch "same directory, different
 * model". FNV-1a over the file's size and mtime plus the embedding width —
 * cheap, and it changes whenever the weights do. Not a security property:
 * it exists to stop an accidental mix, not a deliberate one. */
static uint64_t model_fingerprint(const char *model_path, size_t dim) {
    struct stat sb;
    if (stat(model_path, &sb) != 0) {
        memset(&sb, 0, sizeof sb);
    }
    const uint64_t parts[3] = {
            (uint64_t) sb.st_size,
            (uint64_t) sb.st_mtime,
            (uint64_t) dim,
    };
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < 3; i++) {
        for (int b = 0; b < 8; b++) {
            h ^= (parts[i] >> (b * 8)) & 0xffu;
            h *= 1099511628211ULL;
        }
    }
    return h;
}

/* sign(v) -> one bit per dimension. The whole index in 1/32nd of the float
 * form; the sign of a component is what survives quantization best because
 * it is scale-free, and cosine order over unit vectors is well approximated
 * by Hamming distance over the signs. */
static void pack_signs(size_t dim, const float v[static dim], uint8_t out[static dim / 8]) {
    memset(out, 0, dim / 8u);
    for (size_t i = 0; i < dim; i++) {
        if (v[i] > 0.0f) {
            out[i >> 3] |= (uint8_t) (1u << (i & 7u));
        }
    }
}

/* Run `n` tokens through the model and leave the packed signs in m->bits.
 *
 * The specials come from the model's own tokenizer metadata rather than a
 * guess: geist_session_tokenize returns content tokens by design, and for a
 * pooled vector the wrapping is not cosmetic — a sequence one token short of
 * the model's convention pools to a different vector. */
static enum gm_status embed_window(struct gm *m, size_t n, const geist_token_t *ids) {
    size_t k = 0;
    if (geist_model_add_bos(m->model)) {
        const geist_token_t bos = geist_model_bos_token(m->model);
        if (bos != GEIST_TOKEN_NONE) {
            m->window[k++] = bos;
        }
    }
    memcpy(m->window + k, ids, n * sizeof *ids);
    k += n;
    if (geist_model_add_eos(m->model)) {
        const geist_token_t eos = geist_model_eos_token(m->model);
        if (eos != GEIST_TOKEN_NONE) {
            m->window[k++] = eos;
        }
    }
    if (geist_session_reset(m->sess) != GEIST_OK ||
        geist_session_prefill_tokens(m->sess, k, m->window) != GEIST_OK) {
        return GM_E_ENGINE;
    }
    size_t       n_dims = 0;
    const float *emb    = geist_session_peek_embedding(&n_dims, m->sess);
    if (emb == nullptr || n_dims != m->dim) {
        return GM_E_ENGINE;
    }
    memcpy(m->vec, emb, m->dim * sizeof *m->vec);
    pack_signs(m->dim, m->vec, m->bits);
    return GM_OK;
}

/* The one funnel every document takes, and therefore the one place its
 * limits are enforced — before the file is read, tokenized or embedded. */
static enum gm_status
index_text(struct gm *m, const char *id, const char *text, size_t len, int64_t mtime) {
    if (gm_store_path_too_long(id)) {
        return GM_E_TOO_LONG;
    }
    if (len > GM_MAX_DOC_BYTES) {
        return GM_E_TOO_LONG;
    }
    size_t n_ids = 0;
    if (geist_session_tokenize(m->sess, text, GM_MAX_DOC_TOKENS, m->ids, &n_ids) != GEIST_OK) {
        return GM_E_ENGINE;
    }
    if (n_ids == GM_MAX_DOC_TOKENS) {
        return GM_E_TOO_LONG; /* may have been cut off; refuse rather than guess */
    }
    if (n_ids == 0) {
        return GM_OK; /* nothing to remember; not an error */
    }

    size_t            doc   = 0;
    enum gm_doc_state state = GM_DOC_NEW;
    enum gm_status    s     = gm_store_put_doc(m->store, id, mtime, (uint64_t) len, &doc, &state);
    if (s != GM_OK || state == GM_DOC_UNCHANGED) {
        return s;
    }

    const size_t stride = GM_CHUNK_TOKENS - GM_CHUNK_OVERLAP;
    uint32_t     chunk  = 0;
    for (size_t off = 0; off < n_ids; off += stride) {
        const size_t take = (n_ids - off > GM_CHUNK_TOKENS) ? GM_CHUNK_TOKENS : (n_ids - off);
        s                 = embed_window(m, take, m->ids + off);
        if (s == GM_OK) {
            s = gm_store_put_chunk(m->store, doc, chunk++, m->bits);
        }
        if (s != GM_OK) {
            return s;
        }
        if (off + take == n_ids) {
            break; /* the overlap must not spin a final short chunk forever */
        }
    }
    return GM_OK;
}

enum gm_status
gm_open(const char *dir, const char *model_path, const struct gm_opts *opts, struct gm **out) {
    if (dir == nullptr || model_path == nullptr || out == nullptr) {
        return GM_E_INVALID_ARG;
    }
    *out = nullptr;

    struct gm *m = calloc(1, sizeof *m);
    if (m == nullptr) {
        return GM_E_OOM;
    }
    if (opts != nullptr && opts->query_prefix != nullptr) {
        snprintf(m->query_prefix, sizeof m->query_prefix, "%s", opts->query_prefix);
    }

    enum gm_status s = GM_E_ENGINE;
    if (geist_backend_create("auto", nullptr, nullptr, &m->be) != GEIST_OK) {
        goto fail;
    }
    if (geist_model_load(model_path, m->be, &m->model) != GEIST_OK) {
        goto fail;
    }
    const struct geist_session_opts so = {.max_seq_len = GM_CHUNK_TOKENS + 2u};
    if (geist_session_create(m->model, m->be, &so, &m->sess) != GEIST_OK) {
        goto fail;
    }

    /* The width comes from the model, and the only thing that reports it is
     * an embedding: peek after one prefill. A model that produces none is
     * not an embedding model, which is a refusal, not a zero. */
    m->ids = malloc(GM_MAX_DOC_TOKENS * sizeof *m->ids);
    if (m->ids == nullptr) {
        s = GM_E_OOM;
        goto fail;
    }
    size_t probe_n = 0;
    if (geist_session_tokenize(m->sess, "probe", GM_MAX_DOC_TOKENS, m->ids, &probe_n) != GEIST_OK ||
        probe_n == 0 || geist_session_prefill_tokens(m->sess, probe_n, m->ids) != GEIST_OK) {
        goto fail;
    }
    if (geist_session_peek_embedding(&m->dim, m->sess) == nullptr || m->dim == 0 ||
        m->dim % 8u != 0u) {
        goto fail;
    }

    s        = GM_E_OOM;
    m->vec   = malloc(m->dim * sizeof *m->vec);
    m->bits  = malloc(m->dim / 8u);
    m->window = malloc((GM_CHUNK_TOKENS + 2u) * sizeof *m->window);
    if (m->vec == nullptr || m->bits == nullptr || m->window == nullptr) {
        goto fail;
    }

    s = gm_store_open(dir, m->dim, model_fingerprint(model_path, m->dim), &m->store);
    if (s != GM_OK) {
        goto fail;
    }
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
    if (m->sess != nullptr) {
        geist_session_destroy(m->sess);
    }
    if (m->model != nullptr) {
        geist_model_destroy(m->model);
    }
    if (m->be != nullptr) {
        geist_backend_destroy(m->be);
    }
    free(m->vec);
    free(m->bits);
    free(m->ids);
    free(m->window);
    free(m);
}

enum gm_status gm_remember_file(struct gm *m, const char *path) {
    if (m == nullptr || path == nullptr) {
        return GM_E_INVALID_ARG;
    }
    struct stat sb;
    if (stat(path, &sb) != 0) {
        return GM_E_IO;
    }
    uint8_t       *text = nullptr;
    size_t         len  = 0;
    enum gm_status s    = gm_read_file(path, &text, &len);
    if (s != GM_OK) {
        return s;
    }
    if (text == nullptr) {
        return GM_E_IO; /* stat saw it; it vanished between the two */
    }
    s = index_text(m, path, (const char *) text, len, (int64_t) sb.st_mtime);
    free(text);
    return s;
}

enum gm_status gm_remember_text(struct gm *m, const char *id, const char *text) {
    if (m == nullptr || id == nullptr || text == nullptr) {
        return GM_E_INVALID_ARG;
    }
    /* No mtime, so the freshness test reduces to the length: a repeated id
     * with the same text is unchanged, exactly as for a file. */
    return index_text(m, id, text, strlen(text), 0);
}

enum gm_status
gm_recall(struct gm *m, size_t k, const char *query, struct gm_hit out[static k], size_t *n_out) {
    if (m == nullptr || k == 0 || query == nullptr || n_out == nullptr) {
        return GM_E_INVALID_ARG;
    }
    *n_out = 0;

    /* The instruction goes on the query side only — documents are embedded
     * bare. Asymmetric on purpose; it is how these models are trained. */
    char        buf[1024];
    const char *text = query;
    if (m->query_prefix[0] != '\0') {
        snprintf(buf, sizeof buf, "%s%s", m->query_prefix, query);
        text = buf;
    }

    size_t n_ids = 0;
    if (geist_session_tokenize(m->sess, text, GM_CHUNK_TOKENS, m->ids, &n_ids) != GEIST_OK) {
        return GM_E_ENGINE;
    }
    if (n_ids == 0) {
        return GM_OK;
    }
    const enum gm_status s = embed_window(m, n_ids, m->ids);
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
