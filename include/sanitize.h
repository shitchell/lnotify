#ifndef LNOTIFY_SANITIZE_H
#define LNOTIFY_SANITIZE_H

#include "lnotify.h"

// Strip terminal-dangerous bytes from a string before writing to a PTY.
// Removes bytes 0x00-0x1F (except 0x0A newline) and 0x7F (DEL).
// Returns a heap-allocated sanitized copy (caller frees), or NULL on
// allocation failure.  NULL input returns NULL; empty input returns
// an empty heap-allocated string.
char *sanitize_for_terminal(const char *input);

// Helper: sanitize both title and body of a notification at once.
// s->title and s->body are heap-allocated (caller must call sanitize_notif_free).
// s->t and s->b are never-NULL aliases (empty string when the field is NULL).
typedef struct {
    char *title;
    char *body;
    const char *t;
    const char *b;
} sanitized_notif;

void sanitize_notif(sanitized_notif *s, const notification *n);
void sanitize_notif_free(sanitized_notif *s);

#endif
