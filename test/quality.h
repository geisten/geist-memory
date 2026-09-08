#ifndef QUALITY_H
#define QUALITY_H
#include <stdbool.h>
#include <stddef.h>
/* Small-corpus evaluation only; independent of the production search code. */
enum { QUALITY_DOC_MAX = 512 };
struct quality_result {
    size_t float_rank, binary_rank; /* one-based rank of the relevant document */
    double overlap_at_3;            /* intersection / min(3, document count) */
};
bool quality_compare(size_t count, size_t dim, const float *documents, const float *query,
                     size_t relevant, struct quality_result *out);
#endif
