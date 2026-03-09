#include "font.h"
#include "font_bitmap.h"
#include "log.h"

font_backend g_font;

void font_init(const char *font_path) {
#ifdef HAVE_FREETYPE
    if (freetype_backend_init(&g_font, font_path) == 0) return;
    log_info("font: FreeType init failed, falling back to bitmap");
#endif
    (void)font_path;
    bitmap_backend_init(&g_font);
}

void font_cleanup(void) {
    if (g_font.cleanup) {
        g_font.cleanup();
    }
}
