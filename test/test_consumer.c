/* Compiles and links using installed public files only. Every gm symbol lives
 * in the linked library; the missing-model path needs no large test fixture. */
#include <geist_memory.h>
#include <stdio.h>
int main(void) {
    struct gm *m = nullptr;
    enum gm_status s = gm_open(".", "", nullptr, &m);
    if (s != GM_E_INVALID_ARG || m != nullptr)
        return 1;
    struct gm_stats stats;
    if (gm_get_stats(nullptr, &stats) != GM_E_INVALID_ARG)
        return 1;
    if (gm_compact(nullptr) != GM_E_INVALID_ARG)
        return 1;
    if (gm_import_v1(nullptr, "", 0) != GM_E_INVALID_ARG)
        return 1;
    puts(gm_status_str(GM_OK));
    return 0;
}
