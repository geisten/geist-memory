#define _POSIX_C_SOURCE 200809L

#include "gm_store.h"

#include <geist.h>
#include <geist_util.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Chunking is in TOKENS, not characters: the model's window is a token
 * budget, and a character split lands mid-word in exactly the languages
 * where that costs the most. The overlap keeps a sentence that straddles a
 * boundary retrievable from either side. */
enum { GM_CHUNK_TOKENS = 256u, GM_CHUNK_OVERLAP = 64u };

/* Refuse rather than truncate (a truncated document is a silently wrong
 * answer later). Generous enough for any note or source file. */
enum { GM_MAX_DOC_BYTES = 4u * 1024u * 1024u, GM_MAX_DOC_TOKENS = 65536u };

struct gm {
    struct gm_store       store;
    struct geist_backend *be;
    struct geist_model   *model;
    struct geist_session *sess;
    char                  query_prefix[128];

    float         *vec;      /* [dim] scratch for one embedding */
    uint8_t       *bits;     /* [dim/8] scratch for its packed form */
    geist_token_t *ids;      /* [GM_MAX_DOC_TOKENS] whole-document tokens */
    geist_token_t *window;   /* [GM_CHUNK_TOKENS + 2] one chunk plus BOS/EOS */
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
    uint64_t    h = 1469598103934665603ULL;
    const uint64_t parts[3] = {
            (stat(model_path, &sb) == 0) ? (uint64_t) sb.st_size : 0u,
            (stat(model_path, &sb) == 0) ? (uint64_t) sb.st_mtime : 0u,
            (uint64_t) dim,
    };
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

/* Tokenize `text`, wrap it in the model's own BOS/EOS, embed, pack. The
 * specials are the model's convention and geist_session_tokenize leaves
 * them to the caller by design. */
static enum gm_status embed_window(struct gm           *m,
                                   size_t               n,
                                   const geist_token_t *ids,
                                   uint8_t             *out_bits) {
    const geist_token_t bos = geist_model_bos_token(m->model);
    const geist_token_t eos = geist_model_eos_token(m->model);
    size_t              k   = 0;
    if (bos != GEIST_TOKEN_NONE) {
        m->window[k++] = bos;
    }
    memcpy(m->window + k, ids, n * sizeof *ids);
    k += n;
    if (eos != GEIST_TOKEN_NONE) {
        m->window[k++] = eos;
    }
    const size_t dim = m->store.dim;
    if (geist_session_embed(m->sess, dim, k, m->window, m->vec) != GEIST_OK) {
        return GM_E_ENGINE;
    }
    pack_signs(dim, m->vec, out_bits);
    return GM_OK;
}

static enum gm_status
index_text(struct gm *m, const char *id, const char *text, int64_t mtime, uint64_t size) {
    size_t n_ids = 0;
    if (geist_session_tokenize(m->sess, text, GM_MAX_DOC_TOKENS, m->ids, &n_ids) != GEIST_OK) {
        return GM_E_ENGINE;
    }
    if (n_ids == 0) {
        return GM_OK; /* nothing to remember; not an error */
    }
    size_t         doc = 0;
    enum gm_status s   = gm_store_put_doc(&m->store, id, mtime, size, &doc);
    if (s != GM_OK) {
        return s;
    }

    const size_t stride = GM_CHUNK_TOKENS - GM_CHUNK_OVERLAP;
    uint32_t     chunk  = 0;
    for (size_t off = 0; off < n_ids; off += stride) {
        const size_t take = (n_ids - off > GM_CHUNK_TOKENS) ? GM_CHUNK_TOKENS : (n_ids - off);
        s                 = embed_window(m, take, m->ids + off, m->bits);
        if (s != GM_OK) {
            return s;
        }
        s = gm_store_put_chunk(&m->store, doc, chunk++, (uint32_t) take, m->bits);
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
    const size_t dim = geist_model_embed_dim(m->model);
    if (dim == 0 || dim % 8u != 0u) {
        goto fail;
    }
    const struct geist_session_opts so = {.max_seq_len = GM_CHUNK_TOKENS + 2u};
    if (geist_session_create(m->model, m->be, &so, &m->sess) != GEIST_OK) {
        goto fail;
    }

    s = GM_E_OOM;
    m->vec    = malloc(dim * sizeof *m->vec);
    m->bits   = malloc(dim / 8u);
    m->ids    = malloc(GM_MAX_DOC_TOKENS * sizeof *m->ids);
    m->window = malloc((GM_CHUNK_TOKENS + 2u) * sizeof *m->window);
    if (m->vec == nullptr || m->bits == nullptr || m->ids == nullptr || m->window == nullptr) {
        goto fail;
    }

    s = gm_store_open(&m->store, dir, dim, model_fingerprint(model_path, dim));
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
    gm_store_close(&m->store);
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
    if ((uint64_t) sb.st_size > GM_MAX_DOC_BYTES) {
        return GM_E_TOO_LONG;
    }
    /* Unchanged since the last index: nothing to do. This is what makes
     * `gm_remember_file` over a whole tree cheap to re-run. */
    const size_t found = gm_store_find_doc(&m->store, path);
    if (found != SIZE_MAX && m->store.docs[found].mtime == (int64_t) sb.st_mtime &&
        m->store.docs[found].size == (uint64_t) sb.st_size) {
        return GM_OK;
    }

    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return GM_E_IO;
    }
    char *text = malloc((size_t) sb.st_size + 1u);
    if (text == nullptr) {
        fclose(f);
        return GM_E_OOM;
    }
    const size_t got = fread(text, 1, (size_t) sb.st_size, f);
    fclose(f);
    text[got] = '\0';

    const enum gm_status s =
            index_text(m, path, text, (int64_t) sb.st_mtime, (uint64_t) sb.st_size);
    free(text);
    return s;
}

enum gm_status gm_remember_text(struct gm *m, const char *id, const char *text) {
    if (m == nullptr || id == nullptr || text == nullptr) {
        return GM_E_INVALID_ARG;
    }
    const size_t len = strlen(text);
    if (len > GM_MAX_DOC_BYTES) {
        return GM_E_TOO_LONG;
    }
    /* No mtime to compare against, so a repeated id always re-indexes. */
    return index_text(m, id, text, 0, (uint64_t) len);
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
    enum gm_status s = embed_window(m, n_ids, m->ids, m->bits);
    if (s != GM_OK) {
        return s;
    }

    /* ponytail: brute-force popcount scan, no index. 1024-bit vectors are
     * 128 B, so 100k chunks is 12.8 MB streamed once — single-digit
     * milliseconds on a Pi 5, and it is exact. Reach for an ANN index when
     * a scan measurably exceeds 100 ms, not before. */
    const size_t   bytes = m->store.bytes;
    const uint64_t *q    = (const uint64_t *) (const void *) m->bits;
    const size_t   words = bytes / 8u;

    size_t n = 0;
    for (size_t i = 0; i < m->store.n_chunks; i++) {
        const struct gm_chunk_rec *rec = &m->store.chunks[i];
        if (rec->generation != m->store.docs[rec->doc].generation) {
            continue; /* superseded by a re-index */
        }
        const uint64_t *v = (const uint64_t *) (const void *) (m->store.vecs + i * bytes);
        uint32_t        d = 0;
        for (size_t w = 0; w < words; w++) {
            d += (uint32_t) __builtin_popcountll(q[w] ^ v[w]);
        }
        /* Insertion into the top-k. k is a handful, so this beats sorting
         * the whole store and allocates nothing. */
        if (n < k || d < out[n - 1].distance) {
            size_t pos = (n < k) ? n : k - 1;
            while (pos > 0 && out[pos - 1].distance > d) {
                out[pos] = out[pos - 1];
                pos--;
            }
            out[pos] = (struct gm_hit) {.doc = rec->doc, .chunk = rec->chunk, .distance = d};
            if (n < k) {
                n++;
            }
        }
    }
    *n_out = n;
    return GM_OK;
}

const char *gm_doc_path(const struct gm *m, uint32_t doc) {
    if (m == nullptr || doc >= m->store.n_docs) {
        return nullptr;
    }
    return m->store.docs[doc].path;
}

size_t gm_chunk_count(const struct gm *m) {
    return m != nullptr ? m->store.n_chunks : 0u;
}

size_t gm_dim(const struct gm *m) {
    return m != nullptr ? m->store.dim : 0u;
}
