#include "model_alloc.h"
#include "test_support.h"
#include <errno.h>
int main(void) {
    model_alloc_begin(-1, 0);
    char *a = malloc(64), *b = calloc(2, 32), *c = aligned_alloc(64, 64);
    char *d = strdup("abc");
    CHECK(a && b && c && d && model_alloc_live() == 196);
    memset(a, 42, 64);
    a = realloc(a, 128);
    CHECK(a && a[63] == 42 && model_alloc_live() == 260);
    struct model_alloc_stats s = model_alloc_end();
    CHECK(s.calls == 5 && s.failed == 0 && s.peak == 260);
    model_alloc_begin(0, 128);
    char *failed = realloc(a, 256);
    CHECK(!failed && a[63] == 42 && model_alloc_live() == 260);
    s = model_alloc_end();
    CHECK(s.failed == 1 && s.eligible == 1);
    free(a);
    free(b);
    free(c);
    free(d);
    CHECK(model_alloc_live() == 0);
    model_alloc_begin(0, 0);
    void *p = nullptr;
    CHECK(posix_memalign(&p, 64, 64) == ENOMEM && !p);
    s = model_alloc_end();
    CHECK(s.failed == 1 && !s.live);
    puts("PASS: allocator accounting, alignment, failure and realloc preservation");
}
