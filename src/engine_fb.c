#include "engine_fb.h"
#include "config.h"
#include "font_bitmap.h"
#include "log.h"
#include "lnotify.h"
#include "queue.h"
#include "render_util.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// -------------------------------------------------------------------
//  Internal state (module-level, protected by fb_mutex)
// -------------------------------------------------------------------

static pthread_mutex_t fb_mutex = PTHREAD_MUTEX_INITIALIZER;

// Framebuffer device state
static int          fb_fd       = -1;
static uint8_t     *fb_map      = NULL;
static size_t       fb_map_size = 0;
static int          fb_stride   = 0;   // bytes per row
static int          fb_width    = 0;   // screen width in pixels
static int          fb_height   = 0;   // screen height in pixels

// Saved region (heap-allocated pixel data under the toast)
static uint8_t     *saved_region    = NULL;
static toast_geometry saved_geom    = {0};

// Toast pixel snapshot for verification (a few sample pixels)
#define VERIFY_SAMPLE_COUNT 20
static uint8_t      verify_samples[VERIFY_SAMPLE_COUNT][4]; // BGRA
static int           verify_x[VERIFY_SAMPLE_COUNT];
static int           verify_y[VERIFY_SAMPLE_COUNT];

// Defense thread
static pthread_t    defense_thread;
static bool         defense_running = false;
static bool         defense_stop    = false;

// Notification info the defense thread needs for re-render and queue fallback
static notification defense_notif;          // deep copy
static int          defense_timeout_ms;     // total timeout for this notification
static uint64_t     defense_start_mono;     // when render started (monotonic ms)

// Config snapshot the defense thread needs
static lnotify_config *active_config = NULL;

// Forward declarations
static void fb_compute_geometry(const notification *notif, const lnotify_config *cfg);
static void fb_draw_toast(const notification *notif, const lnotify_config *cfg);
static void fb_save_region(void);
static void fb_restore_region(void);
static void fb_record_verify_samples(void);
static bool fb_verify_visible(void);
static void *defense_thread_fn(void *arg);
static void fb_cleanup_unlocked(void);

// -------------------------------------------------------------------
//  detect()
// -------------------------------------------------------------------

static engine_detect_result fb_detect(session_context *ctx) {
    // Compositors bypass /dev/fb0 entirely -- reject
    if (ctx->session_type) {
        if (strcmp(ctx->session_type, "wayland") == 0 ||
            strcmp(ctx->session_type, "x11") == 0) {
            log_debug("engine_fb: rejecting session_type=%s", ctx->session_type);
            return ENGINE_REJECT;
        }
    }

    // Need to probe for framebuffer access
    if (!context_probe_done(ctx, PROBE_HAS_FRAMEBUFFER)) {
        ctx->requested_probe = PROBE_HAS_FRAMEBUFFER;
        log_debug("engine_fb: requesting PROBE_HAS_FRAMEBUFFER");
        return ENGINE_NEED_PROBE;
    }

    if (ctx->has_framebuffer) {
        log_debug("engine_fb: accepting (fb0 writable)");
        return ENGINE_ACCEPT;
    }

    log_debug("engine_fb: rejecting (fb0 not writable)");
    return ENGINE_REJECT;
}

// -------------------------------------------------------------------
//  Drawing helpers
// -------------------------------------------------------------------

// Draw a single character at (x, y) in the framebuffer with the given color
// and scale factor.
static void fb_draw_char(char ch, int x, int y, int scale,
                          const lnotify_color *color) {
    const uint8_t *bitmap = get_char_bitmap(ch);
    uint8_t bgra[4];
    color_to_bgra(color, bgra);

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = bitmap[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                // Fill a scale x scale block
                for (int sy = 0; sy < scale; sy++) {
                    int py = y + row * scale + sy;
                    if (py < 0 || py >= fb_height) continue;
                    uint8_t *row_ptr = fb_map + py * fb_stride;
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        if (px < 0 || px >= fb_width) continue;
                        uint8_t *pixel = row_ptr + px * 4;
                        pixel[0] = bgra[0];
                        pixel[1] = bgra[1];
                        pixel[2] = bgra[2];
                        pixel[3] = bgra[3];
                    }
                }
            }
        }
    }
}

// Draw a string at (x, y) in the framebuffer.
static void fb_draw_text(const char *text, int x, int y, int scale,
                          const lnotify_color *color) {
    if (!text) return;
    int cx = x;
    for (const char *p = text; *p; p++) {
        fb_draw_char(*p, cx, y, scale, color);
        cx += FONT_WIDTH * scale;
    }
}

// Draw a rounded rectangle (filled) directly into the framebuffer.
static void fb_draw_rounded_rect(int rx, int ry, int rw, int rh,
                                  int radius, const lnotify_color *color) {
    uint8_t bgra[4];
    color_to_bgra(color, bgra);

    // Clip to screen
    int x0 = rx < 0 ? 0 : rx;
    int y0 = ry < 0 ? 0 : ry;
    int x1 = rx + rw;
    int y1 = ry + rh;
    if (x1 > fb_width)  x1 = fb_width;
    if (y1 > fb_height) y1 = fb_height;

    for (int py = y0; py < y1; py++) {
        uint8_t *row_ptr = fb_map + py * fb_stride;
        for (int px = x0; px < x1; px++) {
            if (point_in_rounded_rect(px, py, rx, ry, rw, rh, radius)) {
                uint8_t *pixel = row_ptr + px * 4;
                pixel[0] = bgra[0];
                pixel[1] = bgra[1];
                pixel[2] = bgra[2];
                pixel[3] = bgra[3];
            }
        }
    }
}

// Draw a rounded-rect border (outline only) by drawing two nested rects.
static void fb_draw_rounded_border(int rx, int ry, int rw, int rh,
                                    int radius, int border_w,
                                    const lnotify_color *color) {
    uint8_t bgra[4];
    color_to_bgra(color, bgra);

    int x0 = rx < 0 ? 0 : rx;
    int y0 = ry < 0 ? 0 : ry;
    int x1 = rx + rw;
    int y1 = ry + rh;
    if (x1 > fb_width)  x1 = fb_width;
    if (y1 > fb_height) y1 = fb_height;

    int inner_radius = radius > border_w ? radius - border_w : 0;

    for (int py = y0; py < y1; py++) {
        uint8_t *row_ptr = fb_map + py * fb_stride;
        for (int px = x0; px < x1; px++) {
            bool in_outer = point_in_rounded_rect(px, py, rx, ry, rw, rh,
                                                   radius);
            bool in_inner = point_in_rounded_rect(px, py,
                                                   rx + border_w,
                                                   ry + border_w,
                                                   rw - 2 * border_w,
                                                   rh - 2 * border_w,
                                                   inner_radius);
            if (in_outer && !in_inner) {
                uint8_t *pixel = row_ptr + px * 4;
                pixel[0] = bgra[0];
                pixel[1] = bgra[1];
                pixel[2] = bgra[2];
                pixel[3] = bgra[3];
            }
        }
    }
}

// -------------------------------------------------------------------
//  fb_draw_toast — draws the complete toast into the framebuffer
// -------------------------------------------------------------------

// Compute toast dimensions and populate saved_geom.
// Must be called before fb_draw_toast and fb_save_region.
static void fb_compute_geometry(const notification *notif,
                                 const lnotify_config *cfg) {
    int scale = cfg->font_size / FONT_HEIGHT;
    if (scale < 1) scale = 1;

    int char_h = FONT_HEIGHT * scale;
    int line_spacing = char_h / 2;
    if (line_spacing < 2) line_spacing = 2;

    int title_w = text_width(notif->title, scale);
    int body_w  = text_width(notif->body, scale);
    int content_w = title_w > body_w ? title_w : body_w;
    int content_h = 0;
    if (notif->title) {
        content_h += char_h + line_spacing;
    }
    content_h += char_h;

    int toast_w = content_w + 2 * cfg->padding + 2 * cfg->border_width;
    int toast_h = content_h + 2 * cfg->padding + 2 * cfg->border_width;

    if (toast_w < 100) toast_w = 100;
    if (toast_h < 40)  toast_h = 40;

    compute_toast_geometry(&saved_geom, fb_width, fb_height,
                            cfg->position, toast_w, toast_h, cfg->margin);
}

// Draw the toast into the framebuffer using saved_geom (must call
// fb_compute_geometry first).
static void fb_draw_toast(const notification *notif,
                           const lnotify_config *cfg) {
    int scale = cfg->font_size / FONT_HEIGHT;
    if (scale < 1) scale = 1;

    int char_h = FONT_HEIGHT * scale;
    int line_spacing = char_h / 2;
    if (line_spacing < 2) line_spacing = 2;

    // Draw background (rounded rect)
    fb_draw_rounded_rect(saved_geom.x, saved_geom.y,
                          saved_geom.w, saved_geom.h,
                          cfg->border_radius, &cfg->bg_color);

    // Draw border (outline)
    if (cfg->border_width > 0) {
        fb_draw_rounded_border(saved_geom.x, saved_geom.y,
                                saved_geom.w, saved_geom.h,
                                cfg->border_radius, cfg->border_width,
                                &cfg->border_color);
    }

    // Draw text
    int text_x = saved_geom.x + cfg->border_width + cfg->padding;
    int text_y = saved_geom.y + cfg->border_width + cfg->padding;

    if (notif->title) {
        fb_draw_text(notif->title, text_x, text_y, scale, &cfg->fg_color);
        text_y += char_h + line_spacing;
    }
    fb_draw_text(notif->body, text_x, text_y, scale, &cfg->fg_color);
}

// -------------------------------------------------------------------
//  Region save / restore
// -------------------------------------------------------------------

static void fb_save_region(void) {
    if (saved_region) {
        free(saved_region);
        saved_region = NULL;
    }

    int w = saved_geom.w;
    int h = saved_geom.h;
    if (w <= 0 || h <= 0) return;

    size_t row_bytes = (size_t)w * 4;
    saved_region = malloc(row_bytes * (size_t)h);
    if (!saved_region) {
        log_error("engine_fb: failed to allocate saved region (%dx%d)", w, h);
        return;
    }

    for (int row = 0; row < h; row++) {
        int sy = saved_geom.y + row;
        if (sy < 0 || sy >= fb_height) continue;
        const uint8_t *src = fb_map + sy * fb_stride + saved_geom.x * 4;
        uint8_t *dst = saved_region + (size_t)row * row_bytes;
        int copy_w = w;
        if (saved_geom.x + copy_w > fb_width) {
            copy_w = fb_width - saved_geom.x;
        }
        if (saved_geom.x < 0) continue;
        memcpy(dst, src, (size_t)copy_w * 4);
    }
}

static void fb_restore_region(void) {
    if (!saved_region || !fb_map) return;

    int w = saved_geom.w;
    int h = saved_geom.h;
    size_t row_bytes = (size_t)w * 4;

    for (int row = 0; row < h; row++) {
        int sy = saved_geom.y + row;
        if (sy < 0 || sy >= fb_height) continue;
        uint8_t *dst = fb_map + sy * fb_stride + saved_geom.x * 4;
        const uint8_t *src = saved_region + (size_t)row * row_bytes;
        int copy_w = w;
        if (saved_geom.x + copy_w > fb_width) {
            copy_w = fb_width - saved_geom.x;
        }
        if (saved_geom.x < 0) continue;
        memcpy(dst, src, (size_t)copy_w * 4);
    }
}

// -------------------------------------------------------------------
//  Verification — sample border pixels to detect clobbering
// -------------------------------------------------------------------

static void fb_record_verify_samples(void) {
    // Sample points along the border of the toast region.
    // We pick points on each edge, evenly spaced.
    int gx = saved_geom.x;
    int gy = saved_geom.y;
    int gw = saved_geom.w;
    int gh = saved_geom.h;

    int idx = 0;
    for (int i = 0; i < VERIFY_SAMPLE_COUNT && idx < VERIFY_SAMPLE_COUNT; i++) {
        int px, py;
        int edge = i % 4;
        int step = i / 4 + 1;
        switch (edge) {
            case 0: // top edge
                px = gx + (gw * step) / (VERIFY_SAMPLE_COUNT / 4 + 1);
                py = gy + 1;
                break;
            case 1: // right edge
                px = gx + gw - 2;
                py = gy + (gh * step) / (VERIFY_SAMPLE_COUNT / 4 + 1);
                break;
            case 2: // bottom edge
                px = gx + (gw * step) / (VERIFY_SAMPLE_COUNT / 4 + 1);
                py = gy + gh - 2;
                break;
            case 3: // left edge
                px = gx + 1;
                py = gy + (gh * step) / (VERIFY_SAMPLE_COUNT / 4 + 1);
                break;
            default:
                px = gx; py = gy;
                break;
        }

        // Clamp to screen
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        if (px >= fb_width)  px = fb_width - 1;
        if (py >= fb_height) py = fb_height - 1;

        verify_x[idx] = px;
        verify_y[idx] = py;

        // Read the current pixel
        const uint8_t *pixel = fb_map + py * fb_stride + px * 4;
        verify_samples[idx][0] = pixel[0];
        verify_samples[idx][1] = pixel[1];
        verify_samples[idx][2] = pixel[2];
        verify_samples[idx][3] = pixel[3];
        idx++;
    }
}

static bool fb_verify_visible(void) {
    if (!fb_map) return false;

    int match = 0;
    for (int i = 0; i < VERIFY_SAMPLE_COUNT; i++) {
        int px = verify_x[i];
        int py = verify_y[i];
        if (px < 0 || py < 0 || px >= fb_width || py >= fb_height) continue;

        const uint8_t *pixel = fb_map + py * fb_stride + px * 4;
        if (pixel[0] == verify_samples[i][0] &&
            pixel[1] == verify_samples[i][1] &&
            pixel[2] == verify_samples[i][2] &&
            pixel[3] == verify_samples[i][3]) {
            match++;
        }
    }

    // >80% match threshold
    return match > (VERIFY_SAMPLE_COUNT * 80 / 100);
}

// -------------------------------------------------------------------
//  Cleanup (called under lock, no thread join)
// -------------------------------------------------------------------

static void fb_cleanup_unlocked(void) {
    fb_restore_region();

    if (saved_region) {
        free(saved_region);
        saved_region = NULL;
    }
    if (fb_map) {
        munmap(fb_map, fb_map_size);
        fb_map = NULL;
        fb_map_size = 0;
    }
    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }

    notification_free(&defense_notif);
    memset(&defense_notif, 0, sizeof(defense_notif));

    fb_width = 0;
    fb_height = 0;
    fb_stride = 0;
}

// -------------------------------------------------------------------
//  Defense thread
// -------------------------------------------------------------------

static void *defense_thread_fn(void *arg) {
    (void)arg;
    int consecutive_failures = 0;

    while (1) {
        // Sleep 200ms between checks
        usleep(200 * 1000);

        pthread_mutex_lock(&fb_mutex);

        if (defense_stop) {
            pthread_mutex_unlock(&fb_mutex);
            break;
        }

        // Check if we've exceeded timeout/2 for sustained defense
        uint64_t now = monotonic_ms();
        uint64_t elapsed = now - defense_start_mono;
        uint64_t defense_duration = (uint64_t)defense_timeout_ms / 2;
        if (elapsed > defense_duration) {
            log_debug("engine_fb: defense duration exceeded, stopping");
            pthread_mutex_unlock(&fb_mutex);
            break;
        }

        if (!fb_map) {
            // Framebuffer gone, nothing to defend
            pthread_mutex_unlock(&fb_mutex);
            break;
        }

        if (!fb_verify_visible()) {
            consecutive_failures++;
            log_debug("engine_fb: toast clobbered (failure %d/3)",
                      consecutive_failures);

            if (consecutive_failures >= 3) {
                // Give up — clean up directly (NOT through dismiss!)
                // Push notification to queue for later delivery
                log_info("engine_fb: 3 consecutive defense failures, "
                         "pushing to queue");
                queue_push(&g_queue, &defense_notif);

                fb_cleanup_unlocked();
                defense_running = false;
                pthread_mutex_unlock(&fb_mutex);
                return NULL;
            }

            // Re-render the toast
            if (active_config) {
                fb_draw_toast(&defense_notif, active_config);
                fb_record_verify_samples();
            }
        } else {
            consecutive_failures = 0;
        }

        pthread_mutex_unlock(&fb_mutex);
    }

    return NULL;
}

// -------------------------------------------------------------------
//  render()
// -------------------------------------------------------------------

static bool fb_render(const notification *notif,
                       const session_context *ctx) {
    (void)ctx;  // session_context not needed for rendering

    // Get config — for now use defaults. The daemon will provide the real
    // config through a global or parameter in the future.
    // TODO: Pass config through a global or extend the engine vtable.
    lnotify_config cfg;
    config_defaults(&cfg);

    pthread_mutex_lock(&fb_mutex);

    // Open framebuffer device
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        log_error("engine_fb: cannot open /dev/fb0: %s", strerror(errno));
        pthread_mutex_unlock(&fb_mutex);
        config_free(&cfg);
        return false;
    }

    // Get screen info
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        log_error("engine_fb: FBIOGET_VSCREENINFO failed: %s",
                  strerror(errno));
        close(fb_fd);
        fb_fd = -1;
        pthread_mutex_unlock(&fb_mutex);
        config_free(&cfg);
        return false;
    }
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        log_error("engine_fb: FBIOGET_FSCREENINFO failed: %s",
                  strerror(errno));
        close(fb_fd);
        fb_fd = -1;
        pthread_mutex_unlock(&fb_mutex);
        config_free(&cfg);
        return false;
    }

    fb_width  = (int)vinfo.xres;
    fb_height = (int)vinfo.yres;
    fb_stride = (int)finfo.line_length;
    fb_map_size = (size_t)fb_stride * fb_height;

    log_debug("engine_fb: screen %dx%d, stride=%d, bpp=%d",
              fb_width, fb_height, fb_stride, vinfo.bits_per_pixel);

    if (vinfo.bits_per_pixel != 32) {
        log_error("engine_fb: unsupported bpp=%d (need 32)", vinfo.bits_per_pixel);
        close(fb_fd);
        fb_fd = -1;
        pthread_mutex_unlock(&fb_mutex);
        config_free(&cfg);
        return false;
    }

    // mmap the framebuffer
    fb_map = mmap(NULL, fb_map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fb_fd, 0);
    if (fb_map == MAP_FAILED) {
        log_error("engine_fb: mmap failed: %s", strerror(errno));
        fb_map = NULL;
        close(fb_fd);
        fb_fd = -1;
        pthread_mutex_unlock(&fb_mutex);
        config_free(&cfg);
        return false;
    }

    // Compute geometry first (needed for save_region), then save, then draw
    fb_compute_geometry(notif, &cfg);

    // Save the region underneath before drawing
    fb_save_region();

    // Draw the toast
    fb_draw_toast(notif, &cfg);

    // Record verify samples after drawing
    fb_record_verify_samples();

    // Verify at +16ms
    pthread_mutex_unlock(&fb_mutex);
    usleep(16 * 1000);
    pthread_mutex_lock(&fb_mutex);

    if (!fb_verify_visible()) {
        log_debug("engine_fb: verification failed at +16ms, retrying at +50ms");
        pthread_mutex_unlock(&fb_mutex);
        usleep(34 * 1000);  // total +50ms
        pthread_mutex_lock(&fb_mutex);

        if (!fb_verify_visible()) {
            log_error("engine_fb: verification failed at +50ms, giving up");
            fb_cleanup_unlocked();
            pthread_mutex_unlock(&fb_mutex);
            config_free(&cfg);
            return false;
        }
    }

    log_debug("engine_fb: toast verified visible");

    // Prepare defense thread state
    notification_init(&defense_notif, notif->title, notif->body);
    defense_notif.priority   = notif->priority;
    defense_notif.timeout_ms = notif->timeout_ms;
    defense_notif.ts_mono    = notif->ts_mono;
    defense_timeout_ms = notif->timeout_ms > 0 ? notif->timeout_ms : cfg.default_timeout;
    defense_start_mono = monotonic_ms();
    defense_stop = false;
    defense_running = true;

    // Store config for defense thread re-renders
    if (active_config) {
        config_free(active_config);
        free(active_config);
    }
    active_config = malloc(sizeof(lnotify_config));
    if (active_config) {
        config_defaults(active_config);
        // Copy style values from cfg (they're already defaults, but this
        // pattern is ready for when we receive the real config)
    }

    // Start defense thread
    int err = pthread_create(&defense_thread, NULL, defense_thread_fn, NULL);
    if (err != 0) {
        log_error("engine_fb: failed to create defense thread: %s",
                  strerror(err));
        defense_running = false;
        // Continue anyway — toast is displayed, just no defense
    }

    pthread_mutex_unlock(&fb_mutex);
    config_free(&cfg);
    return true;
}

// -------------------------------------------------------------------
//  dismiss()
// -------------------------------------------------------------------

static void fb_dismiss(void) {
    pthread_mutex_lock(&fb_mutex);

    // Signal defense thread to stop
    if (defense_running) {
        defense_stop = true;
        pthread_mutex_unlock(&fb_mutex);

        // Join the defense thread (safe — we're not the defense thread)
        pthread_join(defense_thread, NULL);

        pthread_mutex_lock(&fb_mutex);
        defense_running = false;
    }

    fb_cleanup_unlocked();

    if (active_config) {
        config_free(active_config);
        free(active_config);
        active_config = NULL;
    }

    pthread_mutex_unlock(&fb_mutex);
}

// -------------------------------------------------------------------
//  Engine vtable
// -------------------------------------------------------------------

engine engine_framebuffer = {
    .name     = "framebuffer",
    .priority = 30,  // after dbus (10), before terminal (40) and queue (50)
    .detect   = fb_detect,
    .render   = fb_render,
    .dismiss  = fb_dismiss,
};
