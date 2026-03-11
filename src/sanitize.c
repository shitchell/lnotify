#include "sanitize.h"

#include <stdlib.h>
#include <string.h>

char *sanitize_for_terminal(const char *input) {
    if (!input) return NULL;

    size_t len = strlen(input);
    char *out = malloc(len + 1);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == 0x0A) {
            // Preserve newline
            out[j++] = (char)c;
        } else if (c <= 0x1F || c == 0x7F) {
            // Strip control characters and DEL
            continue;
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return out;
}
