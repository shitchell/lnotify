#ifndef LNOTIFY_STRUTIL_H
#define LNOTIFY_STRUTIL_H

// Replace *dst with a strdup'd copy of src, freeing the old value.
// If src is NULL, *dst is freed and set to NULL.
// Returns 0 on success, -1 on allocation failure (old value preserved).
int replace_str(char **dst, const char *src);

#endif
