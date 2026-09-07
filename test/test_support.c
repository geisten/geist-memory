#define _POSIX_C_SOURCE 200809L
#include "test_support.h"
#include <dirent.h>
#include <unistd.h>
long gm_test_alloc_after = -1, gm_test_io_after = -1;
long gm_test_crash_after = -1;
size_t gm_test_io_chunk = 0;
bool gm_test_fail(long *counter) {
    if (counter == &gm_test_io_after && gm_test_crash_after >= 0) {
        if (gm_test_crash_after-- == 0)
            _exit(86);
    }
    if (*counter < 0)
        return false;
    if (*counter == 0) {
        *counter = -1;
        return true;
    }
    --*counter;
    return false;
}
void test_dir(char out[64]) {
    strcpy(out, "/tmp/geist-memory-XXXXXX");
    CHECK(mkdtemp(out) != nullptr);
}
void test_clean(const char *dir) {
    DIR *d = opendir(dir);
    CHECK(d);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char path[512];
        int n = snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        CHECK(n > 0 && (size_t)n < sizeof path);
        CHECK(unlink(path) == 0);
    }
    closedir(d);
    CHECK(rmdir(dir) == 0);
}
