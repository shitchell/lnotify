#include "strutil.h"

#include <stdlib.h>
#include <string.h>

int replace_str(char **dst, const char *src) {
    if (!src) {
        free(*dst);
        *dst = NULL;
        return 0;
    }
    char *dup = strdup(src);
    if (!dup) return -1;
    free(*dst);
    *dst = dup;
    return 0;
}
