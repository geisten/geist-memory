#include "quality.h"
#include "geist_memory.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

bool quality_compare(size_t count, size_t dim, const float *documents, const float *query,
                     size_t relevant, struct quality_result *out) {
    if (out)
        memset(out, 0, sizeof *out);
    if (!out || !documents || !query || !count || count > QUALITY_DOC_MAX || relevant >= count ||
        !dim || dim % 8 || dim > GM_DIM_MAX)
        return false;
    double cosine[QUALITY_DOC_MAX];
    uint32_t distance[QUALITY_DOC_MAX];
    size_t floats[QUALITY_DOC_MAX], bits[QUALITY_DOC_MAX];
    for (size_t doc = 0; doc < count; ++doc) {
        double dot = 0, a2 = 0, b2 = 0;
        distance[doc] = 0;
        for (size_t i = 0; i < dim; ++i) {
            double a = documents[doc * dim + i], b = query[i];
            if (!isfinite(a) || !isfinite(b))
                return false;
            dot += a * b;
            a2 += a * a;
            b2 += b * b;
            distance[doc] += (a > 0) != (b > 0);
        }
        if (a2 == 0 || b2 == 0)
            return false;
        cosine[doc] = dot / (sqrt(a2) * sqrt(b2));
        size_t pos = doc;
        while (pos && cosine[floats[pos - 1]] < cosine[doc]) {
            floats[pos] = floats[pos - 1];
            --pos;
        }
        floats[pos] = doc;
        pos = doc;
        while (pos && distance[bits[pos - 1]] > distance[doc]) {
            bits[pos] = bits[pos - 1];
            --pos;
        }
        bits[pos] = doc;
    }
    size_t k = count < 3 ? count : 3, common = 0;
    for (size_t i = 0; i < count; ++i) {
        if (floats[i] == relevant)
            out->float_rank = i + 1;
        if (bits[i] == relevant)
            out->binary_rank = i + 1;
        if (i < k)
            for (size_t j = 0; j < k; ++j)
                common += floats[i] == bits[j];
    }
    out->overlap_at_3 = (double)common / (double)k;
    return true;
}
