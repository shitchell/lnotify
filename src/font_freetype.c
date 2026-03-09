#include "font.h"
#include "font_bitmap.h"  /* for FONT_WIDTH, FONT_HEIGHT (fallback paths) */
#include "render_util.h"  /* for color_to_bgra */
#include "log.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <string.h>

static FT_Library ft_lib;
static FT_Face    ft_face;

static int freetype_text_width(const char *text, int pixel_size) {
    if (!text || !*text) return 0;

    FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)pixel_size);

    int width = 0;
    for (const char *p = text; *p; p++) {
        if (FT_Load_Char(ft_face, (FT_ULong)*p, FT_LOAD_DEFAULT) != 0)
            continue;
        width += (int)(ft_face->glyph->advance.x >> 6);
    }
    return width;
}

static void freetype_draw_text(uint8_t *fb, int fb_w, int fb_h, int stride,
                                const char *text, int x, int y, int pixel_size,
                                const lnotify_color *color) {
    if (!text || !*text) return;

    FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)pixel_size);

    uint8_t bgra[4];
    color_to_bgra(color, bgra);

    int pen_x = x;
    for (const char *p = text; *p; p++) {
        if (FT_Load_Char(ft_face, (FT_ULong)*p, FT_LOAD_RENDER) != 0)
            continue;

        FT_GlyphSlot g = ft_face->glyph;
        FT_Bitmap *bmp = &g->bitmap;

        int glyph_x = pen_x + g->bitmap_left;
        int glyph_y = y + pixel_size - g->bitmap_top;

        for (unsigned int row = 0; row < bmp->rows; row++) {
            int py = glyph_y + (int)row;
            if (py < 0 || py >= fb_h) continue;

            for (unsigned int col = 0; col < bmp->width; col++) {
                int px = glyph_x + (int)col;
                if (px < 0 || px >= fb_w) continue;

                unsigned char alpha = bmp->buffer[row * (unsigned int)bmp->pitch + col];
                if (alpha > 128) {
                    int offset = py * stride + px * 4;
                    fb[offset + 0] = bgra[0];
                    fb[offset + 1] = bgra[1];
                    fb[offset + 2] = bgra[2];
                    fb[offset + 3] = bgra[3];
                }
            }
        }

        pen_x += (int)(g->advance.x >> 6);
    }
}

static void freetype_cleanup(void) {
    FT_Done_Face(ft_face);
    FT_Done_FreeType(ft_lib);
}

/* Common system font paths to try when no font_path is specified. */
static const char *fallback_fonts[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    NULL
};

int freetype_backend_init(font_backend *fb, const char *font_path) {
    if (FT_Init_FreeType(&ft_lib) != 0) {
        log_error("font: FreeType library init failed");
        return -1;
    }

    /* Try the specified font path first. */
    if (font_path && *font_path) {
        if (FT_New_Face(ft_lib, font_path, 0, &ft_face) == 0) {
            log_info("font: loaded FreeType font: %s", font_path);
            goto success;
        }
        log_info("font: could not load '%s', trying fallbacks", font_path);
    }

    /* Try fallback system fonts. */
    for (const char **fp = fallback_fonts; *fp; fp++) {
        if (FT_New_Face(ft_lib, *fp, 0, &ft_face) == 0) {
            log_info("font: loaded FreeType font: %s", *fp);
            goto success;
        }
    }

    /* All paths failed. */
    log_error("font: no usable FreeType font found");
    FT_Done_FreeType(ft_lib);
    return -1;

success:
    fb->text_width = freetype_text_width;
    fb->draw_text  = freetype_draw_text;
    fb->cleanup    = freetype_cleanup;
    return 0;
}
