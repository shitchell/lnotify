#include "render_util.h"
#include <string.h>

void color_to_bgra(const lnotify_color *c, uint8_t bgra[4]) {
    bgra[0] = c->b;
    bgra[1] = c->g;
    bgra[2] = c->r;
    bgra[3] = c->a;
}

bool point_in_rounded_rect(int px, int py, int rx, int ry,
                           int rw, int rh, int radius) {
    // Quick reject: outside bounding box
    if (px < rx || px >= rx + rw || py < ry || py >= ry + rh) {
        return false;
    }

    // No rounding — it's a plain rectangle
    if (radius <= 0) {
        return true;
    }

    // Check if point is in a corner region
    // Corner arc centers:
    //   top-left:     (rx + radius, ry + radius)
    //   top-right:    (rx + rw - radius - 1, ry + radius)
    //   bottom-left:  (rx + radius, ry + rh - radius - 1)
    //   bottom-right: (rx + rw - radius - 1, ry + rh - radius - 1)
    int cx = -1, cy = -1;

    bool in_left   = (px < rx + radius);
    bool in_right  = (px > rx + rw - radius - 1);
    bool in_top    = (py < ry + radius);
    bool in_bottom = (py > ry + rh - radius - 1);

    if (in_left && in_top) {
        cx = rx + radius;
        cy = ry + radius;
    } else if (in_right && in_top) {
        cx = rx + rw - radius - 1;
        cy = ry + radius;
    } else if (in_left && in_bottom) {
        cx = rx + radius;
        cy = ry + rh - radius - 1;
    } else if (in_right && in_bottom) {
        cx = rx + rw - radius - 1;
        cy = ry + rh - radius - 1;
    }

    if (cx >= 0) {
        // Point is in a corner region — check distance from arc center
        int dx = px - cx;
        int dy = py - cy;
        return (dx * dx + dy * dy) <= (radius * radius);
    }

    // Point is in the body of the rectangle (not a corner region)
    return true;
}

void compute_toast_geometry(toast_geometry *geom,
                            int screen_w, int screen_h,
                            const char *position,
                            int toast_w, int toast_h,
                            int margin) {
    geom->w = toast_w;
    geom->h = toast_h;

    // Default to top-right
    int left = 0;
    int top = 0;

    if (position) {
        if (strcmp(position, "top-left") == 0) {
            left = 1; top = 1;
        } else if (strcmp(position, "top-right") == 0) {
            left = 0; top = 1;
        } else if (strcmp(position, "bottom-left") == 0) {
            left = 1; top = 0;
        } else if (strcmp(position, "bottom-right") == 0) {
            left = 0; top = 0;
        } else {
            // Unknown — default to top-right
            left = 0; top = 1;
        }
    } else {
        left = 0; top = 1;
    }

    geom->x = left ? margin : (screen_w - toast_w - margin);
    geom->y = top  ? margin : (screen_h - toast_h - margin);
}

void render_fill_rect(uint8_t *buf, int stride, int buf_w, int buf_h,
                      int x, int y, int w, int h,
                      const lnotify_color *color) {
    uint8_t bgra[4];
    color_to_bgra(color, bgra);

    // Clip to buffer bounds
    clip_rect c = clip_to_bounds(x, y, w, h, buf_w, buf_h);

    for (int row = c.y0; row < c.y1; row++) {
        uint8_t *row_ptr = buf + row * stride;
        for (int col = c.x0; col < c.x1; col++) {
            uint8_t *px = row_ptr + col * 4;
            memcpy(px, bgra, 4);
        }
    }
}
