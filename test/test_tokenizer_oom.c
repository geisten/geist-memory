/* Exercise every allocation in the real tokenizer, without a model download. */
#define GEIST_INTERNAL_ENGINE_LAYER
#include "gguf_tokenizer.h"
#include "model_alloc.h"
#include "test_support.h"

int main(void) {
    const char *vocab[] = {"a", "b", "Ġ", "▁", "<s>"};
    size_t lengths[] = {1, 1, 2, 3, 3};
    float scores[] = {0, 0, 0, 0, 0};
    struct gguf_tokenizer_special special = {.text = "<s>", .len = 3, .id = 4};
    struct gguf_tokenizer tok = {.token_str = vocab,
                                 .token_len = lengths,
                                 .vocab_size = 5,
                                 .scores = scores,
                                 .unk_id = -1,
                                 .add_space_prefix = true,
                                 .specials = &special,
                                 .n_specials = 1};
    for (size_t i = 0; i < 256; i++)
        tok.spm_byte_id[i] = -1;
    const char *texts[] = {"a b ", "a <s>b", "<s>a b<s>a"};
    size_t faults = 0;
    for (unsigned mode = 0; mode < 4; mode++) {
        tok.mode = mode < 2    ? GGUF_TOK_MODE_GPT2
                   : mode == 2 ? GGUF_TOK_MODE_SPM
                               : GGUF_TOK_MODE_UNIGRAM;
        tok.pre_qwen2 = mode == 1;
        for (size_t t = 0; t < sizeof texts / sizeof *texts; t++) {
            int32_t reference[64], actual[64];
            size_t expected = 0, count = 0;
            model_alloc_begin(-1, 0);
            CHECK(gguf_tokenizer_encode(&tok, texts[t], reference, 64, &expected));
            struct model_alloc_stats baseline = model_alloc_end();
            CHECK(expected > 0 && baseline.calls > 1 && !baseline.live);
            for (size_t f = 0; f < baseline.calls; f++) {
                model_alloc_begin((long)f, 0);
                CHECK(!gguf_tokenizer_encode(&tok, texts[t], actual, 64, &count));
                struct model_alloc_stats s = model_alloc_end();
                CHECK(s.failed == 1 && !s.live && count == 0);
                CHECK(gguf_tokenizer_encode(&tok, texts[t], actual, 64, &count));
                CHECK(count == expected && !memcmp(actual, reference, count * sizeof *actual));
                CHECK(!model_alloc_live());
                faults++;
            }
        }
    }
    printf("PASS: %zu tokenizer allocation faults fail closed; all retries match\n", faults);
}
