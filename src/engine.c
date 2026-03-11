#include "engine.h"
#include "engine_dbus.h"
#include "log.h"
#include "logind.h"

#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// context_init_from_logind
// ---------------------------------------------------------------------------

void context_init_from_logind(session_context *ctx, uint32_t vt) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->vt = vt;

    // Defaults (heap-allocated so context_free is uniform)
    ctx->username      = strdup("");
    ctx->session_type  = strdup("");
    ctx->session_class = strdup("");
    ctx->seat          = strdup("");
    ctx->compositor_name     = NULL;
    ctx->terminal_type       = NULL;
    ctx->foreground_process  = NULL;

    // Check initial strdup calls (OOM: leave fields NULL, log once)
    if (!ctx->username || !ctx->session_type ||
        !ctx->session_class || !ctx->seat) {
        log_error("context_init_from_logind: strdup failed for defaults");
    }

    logind_session session;
    if (logind_get_session_by_vt(vt, &session) < 0) {
        log_debug("context_init_from_logind: no session found for VT %u", vt);
        return;
    }

    log_debug("context_init_from_logind: VT %u -> session %s", vt, session.session_id);

    // Transfer fields (replace defaults). Use set_str pattern: only replace
    // if strdup succeeds, so OOM doesn't destroy existing value.
    char *tmp;

    tmp = strdup(session.type ? session.type : "");
    if (tmp) { free(ctx->session_type); ctx->session_type = tmp; }
    else { log_error("context_init_from_logind: strdup failed for session_type"); }

    tmp = strdup(session.session_class ? session.session_class : "");
    if (tmp) { free(ctx->session_class); ctx->session_class = tmp; }
    else { log_error("context_init_from_logind: strdup failed for session_class"); }

    tmp = strdup(session.username ? session.username : "");
    if (tmp) { free(ctx->username); ctx->username = tmp; }
    else { log_error("context_init_from_logind: strdup failed for username"); }

    tmp = strdup(session.seat ? session.seat : "");
    if (tmp) { free(ctx->seat); ctx->seat = tmp; }
    else { log_error("context_init_from_logind: strdup failed for seat"); }

    ctx->uid = session.uid;
    ctx->remote = session.remote;

    log_debug("context_init_from_logind: type=%s class=%s user=%s(%u) "
              "seat=%s remote=%d",
              ctx->session_type ? ctx->session_type : "(null)",
              ctx->session_class ? ctx->session_class : "(null)",
              ctx->username ? ctx->username : "(null)",
              ctx->uid,
              ctx->seat ? ctx->seat : "(null)",
              ctx->remote);

    logind_session_free(&session);
}

// ---------------------------------------------------------------------------
// context_free
// ---------------------------------------------------------------------------

void context_free(session_context *ctx) {
    free(ctx->username);
    free(ctx->session_type);
    free(ctx->session_class);
    free(ctx->seat);
    free(ctx->compositor_name);
    free(ctx->terminal_type);
    free(ctx->foreground_process);
    memset(ctx, 0, sizeof(*ctx));
}

// ---------------------------------------------------------------------------
// Probe implementations
// ---------------------------------------------------------------------------

// Callback for D-Bus notification service probe
static int introspect_notifications(sd_bus *bus, void *userdata) {
    (void)userdata;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(bus,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.DBus.Introspectable",
        "Introspect",
        &error, &reply, "");
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return r;
}

static void probe_has_dbus_notifications(session_context *ctx) {
    int rc = dbus_call_as_user(ctx->uid, introspect_notifications, NULL);
    ctx->has_dbus_notifications = (rc >= 0);
    log_debug("probe: has_dbus_notifications = %d", ctx->has_dbus_notifications);
}

static void probe_compositor_name(session_context *ctx) {
    // Stub: not yet implemented
    log_debug("probe: compositor_name not yet implemented");
    ctx->compositor_name = NULL;
}

static void probe_has_framebuffer(session_context *ctx) {
    ctx->has_framebuffer = (access("/dev/fb0", W_OK) == 0);
    log_debug("probe: has_framebuffer = %d", ctx->has_framebuffer);
}

static void probe_terminal_capabilities(session_context *ctx) {
    // Stub: not yet implemented
    log_debug("probe: terminal_capabilities not yet implemented");
    ctx->terminal_type = NULL;
    ctx->terminal_supports_osc = false;
}

static void probe_foreground_process(session_context *ctx) {
    // Stub: not yet implemented
    log_debug("probe: foreground_process not yet implemented");
    ctx->foreground_process = NULL;
}

// ---------------------------------------------------------------------------
// context_run_probe
// ---------------------------------------------------------------------------

void context_run_probe(session_context *ctx, probe_key key) {
    if (context_probe_done(ctx, key)) {
        log_debug("probe %d already completed, skipping", key);
        return;
    }

    switch (key) {
    case PROBE_HAS_DBUS_NOTIFICATIONS:
        probe_has_dbus_notifications(ctx);
        break;
    case PROBE_COMPOSITOR_NAME:
        probe_compositor_name(ctx);
        break;
    case PROBE_HAS_FRAMEBUFFER:
        probe_has_framebuffer(ctx);
        break;
    case PROBE_TERMINAL_CAPABILITIES:
        probe_terminal_capabilities(ctx);
        break;
    case PROBE_FOREGROUND_PROCESS:
        probe_foreground_process(ctx);
        break;
    case PROBE_COUNT:
        log_error("context_run_probe: invalid probe key PROBE_COUNT");
        return;
    }

    ctx->probes_completed |= (1u << key);
}
