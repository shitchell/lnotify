#ifndef LNOTIFY_FONT_BITMAP_H
#define LNOTIFY_FONT_BITMAP_H

#include <stdint.h>

// Font dimensions (fixed 8x8 bitmap font)
#define FONT_WIDTH  8
#define FONT_HEIGHT 8

// Return a pointer to 8 bytes representing an 8x8 bitmap for the given
// character. Each byte is one row (MSB = leftmost pixel).
// Non-printable or unmapped characters fall back to '?'.
// The returned pointer is to static data and must not be freed.
const uint8_t *get_char_bitmap(char ch);

// Forward declaration — full definition in font.h.
typedef struct font_backend font_backend;

// Initialise the bitmap font backend, populating the function pointers in fb.
void bitmap_backend_init(font_backend *fb);

#endif
