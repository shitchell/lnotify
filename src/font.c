#include "font.h"
#include "font_bitmap.h"
#include "log.h"

font_backend g_font;

void font_init(const char *font_name, const char *font_path) {
#ifdef HAVE_FREETYPE
    int freetype_backend_init(font_backend *fb, const char *name,
                              const char *path);
    if (freetype_backend_init(&g_font, font_name, font_path) == 0) return;
    log_info("font: FreeType init failed, falling back to bitmap");
#endif
    (void)font_name;
    (void)font_path;
    bitmap_backend_init(&g_font);
}

void font_cleanup(void) {
    if (g_font.cleanup) {
        g_font.cleanup();
    }
}
