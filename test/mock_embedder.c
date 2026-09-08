#include "test_support.h"
size_t mock_dim = 72, mock_calls = 0;
long mock_fail_after = -1;
struct gm_embedder {
    float vec[GM_DIM_MAX];
};
enum gm_status gm_embedder_open(const char *path, bool bos, bool eos, struct gm_embedder **out,
                                size_t *dim) {
    (void)path;
    (void)bos;
    (void)eos;
    *out = gm_zero(sizeof **out);
    *dim = mock_dim;
    return *out ? GM_OK : GM_E_OOM;
}
void gm_embedder_close(struct gm_embedder *e) {
    free(e);
}
enum gm_status gm_embedder_tokenize(struct gm_embedder *e, const char *text, size_t cap,
                                    int32_t *ids, size_t *n) {
    (void)e;
    *n = 0;
    for (size_t i = 0; text[i]; ++i) {
        if (*n == cap)
            return GM_OK; /* deliberately model a truncating tokenizer */
        ids[(*n)++] = (unsigned char)text[i];
    }
    return GM_OK;
}
enum gm_status gm_embedder_embed(struct gm_embedder *e, size_t n, const int32_t *ids, size_t dim,
                                 const float **out) {
    ++mock_calls;
    if (gm_test_fail(&mock_fail_after))
        return GM_E_ENGINE;
    CHECK(n && n <= GM_WINDOW);
    for (size_t i = 0; i < dim; ++i)
        e->vec[i] = ((unsigned)ids[0] >> (i % 8)) & 1 ? 1.0f : -1.0f;
    *out = e->vec;
    return GM_OK;
}
