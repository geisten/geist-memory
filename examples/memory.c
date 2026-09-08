/* One small CLI demonstrating ownership, checked statuses and cleanup. */
#include "geist_memory.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: %s DIR MODEL recall QUERY | remember ID TEXT\n", argv[0]);
        return 2;
    }
    bool remember = strcmp(argv[3], "remember") == 0;
    if ((remember && argc != 6) || (!remember && (argc != 5 || strcmp(argv[3], "recall"))))
        return 2;
    struct gm *m = nullptr;
    const struct gm_opts opts = {.query_prefix = "query: "};
    enum gm_status s = gm_open(argv[1], argv[2], &opts, &m);
    if (s != GM_OK)
        goto done;
    if (remember)
        s = gm_remember_text(m, argv[4], argv[5]);
    else {
        struct gm_hit hits[5];
        size_t n = 0;
        s = gm_recall(m, 5, argv[4], hits, &n);
        if (s == GM_OK)
            for (size_t i = 0; i < n; ++i)
                printf("%s #%u distance=%u\n", gm_doc_path(m, hits[i].doc), hits[i].chunk,
                       hits[i].distance);
    }
done:
    gm_close(m);
    if (s != GM_OK)
        fprintf(stderr, "%s\n", gm_status_str(s));
    return s == GM_OK ? 0 : 1;
}
