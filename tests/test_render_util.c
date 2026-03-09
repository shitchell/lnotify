#include "render_util.h"
#include "font_bitmap.h"
#include "config.h"
#include <string.h>

#include "test_util.h"

void test_render_util_suite(void) {
    // Test: color from config struct to BGRA bytes
    {
        lnotify_color c = {.r = 0x11, .g = 0x22, .b = 0x33, .a = 0xFF};
        uint8_t bgra[4];
        color_to_bgra(&c, bgra);
        ASSERT_EQ(bgra[0], 0x33, "blue byte");
        ASSERT_EQ(bgra[1], 0x22, "green byte");
        ASSERT_EQ(bgra[2], 0x11, "red byte");
        ASSERT_EQ(bgra[3], 0xFF, "alpha byte");
    }

    // Test: color_to_bgra with zero alpha
    {
        lnotify_color c = {.r = 0xFF, .g = 0x00, .b = 0x80, .a = 0x00};
        uint8_t bgra[4];
        color_to_bgra(&c, bgra);
        ASSERT_EQ(bgra[0], 0x80, "blue byte with zero alpha");
        ASSERT_EQ(bgra[1], 0x00, "green byte with zero alpha");
        ASSERT_EQ(bgra[2], 0xFF, "red byte with zero alpha");
        ASSERT_EQ(bgra[3], 0x00, "alpha byte zero");
    }

    // Test: point-in-rounded-rect - center
    {
        // 100x50 rect at (10,10) with radius 5
        ASSERT_TRUE(point_in_rounded_rect(50, 25, 10, 10, 100, 50, 5),
                    "center is inside");
    }

    // Test: point-in-rounded-rect - corner outside radius
    {
        ASSERT_FALSE(point_in_rounded_rect(10, 10, 10, 10, 100, 50, 5),
                     "corner outside radius");
    }

    // Test: point-in-rounded-rect - just inside corner
    {
        ASSERT_TRUE(point_in_rounded_rect(15, 15, 10, 10, 100, 50, 5),
                    "just inside corner radius");
    }

    // Test: point-in-rounded-rect - outside rect entirely
    {
        ASSERT_FALSE(point_in_rounded_rect(0, 0, 10, 10, 100, 50, 5),
                     "outside rect entirely");
    }

    // Test: point-in-rounded-rect - on edge (non-corner)
    {
        ASSERT_TRUE(point_in_rounded_rect(50, 10, 10, 10, 100, 50, 5),
                    "on top edge, non-corner region");
    }

    // Test: point-in-rounded-rect - zero radius (standard rect)
    {
        ASSERT_TRUE(point_in_rounded_rect(10, 10, 10, 10, 100, 50, 0),
                    "corner with zero radius is inside");
        ASSERT_FALSE(point_in_rounded_rect(9, 10, 10, 10, 100, 50, 0),
                     "just outside with zero radius");
    }

    // Test: point-in-rounded-rect - bottom-right corner outside
    {
        ASSERT_FALSE(point_in_rounded_rect(109, 59, 10, 10, 100, 50, 5),
                     "bottom-right corner outside radius");
    }

    // Test: toast geometry - top-right
    {
        toast_geometry geom;
        compute_toast_geometry(&geom, 1920, 1080, "top-right",
                               400, 80, 30);
        ASSERT_EQ(geom.x, 1920 - 400 - 30, "x = screen - width - margin");
        ASSERT_EQ(geom.y, 30, "y = margin");
        ASSERT_EQ(geom.w, 400, "width");
        ASSERT_EQ(geom.h, 80, "height");
    }

    // Test: toast geometry - bottom-left
    {
        toast_geometry geom;
        compute_toast_geometry(&geom, 1920, 1080, "bottom-left",
                               400, 80, 30);
        ASSERT_EQ(geom.x, 30, "x = margin");
        ASSERT_EQ(geom.y, 1080 - 80 - 30, "y = screen - height - margin");
    }

    // Test: toast geometry - top-left
    {
        toast_geometry geom;
        compute_toast_geometry(&geom, 1920, 1080, "top-left",
                               400, 80, 30);
        ASSERT_EQ(geom.x, 30, "x = margin for top-left");
        ASSERT_EQ(geom.y, 30, "y = margin for top-left");
    }

    // Test: toast geometry - bottom-right
    {
        toast_geometry geom;
        compute_toast_geometry(&geom, 1920, 1080, "bottom-right",
                               400, 80, 30);
        ASSERT_EQ(geom.x, 1920 - 400 - 30, "x = screen - width - margin for bottom-right");
        ASSERT_EQ(geom.y, 1080 - 80 - 30, "y = screen - height - margin for bottom-right");
    }

    // Test: toast geometry - unknown position defaults to top-right
    {
        toast_geometry geom;
        compute_toast_geometry(&geom, 1920, 1080, "nonsense",
                               400, 80, 30);
        ASSERT_EQ(geom.x, 1920 - 400 - 30, "unknown position defaults to top-right x");
        ASSERT_EQ(geom.y, 30, "unknown position defaults to top-right y");
    }

    // Test: text width calculation
    {
        int w = text_width("Hello", 2);  // scale=2, 8px font
        ASSERT_EQ(w, 5 * 8 * 2, "5 chars * 8px * scale 2");
    }

    // Test: text width with scale 1
    {
        int w = text_width("AB", 1);
        ASSERT_EQ(w, 2 * 8 * 1, "2 chars * 8px * scale 1");
    }

    // Test: text width empty string
    {
        int w = text_width("", 3);
        ASSERT_EQ(w, 0, "empty string has zero width");
    }

    // Test: text width NULL string
    {
        int w = text_width(NULL, 2);
        ASSERT_EQ(w, 0, "NULL string has zero width");
    }

    // Test: font bitmap returns non-NULL for printable ASCII
    {
        const uint8_t *bmp = get_char_bitmap('A');
        ASSERT_NOT_NULL(bmp, "get_char_bitmap('A') returns non-NULL");
        // 'A' bitmap should have some set bits
        int any_set = 0;
        for (int i = 0; i < 8; i++) {
            if (bmp[i] != 0) any_set = 1;
        }
        ASSERT_TRUE(any_set, "'A' bitmap has pixels");
    }

    // Test: font bitmap for space is all zeros
    {
        const uint8_t *bmp = get_char_bitmap(' ');
        ASSERT_NOT_NULL(bmp, "get_char_bitmap(' ') returns non-NULL");
        int all_zero = 1;
        for (int i = 0; i < 8; i++) {
            if (bmp[i] != 0) all_zero = 0;
        }
        ASSERT_TRUE(all_zero, "space bitmap is all zeros");
    }

    // Test: font bitmap for unknown char falls back to '?'
    {
        const uint8_t *bmp_unknown = get_char_bitmap('\x01');
        const uint8_t *bmp_question = get_char_bitmap('?');
        ASSERT_NOT_NULL(bmp_unknown, "unknown char returns non-NULL");
        int same = 1;
        for (int i = 0; i < 8; i++) {
            if (bmp_unknown[i] != bmp_question[i]) same = 0;
        }
        ASSERT_TRUE(same, "unknown char falls back to '?' bitmap");
    }

    // Test: render_fill_rect writes correct pixels
    {
        // 4x3 buffer, BGRA (4 bytes/pixel)
        uint8_t buf[4 * 3 * 4];
        memset(buf, 0, sizeof(buf));
        lnotify_color c = {.r = 0xAA, .g = 0xBB, .b = 0xCC, .a = 0xFF};
        render_fill_rect(buf, 4 * 4, 4, 3, 1, 1, 2, 2, &c);
        // pixel at (1,1) should be filled
        int off = (1 * 4 + 1) * 4;
        ASSERT_EQ(buf[off + 0], 0xCC, "fill rect blue at (1,1)");
        ASSERT_EQ(buf[off + 1], 0xBB, "fill rect green at (1,1)");
        ASSERT_EQ(buf[off + 2], 0xAA, "fill rect red at (1,1)");
        ASSERT_EQ(buf[off + 3], 0xFF, "fill rect alpha at (1,1)");
        // pixel at (0,0) should be untouched
        ASSERT_EQ(buf[0], 0x00, "fill rect doesn't touch (0,0)");
    }
}
