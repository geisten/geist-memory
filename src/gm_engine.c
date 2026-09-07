#include "gm_engine.h"
#include "gm_internal.h"
#include <geist.h>
#include <geist_util.h>
struct gm_engine {
    struct geist_backend *backend;
    struct geist_model *model;
    struct geist_session *session;
    bool omit_bos, omit_eos;
};
static enum gm_status status(enum geist_status s) {
    return s == GEIST_OK ? GM_OK : s == GEIST_E_OOM ? GM_E_OOM : GM_E_ENGINE;
}
void gm_engine_close(struct gm_engine *e) {
    if (!e)
        return;
    if (e->session)
        geist_session_destroy(e->session);
    if (e->model)
        geist_model_destroy(e->model);
    if (e->backend)
        geist_backend_destroy(e->backend);
    free(e);
}
enum gm_status gm_engine_tokenize(struct gm_engine *e, const char *text, size_t capacity,
                                  int32_t *ids, size_t *n) {
    *n = 0;
    enum geist_status s = geist_session_tokenize(e->session, text, capacity, ids, n);
    /* Some engine tokenizer paths report a full buffer without distinguishing
     * truncation. Callers provide limit+1 slots and reject the extra token. */
    return s == GEIST_E_INVALID_ARG ? GM_E_TOO_LONG : status(s);
}
enum gm_status gm_engine_embed(struct gm_engine *e, size_t n, const int32_t *ids, size_t dim,
                               const float **out) {
    *out = nullptr;
    if (!n || n > GM_WINDOW)
        return GM_E_INVALID_ARG;
    geist_token_t window[GM_WINDOW + 2];
    size_t k = 0;
    geist_token_t bos = geist_model_bos_token(e->model), eos = geist_model_eos_token(e->model);
    if (!e->omit_bos && bos != GEIST_TOKEN_NONE)
        window[k++] = bos;
    memcpy(window + k, ids, n * sizeof *ids);
    k += n;
    if (!e->omit_eos && eos != GEIST_TOKEN_NONE)
        window[k++] = eos;
    enum geist_status s = geist_session_reset(e->session);
    if (s == GEIST_OK)
        s = geist_session_prefill_tokens(e->session, k, window);
    if (s != GEIST_OK)
        return status(s);
    size_t width = 0;
    const float *v = geist_session_peek_embedding(&width, e->session);
    if (!v || width != dim)
        return GM_E_ENGINE;
    *out = v;
    return GM_OK;
}
enum gm_status gm_engine_open(const char *path, bool omit_bos, bool omit_eos,
                              struct gm_engine **out, size_t *dim) {
    *out = nullptr;
    *dim = 0;
    struct gm_engine *e = gm_zero(sizeof *e);
    if (!e)
        return GM_E_OOM;
    e->omit_bos = omit_bos;
    e->omit_eos = omit_eos;
    enum geist_status s = geist_backend_create("auto", nullptr, nullptr, &e->backend);
    const struct geist_session_opts opts = {.max_seq_len = GM_WINDOW + 2};
    if (s == GEIST_OK)
        s = geist_model_load_with_opts(path, e->backend, &opts, &e->model);
    if (s == GEIST_OK)
        s = geist_session_create(e->model, e->backend, &opts, &e->session);
    if (s != GEIST_OK) {
        enum gm_status result = status(s);
        gm_engine_close(e);
        return result;
    }
    int32_t ids[GM_WINDOW + 1];
    size_t n = 0;
    enum gm_status result = gm_engine_tokenize(e, "probe", GM_WINDOW + 1, ids, &n);
    if (result != GM_OK || !n || n > GM_WINDOW) {
        gm_engine_close(e);
        return result == GM_OK ? GM_E_ENGINE : result;
    }
    s = geist_session_prefill_tokens(e->session, n, ids);
    if (s != GEIST_OK || !geist_session_peek_embedding(dim, e->session) || !*dim || *dim % 8 ||
        *dim > GM_DIM_MAX) {
        gm_engine_close(e);
        *dim = 0;
        return s == GEIST_E_OOM ? GM_E_OOM : GM_E_ENGINE;
    }
    *out = e;
    return GM_OK;
}
