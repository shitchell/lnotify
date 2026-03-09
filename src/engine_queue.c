#include "engine_queue.h"
#include "queue.h"
#include "log.h"

static engine_detect_result queue_detect(session_context *ctx) {
    (void)ctx;
    return ENGINE_ACCEPT;
}

static bool queue_render(const notification *notif, const session_context *ctx) {
    (void)ctx;
    queue_push(&g_queue, notif);
    log_info("notification queued: %s", notif->body);
    return true;
}

static void queue_dismiss(void) {
    log_debug("queue engine has no dismiss");
}

engine engine_queue = {
    .name     = "queue",
    .priority = 100,
    .detect   = queue_detect,
    .render   = queue_render,
    .dismiss  = queue_dismiss,
};
