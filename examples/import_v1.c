#include "geist_memory.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s V1_SOURCE NEW_DESTINATION\n", argv[0]);
        return 2;
    }
    enum gm_status s = gm_import_v1(argv[1], argv[2], 0);
    if (s != GM_OK) {
        fprintf(stderr, "import: %s (inspect any newly created destination before retrying)\n",
                gm_status_str(s));
        return 1;
    }
    puts("Imported v1 data. The original model fingerprint is preserved, not revalidated.");
    return 0;
}
