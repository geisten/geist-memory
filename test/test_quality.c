#include "quality.h"
#include "test_support.h"
#include <float.h>
#include <math.h>
int main(void) {
    float docs[32], query[8];
    for (size_t i = 0; i < 8; ++i) {
        query[i] = i ? 1 : 4;
        for (size_t d = 0; d < 4; ++d)
            docs[d * 8 + i] = i ? 1 : (float)(d + 1);
    }
    struct quality_result r;
    CHECK(quality_compare(4, 8, docs, query, 3, &r));
    CHECK(r.float_rank == 1 && r.binary_rank == 4);
    CHECK(fabs(r.overlap_at_3 - 2.0 / 3.0) < 1e-12);
    /* Equal signs tie in insertion order; cosine remains scale-invariant. */
    for (size_t i = 0; i < 8; ++i)
        docs[24 + i] *= 2;
    CHECK(quality_compare(4, 8, docs, query, 3, &r) && r.float_rank == 1);
    for (size_t i = 0; i < 8; ++i) {
        docs[i] = -query[i];
        docs[8 + i] = query[i];
    }
    CHECK(quality_compare(2, 8, docs, query, 1, &r));
    CHECK(r.float_rank == 1 && r.binary_rank == 1 && r.overlap_at_3 == 1);
    CHECK(quality_compare(2, 8, docs, query, 0, &r));
    CHECK(r.float_rank == 2 && r.binary_rank == 2);
    for (size_t i = 0; i < 8; ++i)
        query[i] = docs[i] = FLT_MAX;
    CHECK(quality_compare(1, 8, docs, query, 0, &r) && r.float_rank == 1);
    query[0] = NAN;
    CHECK(!quality_compare(1, 8, docs, query, 0, &r) && r.float_rank == 0);
    query[0] = INFINITY;
    CHECK(!quality_compare(1, 8, docs, query, 0, &r));
    memset(query, 0, sizeof query);
    CHECK(!quality_compare(1, 8, docs, query, 0, &r));
    CHECK(!quality_compare(0, 8, docs, query, 0, &r));
    CHECK(!quality_compare(1, 8, docs, query, 1, &r));
    CHECK(!quality_compare(1, 7, docs, query, 0, &r));
    CHECK(!quality_compare(1, 8, nullptr, query, 0, &r));
    /* Large evaluator bounds and tie order, independent of model inference. */
    static float many[QUALITY_DOC_MAX * 8];
    for (size_t i = 0; i < sizeof many / sizeof *many; ++i)
        many[i] = 1;
    for (size_t i = 0; i < 8; ++i)
        query[i] = 1;
    CHECK(quality_compare(QUALITY_DOC_MAX, 8, many, query, QUALITY_DOC_MAX - 1, &r));
    CHECK(r.float_rank == QUALITY_DOC_MAX && r.binary_rank == QUALITY_DOC_MAX);
    CHECK(!quality_compare(QUALITY_DOC_MAX + 1, 8, many, query, 0, &r));
    puts("PASS: cosine/sign ranking, ties, overlap, non-finite and zero vectors");
}
