#ifndef LNOTIFY_RENDER_UTIL_H
#define LNOTIFY_RENDER_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

// Toast position and dimensions on screen
typedef struct {
    int x;
    int y;
    int w;
    int h;
} toast_geometry;

// Convert an lnotify_color (RGBA) to a 4-byte BGRA array (framebuffer order).
void color_to_bgra(const lnotify_color *c, uint8_t bgra[4]);

// Test whether pixel (px,py) falls inside a rounded rectangle defined by
// top-left corner (rx,ry), dimensions (rw,rh), and corner radius.
// Returns true if inside, false otherwise.
bool point_in_rounded_rect(int px, int py, int rx, int ry,
                           int rw, int rh, int radius);

// Compute the screen position of a toast notification.
// position: one of "top-right", "top-left", "bottom-right", "bottom-left".
// Unknown strings default to "top-right".
// screen_w/screen_h: screen dimensions.
// toast_w/toast_h: toast dimensions.
// margin: distance from screen edge.
void compute_toast_geometry(toast_geometry *geom,
                            int screen_w, int screen_h,
                            const char *position,
                            int toast_w, int toast_h,
                            int margin);

// Fill a solid rectangle in a BGRA framebuffer.
// buf: pixel buffer (BGRA, 4 bytes per pixel), row stride = stride bytes.
// stride: bytes per row (typically buf_w * 4 for BGRA).
// buf_h: buffer height in pixels (for bounds checking).
// x, y, w, h: rectangle to fill (clipped to buffer bounds).
// color: fill color (converted to BGRA).
void render_fill_rect(uint8_t *buf, int stride, int buf_w, int buf_h,
                      int x, int y, int w, int h,
                      const lnotify_color *color);

#endif
