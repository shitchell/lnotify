#ifndef LNOTIFY_SANITIZE_H
#define LNOTIFY_SANITIZE_H

// Strip terminal-dangerous bytes from a string before writing to a PTY.
// Removes bytes 0x00-0x1F (except 0x0A newline) and 0x7F (DEL).
// Returns a heap-allocated sanitized copy (caller frees), or NULL on
// allocation failure.  NULL input returns NULL; empty input returns
// an empty heap-allocated string.
char *sanitize_for_terminal(const char *input);

#endif
