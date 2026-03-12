#include "lnotify.h"
#include <string.h>

uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint64_t wallclock_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int notification_init(notification *n, const char *title, const char *body) {
    memset(n, 0, sizeof(*n));
    n->title = title ? strdup(title) : NULL;
    n->body = body ? strdup(body) : NULL;
    n->priority = 1;        // normal
    n->timeout_ms = -1;     // use config default
    n->ts_sent = wallclock_ms();

    // Check for strdup failures on non-NULL inputs
    if ((title && !n->title) || (body && !n->body)) {
        notification_free(n);
        return -1;
    }
    return 0;
}

int notification_copy(notification *dst, const notification *src) {
    if (notification_init(dst, src->title, src->body) < 0) return -1;

    dst->id          = src->id;
    dst->priority    = src->priority;
    dst->timeout_ms  = src->timeout_ms;
    dst->origin_uid  = src->origin_uid;
    dst->ts_sent     = src->ts_sent;
    dst->ts_received = src->ts_received;
    dst->ts_mono     = src->ts_mono;

    if (src->app) {
        dst->app = strdup(src->app);
        if (!dst->app) { notification_free(dst); return -1; }
    }
    if (src->group_id) {
        dst->group_id = strdup(src->group_id);
        if (!dst->group_id) { notification_free(dst); return -1; }
    }

    return 0;
}

void notification_free(notification *n) {
    free(n->title);
    free(n->body);
    free(n->app);
    free(n->group_id);
    n->title = n->body = n->app = n->group_id = NULL;
}

bool notification_expired(const notification *n, uint64_t now_mono) {
    if (n->timeout_ms < 0 || n->ts_mono == 0) return false;
    return (now_mono - n->ts_mono) >= (uint64_t)n->timeout_ms;
}

int32_t notification_remaining_ms(const notification *n, uint64_t now_mono) {
    if (n->timeout_ms < 0 || n->ts_mono == 0) return n->timeout_ms;
    uint64_t elapsed = now_mono - n->ts_mono;
    if (elapsed >= (uint64_t)n->timeout_ms) return 0;
    return (int32_t)((uint64_t)n->timeout_ms - elapsed);
}

bool notification_group_matches(const notification *a, const notification *b) {
    if (!a->group_id || !b->group_id) return false;
    return strcmp(a->group_id, b->group_id) == 0;
}
