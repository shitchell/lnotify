#define _GNU_SOURCE  // for setresuid/setresgid

#include "engine_dbus.h"
#include "log.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// -------------------------------------------------------------------
//  Constants
// -------------------------------------------------------------------

#define DBUS_RETRY_BASE_MS     200
#define DBUS_RETRY_MULTIPLIER  1.5
#define DBUS_RETRY_MAX_ATTEMPTS 5

// -------------------------------------------------------------------
//  detect()
// -------------------------------------------------------------------

static engine_detect_result dbus_detect(session_context *ctx) {
    // D-Bus notification servers only run under compositors (wayland/x11)
    if (!ctx->session_type ||
        (strcmp(ctx->session_type, "wayland") != 0 &&
         strcmp(ctx->session_type, "x11") != 0)) {
        log_debug("engine_dbus: rejecting session_type=%s (not wayland/x11)",
                  ctx->session_type ? ctx->session_type : "(null)");
        return ENGINE_REJECT;
    }

    // Probe for the notification service on the session bus
    if (!context_probe_done(ctx, PROBE_HAS_DBUS_NOTIFICATIONS)) {
        ctx->requested_probe = PROBE_HAS_DBUS_NOTIFICATIONS;
        log_debug("engine_dbus: requesting PROBE_HAS_DBUS_NOTIFICATIONS");
        return ENGINE_NEED_PROBE;
    }

    if (ctx->has_dbus_notifications) {
        log_debug("engine_dbus: accepting (notification server available)");
        return ENGINE_ACCEPT;
    }

    log_debug("engine_dbus: rejecting (no notification server)");
    return ENGINE_REJECT;
}

// -------------------------------------------------------------------
//  dbus_call_as_user() — run a callback on a user's session bus
// -------------------------------------------------------------------

int dbus_call_as_user(uint32_t target_uid, dbus_user_fn fn, void *userdata) {
    uid_t my_uid = getuid();

    if ((uint32_t)my_uid == target_uid) {
        // Same user — open bus directly
        sd_bus *bus = NULL;
        int r = sd_bus_open_user(&bus);
        if (r < 0) return r;
        r = fn(bus, userdata);
        sd_bus_unref(bus);
        return r;
    }

    // Cross-user — fork, become target user, open their bus
    pid_t pid = fork();

    if (pid < 0) {
        log_error("dbus_call_as_user: fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        // Child: become the target user
        char xdg[64];
        snprintf(xdg, sizeof(xdg), "/run/user/%u", target_uid);
        setenv("XDG_RUNTIME_DIR", xdg, 1);

        // Set GID before UID (required order)
        if (setresgid(target_uid, target_uid, target_uid) < 0) _exit(127);
        if (setresuid(target_uid, target_uid, target_uid) < 0) _exit(127);

        sd_bus *bus = NULL;
        if (sd_bus_open_user(&bus) < 0) _exit(1);

        int rc = fn(bus, userdata);
        sd_bus_unref(bus);
        _exit(rc < 0 ? 1 : 0);
    }

    // Parent: wait for child
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        log_error("dbus_call_as_user: waitpid failed: %s", strerror(errno));
        return -1;
    }

    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        return exit_code == 0 ? 0 : -1;
    }
    return -1;
}

// -------------------------------------------------------------------
//  send_notification callback
// -------------------------------------------------------------------

typedef struct {
    const char *title;
    const char *body;
    int32_t timeout_ms;
} dbus_notify_args;

static int send_notification(sd_bus *bus, void *userdata) {
    dbus_notify_args *args = userdata;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;

    // Build Notify message manually (signature "susssasa{sv}i")
    // Need low-level API for empty arrays
    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_method_call(bus, &m,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify");
    if (r < 0) return r;

    r = sd_bus_message_append(m, "susss",
        "lnotify",                              // app_name
        (uint32_t)0,                            // replaces_id
        "",                                     // app_icon
        args->title ? args->title : "",         // summary
        args->body ? args->body : "");          // body
    if (r < 0) { sd_bus_message_unref(m); return r; }

    // Empty actions array: as
    r = sd_bus_message_open_container(m, 'a', "s");
    if (r < 0) { sd_bus_message_unref(m); return r; }
    r = sd_bus_message_close_container(m);
    if (r < 0) { sd_bus_message_unref(m); return r; }

    // Empty hints dict: a{sv}
    r = sd_bus_message_open_container(m, 'a', "{sv}");
    if (r < 0) { sd_bus_message_unref(m); return r; }
    r = sd_bus_message_close_container(m);
    if (r < 0) { sd_bus_message_unref(m); return r; }

    // Timeout
    int32_t timeout = args->timeout_ms > 0 ? args->timeout_ms : 5000;
    r = sd_bus_message_append(m, "i", timeout);
    if (r < 0) { sd_bus_message_unref(m); return r; }

    r = sd_bus_call(bus, m, 0, &error, &reply);
    sd_bus_message_unref(m);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return r;
}

// -------------------------------------------------------------------
//  Execute with retry
// -------------------------------------------------------------------

static bool dbus_exec_with_retry(uint32_t target_uid, dbus_user_fn fn,
                                 void *userdata) {
    int delay_ms = DBUS_RETRY_BASE_MS;

    for (int attempt = 1; attempt <= DBUS_RETRY_MAX_ATTEMPTS; attempt++) {
        int rc = dbus_call_as_user(target_uid, fn, userdata);
        if (rc >= 0) {
            log_debug("engine_dbus: succeeded on attempt %d (uid=%u)",
                      attempt, target_uid);
            return true;
        }

        if (attempt < DBUS_RETRY_MAX_ATTEMPTS) {
            log_debug("engine_dbus: failed (attempt %d/%d, uid=%u), "
                      "retrying in %dms",
                      attempt, DBUS_RETRY_MAX_ATTEMPTS, target_uid, delay_ms);
            usleep((useconds_t)delay_ms * 1000);
            delay_ms = (int)(delay_ms * DBUS_RETRY_MULTIPLIER);
        }
    }

    log_error("engine_dbus: failed after %d attempts (uid=%u)",
              DBUS_RETRY_MAX_ATTEMPTS, target_uid);
    return false;
}

// -------------------------------------------------------------------
//  render()
// -------------------------------------------------------------------

static bool dbus_render(const notification *notif,
                         const session_context *ctx) {
    dbus_notify_args args = {
        .title = notif->title,
        .body = notif->body,
        .timeout_ms = notif->timeout_ms,
    };

    bool success = dbus_exec_with_retry(ctx->uid, send_notification, &args);

    if (success) {
        log_info("engine_dbus: notification delivered via D-Bus");
    }

    return success;
}

// -------------------------------------------------------------------
//  dismiss() — no-op for D-Bus
// -------------------------------------------------------------------

static void dbus_dismiss(void) {
    // The notification server handles its own timeout/dismissal.
    // Nothing for us to do.
    log_debug("engine_dbus: dismiss (no-op, server handles timeout)");
}

// -------------------------------------------------------------------
//  Engine vtable
// -------------------------------------------------------------------

engine engine_dbus = {
    .name     = "dbus",
    .priority = 10,  // highest priority — tried first
    .detect   = dbus_detect,
    .render   = dbus_render,
    .dismiss  = dbus_dismiss,
};
