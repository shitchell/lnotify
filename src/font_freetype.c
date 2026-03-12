#include "font.h"
#include "render_util.h"  /* for color_to_bgra */
#include "log.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <fontconfig/fontconfig.h>
#include <string.h>
#include <stdlib.h>

static FT_Library ft_lib;
static FT_Face    ft_face;

static int freetype_text_width(const char *text, int pixel_size) {
    if (!text || !*text) return 0;

    if (FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)pixel_size) != 0) {
        log_error("freetype: FT_Set_Pixel_Sizes failed for size %d", pixel_size);
        return 0;
    }

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

    if (FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)pixel_size) != 0) {
        log_error("freetype: FT_Set_Pixel_Sizes failed for size %d", pixel_size);
        return;
    }

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

                unsigned char alpha = bmp->buffer[(int)row * bmp->pitch + (int)col];
                if (alpha == 0) continue;
                uint8_t *pixel = fb + py * stride + px * 4;
                if (alpha == 255) {
                    memcpy(pixel, bgra, 4);
                } else {
                    uint8_t inv = 255 - alpha;
                    pixel[0] = (uint8_t)((bgra[0] * alpha + pixel[0] * inv) / 255);
                    pixel[1] = (uint8_t)((bgra[1] * alpha + pixel[1] * inv) / 255);
                    pixel[2] = (uint8_t)((bgra[2] * alpha + pixel[2] * inv) / 255);
                    pixel[3] = 255;
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

/* Resolve a font name (e.g. "sans", "monospace", "DejaVu Sans") to a file
 * path using fontconfig. Returns a heap-allocated string or NULL on failure. */
static char *fc_resolve_font(const char *name) {
    FcConfig *config = FcInitLoadConfigAndFonts();
    if (!config) return NULL;

    FcPattern *pat = FcNameParse((const FcChar8 *)name);
    if (!pat) { FcConfigDestroy(config); return NULL; }

    FcConfigSubstitute(config, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern *match = FcFontMatch(config, pat, &result);
    char *path = NULL;

    if (match) {
        FcChar8 *file = NULL;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
            path = strdup((const char *)file);
        }
        FcPatternDestroy(match);
    }

    FcPatternDestroy(pat);
    FcConfigDestroy(config);
    return path;
}

/* Try to load a FreeType face from a file path. Returns 0 on success. */
static int try_load_face(const char *path, const char *label) {
    if (FT_New_Face(ft_lib, path, 0, &ft_face) == 0) {
        log_info("font: loaded FreeType font: %s", path);
        return 0;
    }
    log_info("font: could not load %s: %s", label, path);
    return -1;
}

int freetype_backend_init(font_backend *fb, const char *font_name,
                          const char *font_path) {
    if (FT_Init_FreeType(&ft_lib) != 0) {
        log_error("font: FreeType library init failed");
        return -1;
    }

    /* Priority 1: explicit font_path (must contain '/') */
    if (font_path && *font_path) {
        if (try_load_face(font_path, "font_path") == 0)
            goto success;
    }

    /* Priority 2: resolve font_name via fontconfig */
    {
        const char *name = (font_name && *font_name) ? font_name : "sans";
        char *resolved = fc_resolve_font(name);
        if (resolved) {
            int ok = (FT_New_Face(ft_lib, resolved, 0, &ft_face) == 0);
            if (ok) {
                log_info("font: loaded FreeType font: %s (resolved from '%s')",
                         resolved, name);
            } else {
                log_info("font: fontconfig resolved '%s' to '%s' but load failed",
                         name, resolved);
            }
            free(resolved);
            if (ok) goto success;
        } else {
            log_info("font: fontconfig could not resolve '%s'", name);
        }
    }

    /* All attempts failed. */
    log_error("font: no usable FreeType font found");
    FT_Done_FreeType(ft_lib);
    return -1;

success:
    fb->text_width = freetype_text_width;
    fb->draw_text  = freetype_draw_text;
    fb->cleanup    = freetype_cleanup;
    return 0;
}
