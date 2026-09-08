/* Exact before/after reference for the load-time context bound on one backend. */
#include "gm_internal.h"
#include "test_support.h"
#include <geist.h>
#include <geist_util.h>
#include <math.h>
static float reference[3][GM_DIM_MAX];
int main(void) {
    const char *path = getenv("GEIST_EMBED_GGUF_PATH");
    CHECK(path);
    char long_text[4096] = "";
    for (size_t i = 0; i < 100; ++i)
        strcat(long_text, "Ocean tides are caused by the moon. ");
    const char *texts[] = {"Yeast makes bread rise.", "query: What causes ocean tides?", long_text};
    size_t dimension = 0;
    for (int bounded = 0; bounded < 2; ++bounded) {
        struct geist_backend *be = nullptr;
        struct geist_model *model = nullptr;
        struct geist_session *session = nullptr;
        struct geist_session_opts opts = {.max_seq_len = GM_WINDOW + 2};
        CHECK(geist_backend_create("auto", nullptr, nullptr, &be) == GEIST_OK);
        CHECK((bounded ? geist_model_load_with_opts(path, be, &opts, &model)
                       : geist_model_load(path, be, &model)) == GEIST_OK);
        CHECK(geist_session_create(model, be, &opts, &session) == GEIST_OK);
        for (size_t t = 0; t < 3; ++t) {
            static int32_t ids[GM_TOKENS + 1];
            size_t n = 0;
            CHECK(geist_session_tokenize(session, texts[t], GM_TOKENS + 1, ids, &n) == GEIST_OK);
            CHECK(n && n <= GM_TOKENS);
            if (n > GM_WINDOW)
                n = GM_WINDOW;
            geist_token_t eos = geist_model_eos_token(model);
            if (eos != GEIST_TOKEN_NONE)
                ids[n++] = eos;
            CHECK(geist_session_reset(session) == GEIST_OK);
            CHECK(geist_session_prefill_tokens(session, n, ids) == GEIST_OK);
            size_t dim = 0;
            const float *v = geist_session_peek_embedding(&dim, session);
            CHECK(v && dim && dim <= GM_DIM_MAX);
            for (size_t i = 0; i < dim; ++i)
                CHECK(isfinite(v[i]));
            if (!bounded) {
                dimension = dim;
                memcpy(reference[t], v, dim * sizeof *v);
            } else
                CHECK(dim == dimension && !memcmp(reference[t], v, dim * sizeof *v));
        }
        geist_session_destroy(session);
        geist_model_destroy(model);
        geist_backend_destroy(be);
    }
    puts("PASS: bounded model load preserves exact embeddings, including a full token window");
}
