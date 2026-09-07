#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H
#include "gm_internal.h"
#include <stdio.h>
/* Unlike assert, checks still execute under -DNDEBUG. */
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
extern size_t mock_dim, mock_calls;
extern long mock_fail_after;
extern long gm_test_crash_after;
extern size_t gm_test_io_chunk;
void test_dir(char out[64]);
void test_clean(const char *dir);
#endif
