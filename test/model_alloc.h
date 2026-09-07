#ifndef MODEL_ALLOC_H
#define MODEL_ALLOC_H
#include <stddef.h>
struct model_alloc_stats {
    size_t live, peak, calls, eligible, failed, largest;
};
void model_alloc_begin(long fail_at, size_t minimum);
struct model_alloc_stats model_alloc_end(void);
size_t model_alloc_live(void);
#endif
