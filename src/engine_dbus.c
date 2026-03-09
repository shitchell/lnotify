#define _GNU_SOURCE  // for setresuid/setresgid

#include "engine_dbus.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
//  Helpers for gdbus command construction
// -------------------------------------------------------------------

// Shell-escape a string for use inside single quotes. Returns a
// heap-allocated string that the caller must free.
// Strategy: replace each ' with '\'' (end quote, escaped quote, reopen quote).
static char *shell_escape(const char *s) {
    if (!s) return strdup("''");

    // Count single quotes to determine buffer size
    size_t len = 0;
    size_t quotes = 0;
    for (const char *p = s; *p; p++) {
        len++;
        if (*p == '\'') quotes++;
    }

    // Escaped form: 'text' but each ' becomes '\'' (4 chars replacing 1)
    // Result: opening ' + body + closing '
    size_t out_len = 2 + len + quotes * 3 + 1;  // 2 for outer quotes
    char *out = malloc(out_len);
    if (!out) return NULL;

    char *wp = out;
    *wp++ = '\'';
    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            *wp++ = '\'';  // end current quote
            *wp++ = '\\';
            *wp++ = '\'';  // escaped literal quote
            *wp++ = '\'';  // reopen quote
        } else {
            *wp++ = *p;
        }
    }
    *wp++ = '\'';
    *wp = '\0';

    return out;
}

// Build the gdbus call command string. Returns a heap-allocated string.
// The caller must free it.
static char *build_gdbus_command(const char *title, const char *body,
                                  int32_t timeout_ms) {
    char *escaped_title = shell_escape(title ? title : "");
    char *escaped_body  = shell_escape(body ? body : "");
    if (!escaped_title || !escaped_body) {
        free(escaped_title);
        free(escaped_body);
        return NULL;
    }

    // gdbus call --session --dest org.freedesktop.Notifications
    //   --object-path /org/freedesktop/Notifications
    //   --method org.freedesktop.Notifications.Notify
    //   "lnotify" 0 "" "TITLE" "BODY" [] {} TIMEOUT
    //
    // String args to gdbus are unquoted (gdbus parses them as GVariant text).
    // We shell-escape and let the shell pass them as argv.
    size_t cmd_len = 512 + strlen(escaped_title) + strlen(escaped_body);
    char *cmd = malloc(cmd_len);
    if (!cmd) {
        free(escaped_title);
        free(escaped_body);
        return NULL;
    }

    int timeout = timeout_ms > 0 ? timeout_ms : 5000;

    snprintf(cmd, cmd_len,
             "gdbus call --session "
             "--dest org.freedesktop.Notifications "
             "--object-path /org/freedesktop/Notifications "
             "--method org.freedesktop.Notifications.Notify "
             "'lnotify' 0 '' %s %s '[]' '{}' %d "
             ">/dev/null 2>&1",
             escaped_title, escaped_body, timeout);

    free(escaped_title);
    free(escaped_body);
    return cmd;
}

// -------------------------------------------------------------------
//  D-Bus session bus address discovery for cross-user delivery
// -------------------------------------------------------------------

// Try to read DBUS_SESSION_BUS_ADDRESS from /proc/{pid}/environ.
// Returns a heap-allocated string or NULL.
static char *get_dbus_addr_from_proc(uint32_t uid) {
    // Find a process owned by this uid that has the env var.
    // Strategy: scan /proc for processes owned by uid, check environ.
    // We use loginctl to find the session leader pid instead.
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "loginctl show-user %u -p Sessions --value 2>/dev/null", uid);

    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    char sessions[512] = {0};
    if (!fgets(sessions, sizeof(sessions), fp)) {
        pclose(fp);
        return NULL;
    }
    pclose(fp);

    // sessions is a space-separated list of session IDs
    // For each, get the Leader PID and check its environ
    char *saveptr = NULL;
    char *sid = strtok_r(sessions, " \t\n", &saveptr);
    while (sid) {
        char pcmd[256];
        snprintf(pcmd, sizeof(pcmd),
                 "loginctl show-session %s -p Leader --value 2>/dev/null", sid);
        fp = popen(pcmd, "r");
        if (!fp) { sid = strtok_r(NULL, " \t\n", &saveptr); continue; }

        char pid_str[32] = {0};
        if (!fgets(pid_str, sizeof(pid_str), fp)) {
            pclose(fp);
            sid = strtok_r(NULL, " \t\n", &saveptr);
            continue;
        }
        pclose(fp);

        int leader_pid = atoi(pid_str);
        if (leader_pid <= 0) {
            sid = strtok_r(NULL, " \t\n", &saveptr);
            continue;
        }

        // Read /proc/{pid}/environ (NUL-separated key=value pairs)
        char env_path[64];
        snprintf(env_path, sizeof(env_path), "/proc/%d/environ", leader_pid);

        int fd = open(env_path, O_RDONLY);
        if (fd < 0) {
            sid = strtok_r(NULL, " \t\n", &saveptr);
            continue;
        }

        // Read in chunks, search for DBUS_SESSION_BUS_ADDRESS=
        char buf[8192];
        ssize_t nread = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (nread <= 0) {
            sid = strtok_r(NULL, " \t\n", &saveptr);
            continue;
        }
        buf[nread] = '\0';

        // Scan NUL-separated entries
        const char *needle = "DBUS_SESSION_BUS_ADDRESS=";
        size_t needle_len = strlen(needle);
        const char *p = buf;
        const char *end = buf + nread;
        while (p < end) {
            size_t entry_len = strlen(p);
            if (entry_len > needle_len &&
                strncmp(p, needle, needle_len) == 0) {
                char *addr = strdup(p + needle_len);
                return addr;
            }
            p += entry_len + 1;  // skip past NUL
        }

        sid = strtok_r(NULL, " \t\n", &saveptr);
    }

    // Fallback: standard XDG path
    char fallback[128];
    snprintf(fallback, sizeof(fallback), "unix:path=/run/user/%u/bus", uid);
    return strdup(fallback);
}

// -------------------------------------------------------------------
//  Execute gdbus with retry (same-user case)
// -------------------------------------------------------------------

static bool dbus_exec_same_user(const char *gdbus_cmd) {
    int delay_ms = DBUS_RETRY_BASE_MS;

    for (int attempt = 1; attempt <= DBUS_RETRY_MAX_ATTEMPTS; attempt++) {
        int rc = system(gdbus_cmd);
        if (rc == 0) {
            log_debug("engine_dbus: gdbus succeeded on attempt %d", attempt);
            return true;
        }

        if (attempt < DBUS_RETRY_MAX_ATTEMPTS) {
            log_debug("engine_dbus: gdbus failed (attempt %d/%d), "
                      "retrying in %dms",
                      attempt, DBUS_RETRY_MAX_ATTEMPTS, delay_ms);
            usleep((useconds_t)delay_ms * 1000);
            delay_ms = (int)(delay_ms * DBUS_RETRY_MULTIPLIER);
        }
    }

    log_error("engine_dbus: gdbus failed after %d attempts",
              DBUS_RETRY_MAX_ATTEMPTS);
    return false;
}

// -------------------------------------------------------------------
//  Execute gdbus with fork+setresuid (cross-user case)
// -------------------------------------------------------------------

static bool dbus_exec_cross_user(const char *gdbus_cmd,
                                  uint32_t target_uid,
                                  const char *dbus_addr) {
    int delay_ms = DBUS_RETRY_BASE_MS;

    for (int attempt = 1; attempt <= DBUS_RETRY_MAX_ATTEMPTS; attempt++) {
        pid_t pid = fork();

        if (pid < 0) {
            log_error("engine_dbus: fork failed: %s", strerror(errno));
            return false;
        }

        if (pid == 0) {
            // Child process: become the target user
            gid_t target_gid = (gid_t)target_uid;  // Primary GID usually matches UID

            // Look up the actual GID from /etc/passwd would be better,
            // but for v1, getpwuid is fine
            // Note: we set GID before UID (required order)
            if (setresgid(target_gid, target_gid, target_gid) < 0) {
                _exit(127);
            }
            if (setresuid((uid_t)target_uid, (uid_t)target_uid,
                          (uid_t)target_uid) < 0) {
                _exit(127);
            }

            // Set the D-Bus session bus address
            if (dbus_addr) {
                setenv("DBUS_SESSION_BUS_ADDRESS", dbus_addr, 1);
            }

            // Set XDG_RUNTIME_DIR for gdbus (needed for session bus access)
            char xdg_dir[64];
            snprintf(xdg_dir, sizeof(xdg_dir), "/run/user/%u", target_uid);
            setenv("XDG_RUNTIME_DIR", xdg_dir, 1);

            // Execute the gdbus command via shell
            execl("/bin/sh", "sh", "-c", gdbus_cmd, (char *)NULL);
            _exit(127);
        }

        // Parent: wait for child
        int status;
        if (waitpid(pid, &status, 0) < 0) {
            log_error("engine_dbus: waitpid failed: %s", strerror(errno));
            return false;
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            log_debug("engine_dbus: cross-user gdbus succeeded on attempt %d "
                      "(target uid=%u)", attempt, target_uid);
            return true;
        }

        if (attempt < DBUS_RETRY_MAX_ATTEMPTS) {
            log_debug("engine_dbus: cross-user gdbus failed (attempt %d/%d), "
                      "retrying in %dms",
                      attempt, DBUS_RETRY_MAX_ATTEMPTS, delay_ms);
            usleep((useconds_t)delay_ms * 1000);
            delay_ms = (int)(delay_ms * DBUS_RETRY_MULTIPLIER);
        }
    }

    log_error("engine_dbus: cross-user gdbus failed after %d attempts "
              "(target uid=%u)", DBUS_RETRY_MAX_ATTEMPTS, target_uid);
    return false;
}

// -------------------------------------------------------------------
//  render()
// -------------------------------------------------------------------

static bool dbus_render(const notification *notif,
                         const session_context *ctx) {
    char *gdbus_cmd = build_gdbus_command(notif->title, notif->body,
                                           notif->timeout_ms);
    if (!gdbus_cmd) {
        log_error("engine_dbus: failed to build gdbus command");
        return false;
    }

    bool success;
    uid_t my_uid = getuid();

    if ((uint32_t)my_uid == ctx->uid) {
        // Same user — just execute gdbus directly
        log_debug("engine_dbus: same-user delivery (uid=%u)", ctx->uid);
        success = dbus_exec_same_user(gdbus_cmd);
    } else {
        // Cross-user delivery — fork + setresuid
        log_info("engine_dbus: cross-user delivery "
                 "(daemon uid=%u, target uid=%u)",
                 (uint32_t)my_uid, ctx->uid);

        char *dbus_addr = get_dbus_addr_from_proc(ctx->uid);
        log_debug("engine_dbus: target DBUS_SESSION_BUS_ADDRESS=%s",
                  dbus_addr ? dbus_addr : "(null)");

        success = dbus_exec_cross_user(gdbus_cmd, ctx->uid, dbus_addr);
        free(dbus_addr);
    }

    free(gdbus_cmd);

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
