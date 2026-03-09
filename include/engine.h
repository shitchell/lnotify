#ifndef LNOTIFY_ENGINE_H
#define LNOTIFY_ENGINE_H

#include "lnotify.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_ENGINES 32

typedef enum {
    ENGINE_ACCEPT,
    ENGINE_REJECT,
    ENGINE_NEED_PROBE,
} engine_detect_result;

typedef enum {
    PROBE_HAS_DBUS_NOTIFICATIONS,
    PROBE_COMPOSITOR_NAME,
    PROBE_HAS_FRAMEBUFFER,
    PROBE_TERMINAL_CAPABILITIES,
    PROBE_FOREGROUND_PROCESS,
    PROBE_COUNT,  // sentinel -- must be last
} probe_key;

typedef struct {
    // Phase 0: from logind (always available)
    uint32_t    vt;
    uint32_t    uid;
    const char *username;
    const char *session_type;     // "wayland", "x11", "tty", ""
    const char *session_class;    // "user", "greeter"
    const char *seat;
    bool        remote;

    // Probed fields (populated on demand)
    bool        has_dbus_notifications;
    const char *compositor_name;
    bool        has_framebuffer;
    const char *terminal_type;
    bool        terminal_supports_osc;
    const char *foreground_process;

    // Probe bookkeeping
    uint32_t    probes_completed;   // bitfield of probe_key
    probe_key   requested_probe;
} session_context;

typedef struct engine {
    const char *name;
    int priority;

    engine_detect_result (*detect)(session_context *ctx);
    bool (*render)(const notification *notif, const session_context *ctx);
    void (*dismiss)(void);
} engine;

// Initialize a session context with logind data for a given VT.
// Queries loginctl for session properties.
void context_init_from_logind(session_context *ctx, uint32_t vt);

// Free heap-allocated strings in a session context.
void context_free(session_context *ctx);

// Run a specific probe and update the context.
void context_run_probe(session_context *ctx, probe_key key);

// Check if a probe has already been run.
static inline bool context_probe_done(const session_context *ctx,
                                       probe_key key) {
    return (ctx->probes_completed & (1u << key)) != 0;
}

#endif
