#ifndef LNOTIFY_FONT_H
#define LNOTIFY_FONT_H

#include <stdint.h>
#include "config.h"

// Font backend interface — each backend populates these function pointers.
typedef struct font_backend {
    int  (*text_width)(const char *text, int pixel_size);
    void (*draw_text)(uint8_t *fb, int fb_w, int fb_h, int stride,
                      const char *text, int x, int y, int pixel_size,
                      const lnotify_color *color);
    void (*cleanup)(void);
} font_backend;

// Global font backend (set by font_init).
extern font_backend g_font;

// Initialise the font backend. font_path (explicit TTF path) takes priority
// over font_name (fontconfig name like "sans"). If both are empty/NULL, a
// sensible system default is used. Falls back to bitmap font if FreeType
// is unavailable or fails.
void font_init(const char *font_name, const char *font_path);

// Clean up the active font backend.
void font_cleanup(void);

#endif
