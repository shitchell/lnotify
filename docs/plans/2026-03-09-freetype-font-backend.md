# Optional FreeType Font Backend

**Date:** 2026-03-09
**Status:** Design complete, pending implementation
**Motivation:** The built-in 8x8 bitmap font only renders cleanly at multiples of 8px (8, 16, 24...). FreeType enables crisp text at any pixel size. Optional dependency — bitmap font is always the fallback.

## Font Backend Abstraction

```c
// include/font.h
typedef struct {
    int  (*text_width)(const char *text, int pixel_size);
    void (*draw_text)(uint8_t *fb, int fb_w, int fb_h, int stride,
                      const char *text, int x, int y, int pixel_size,
                      const lnotify_color *color);
    void (*cleanup)(void);
} font_backend;

extern font_backend g_font;

void font_init(const char *font_path);
void font_cleanup(void);
```

`engine_fb.c` calls `g_font.draw_text()` and `g_font.text_width()` — no `#ifdef`s in rendering code.

## Build Integration

Autodetect with user override:

```makefile
FREETYPE ?= $(shell pkg-config --exists freetype2 2>/dev/null && echo 1 || echo 0)
ifeq ($(FREETYPE),1)
  CFLAGS  += -DHAVE_FREETYPE $(shell pkg-config --cflags freetype2)
  LDFLAGS += $(shell pkg-config --libs freetype2)
  COMMON_SRC += src/font_freetype.c
endif
```

User can force off with `make FREETYPE=0`.

## Backend Selection (font_init)

```c
void font_init(const char *font_path) {
#ifdef HAVE_FREETYPE
    if (freetype_backend_init(&g_font, font_path) == 0) return;
    log_info("font: FreeType init failed, falling back to bitmap");
#endif
    bitmap_backend_init(&g_font);
}
```

FreeType tried first, bitmap always available as fallback.

## File Layout

| File | Purpose |
|------|---------|
| `include/font.h` | `font_backend` struct, `g_font` extern, init/cleanup declarations |
| `src/font.c` | `font_init()`, `font_cleanup()`, `g_font` definition |
| `src/font_bitmap.c` | Add `bitmap_backend_init()` — wraps existing glyph data into backend API |
| `src/font_freetype.c` | FreeType backend (only compiled when `FREETYPE=1`) |
| `include/font_bitmap.h` | Unchanged — glyph data API (`get_char_bitmap`) |

## Modified Files

| File | Change |
|------|--------|
| `src/engine_fb.c` | Replace `fb_draw_char`/`fb_draw_text`/`text_width` with `g_font.draw_text`/`g_font.text_width` |
| `include/render_util.h` | Remove `text_width()` declaration (moves to font.h) |
| `src/render_util.c` | Remove `text_width()` implementation (moves to backends) |
| `Makefile` | Autodetect freetype, conditionally compile, pass `-DHAVE_FREETYPE` |
| `src/daemon/main.c` | Call `font_init()` at startup, `font_cleanup()` at shutdown |

## Bitmap Backend

Wraps the existing 8x8 font data (`get_char_bitmap`) into the `font_backend` API:
- `text_width`: `strlen(text) * FONT_WIDTH * scale` (scale = pixel_size / FONT_HEIGHT)
- `draw_text`: Existing scale-based rendering (each font pixel → scale×scale block)
- `cleanup`: No-op

Renders cleanly at multiples of 8 (8, 16, 24). Intermediate sizes use nearest-neighbor.

## FreeType Backend

Uses `libfreetype` to render TTF/OTF fonts at any pixel size:
- **Init:** `FT_Init_FreeType`, `FT_New_Face` from `font_path` (config's `font_path`, default DejaVuSans.ttf). Falls back to common system fonts if configured path fails.
- **text_width:** Sum `face->glyph->advance.x >> 6` per character for proper per-character metrics.
- **draw_text:** `FT_Set_Pixel_Sizes(face, 0, pixel_size)`, `FT_Load_Char(ch, FT_LOAD_RENDER)`, blit glyph bitmap to framebuffer (binary threshold: write foreground color where alpha > 128).
- **cleanup:** `FT_Done_Face`, `FT_Done_FreeType`.

## Config Integration

Existing config keys used:
- `font_path` — TTF path for FreeType (ignored by bitmap backend)
- `font_size` — pixel size for title; body is `font_size * 3/4` (clamped to min 8)

## What This Enables

- Title at 16px, body at 12px — both crisp with FreeType
- Any font_size value works, not just multiples of 8
- Systems without libfreetype-dev still build and work (bitmap fallback)
- Zero runtime cost for bitmap path (no FreeType code loaded)
