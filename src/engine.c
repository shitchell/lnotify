#include "engine.h"
#include "engine_dbus.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// strdup that returns "" instead of NULL on NULL input
static char *safe_strdup(const char *s) {
    return strdup(s ? s : "");
}

// Parse a loginctl property line "Key=Value\n" and return the value
// if the key matches, or NULL otherwise. Caller must free the result.
static char *parse_loginctl_prop(const char *line, const char *key) {
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0 || line[klen] != '=')
        return NULL;
    const char *val = line + klen + 1;
    // Trim trailing newline
    size_t vlen = strlen(val);
    while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r'))
        vlen--;
    char *dup = malloc(vlen + 1);
    if (!dup) return NULL;
    memcpy(dup, val, vlen);
    dup[vlen] = '\0';
    return dup;
}

// ---------------------------------------------------------------------------
// context_init_from_logind
// ---------------------------------------------------------------------------

void context_init_from_logind(session_context *ctx, uint32_t vt) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->vt = vt;

    // Defaults for string fields (heap-allocated so context_free is uniform)
    ctx->username      = safe_strdup("");
    ctx->session_type  = safe_strdup("");
    ctx->session_class = safe_strdup("");
    ctx->seat          = safe_strdup("");
    ctx->compositor_name     = NULL;
    ctx->terminal_type       = NULL;
    ctx->foreground_process  = NULL;

    // Build command: find session on this VT via loginctl
    // loginctl list-sessions --no-legend gives lines like:
    //   SESSION_ID UID USER SEAT TTY
    // We look for the one whose VTNr matches.
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "loginctl list-sessions --no-legend 2>/dev/null");

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        log_debug("context_init_from_logind: popen(loginctl list-sessions) "
                  "failed");
        return;
    }

    char line[512];
    char session_id[64] = {0};

    // First pass: find session ID for this VT
    while (fgets(line, sizeof(line), fp)) {
        // Extract first token (session id)
        char sid[64];
        if (sscanf(line, "%63s", sid) != 1)
            continue;

        // Query this session's VTNr
        char qcmd[256];
        snprintf(qcmd, sizeof(qcmd),
                 "loginctl show-session %s -p VTNr --value 2>/dev/null", sid);
        FILE *qfp = popen(qcmd, "r");
        if (!qfp) continue;

        char vt_str[32] = {0};
        if (fgets(vt_str, sizeof(vt_str), qfp)) {
            uint32_t svt = (uint32_t)atoi(vt_str);
            if (svt == vt) {
                strncpy(session_id, sid, sizeof(session_id) - 1);
                pclose(qfp);
                break;
            }
        }
        pclose(qfp);
    }
    pclose(fp);

    if (session_id[0] == '\0') {
        log_debug("context_init_from_logind: no session found for VT %u", vt);
        return;
    }

    log_debug("context_init_from_logind: VT %u -> session %s", vt, session_id);

    // Query session properties
    char props_cmd[256];
    snprintf(props_cmd, sizeof(props_cmd),
             "loginctl show-session %s 2>/dev/null", session_id);
    fp = popen(props_cmd, "r");
    if (!fp) return;

    while (fgets(line, sizeof(line), fp)) {
        char *val;

        if ((val = parse_loginctl_prop(line, "Type"))) {
            free((void *)ctx->session_type);
            ctx->session_type = val;
        } else if ((val = parse_loginctl_prop(line, "Class"))) {
            free((void *)ctx->session_class);
            ctx->session_class = val;
        } else if ((val = parse_loginctl_prop(line, "Name"))) {
            free((void *)ctx->username);
            ctx->username = val;
        } else if ((val = parse_loginctl_prop(line, "Seat"))) {
            free((void *)ctx->seat);
            ctx->seat = val;
        } else if ((val = parse_loginctl_prop(line, "Remote"))) {
            ctx->remote = (strcmp(val, "yes") == 0);
            free(val);
        } else if ((val = parse_loginctl_prop(line, "User"))) {
            // UID field
            ctx->uid = (uint32_t)atoi(val);
            free(val);
        }
    }
    pclose(fp);

    log_debug("context_init_from_logind: type=%s class=%s user=%s(%u) "
              "seat=%s remote=%d",
              ctx->session_type, ctx->session_class, ctx->username,
              ctx->uid, ctx->seat, ctx->remote);
}

// ---------------------------------------------------------------------------
// context_free
// ---------------------------------------------------------------------------

void context_free(session_context *ctx) {
    free((void *)ctx->username);
    free((void *)ctx->session_type);
    free((void *)ctx->session_class);
    free((void *)ctx->seat);
    free((void *)ctx->compositor_name);
    free((void *)ctx->terminal_type);
    free((void *)ctx->foreground_process);
    memset(ctx, 0, sizeof(*ctx));
}

// ---------------------------------------------------------------------------
// Probe implementations
// ---------------------------------------------------------------------------

static void probe_has_dbus_notifications(session_context *ctx) {
    // Introspect the Notifications interface on the target user's session bus.
    // Uses dbus_run_as_user() to handle cross-user (e.g. root daemon probing
    // a regular user's D-Bus session).
    int rc = dbus_run_as_user(
        "gdbus introspect --session "
        "--dest org.freedesktop.Notifications "
        "--object-path /org/freedesktop/Notifications "
        ">/dev/null 2>&1",
        ctx->uid);
    ctx->has_dbus_notifications = (rc == 0);
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
