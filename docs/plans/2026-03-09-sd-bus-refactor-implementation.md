# sd-bus Refactor Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace all `system()`/`popen()`/`execl("/bin/sh")` calls with native `sd-bus` for D-Bus/logind and direct `fork`/`exec` for tmux.

**Architecture:** New `logind.c` module wraps sd-bus calls to `org.freedesktop.login1` (system bus). D-Bus notification probe and render use sd-bus directly (with fork+setresuid for cross-user). tmux commands use `fork`/`execlp` (no shell).

**Tech Stack:** `libsystemd` (sd-bus), pkg-config

**Design doc:** `docs/plans/2026-03-09-sd-bus-refactor.md`

---

### Task 1: Add libsystemd to the build system

**Files:**
- Modify: `Makefile`

**Step 1: Update Makefile to use pkg-config for libsystemd**

```makefile
CC = cc
VERSION := $(shell git describe --tags --always 2>/dev/null || echo "unknown")
SDBUS_CFLAGS := $(shell pkg-config --cflags libsystemd 2>/dev/null)
SDBUS_LIBS := $(shell pkg-config --libs libsystemd 2>/dev/null)
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Iinclude -Itests -g -DLNOTIFY_VERSION=\"$(VERSION)\" $(SDBUS_CFLAGS)
LDFLAGS = -lpthread $(SDBUS_LIBS)
```

**Step 2: Verify build still works**

Run: `make clean && make 2>&1`
Expected: Clean build, zero warnings.

Run: `make test 2>&1`
Expected: 192 passed, 0 failed.

**Step 3: Commit**

```
feat: add libsystemd (sd-bus) to build system
```

---

### Task 2: Create logind.h / logind.c — session-by-VT lookup

This task builds the new logind module incrementally. Start with the API needed by `context_init_from_logind()` in `engine.c`.

**Files:**
- Create: `include/logind.h`
- Create: `src/logind.c`
- Modify: `Makefile` (add `src/logind.c` to `COMMON_SRC`)

**Step 1: Create `include/logind.h`**

```c
#ifndef LNOTIFY_LOGIND_H
#define LNOTIFY_LOGIND_H

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

// Session info from logind (heap-allocated strings, caller must free via logind_session_free)
typedef struct {
    char    *session_id;    // e.g. "36625"
    char    *type;          // "wayland", "x11", "tty"
    char    *class;         // "user", "greeter"
    char    *username;
    char    *seat;          // "seat0"
    uint32_t uid;
    uint32_t leader_pid;
    uint32_t vt;
    bool     remote;
} logind_session;

// Find the session on a given VT. Returns 0 on success, -1 if none found.
// Populates *out with heap-allocated strings.
int logind_get_session_by_vt(uint32_t vt, logind_session *out);

// List remote sessions. Fills out[] up to max entries.
// Returns count of sessions found (0 if none). Each entry has heap-allocated strings.
int logind_list_remote_sessions(logind_session *out, int max);

// Free heap-allocated strings in a logind_session. Safe to call multiple times.
void logind_session_free(logind_session *s);

// Close the cached system bus connection. Call at daemon shutdown.
void logind_close(void);

#endif
```

**Step 2: Create `src/logind.c` with `logind_get_session_by_vt()`**

The implementation should:
1. Open a system bus connection (cached as file-scope static, opened on first use)
2. Call `org.freedesktop.login1.Manager.ListSessions` — returns `a(susso)` (session_id, uid, username, seat, object_path)
3. For each session, query `VTNr` property via `sd_bus_get_property_trivial()` on the session object path
4. When VT matches, query remaining properties: `Type`, `Class`, `Name`, `Seat`, `Remote`, `Leader`, `User`
5. Use `sd_bus_get_property_string()` for string properties, `sd_bus_get_property_trivial()` for uint32/bool

Key sd-bus patterns:

```c
#include <systemd/sd-bus.h>

// Cached system bus connection
static sd_bus *g_system_bus = NULL;

static sd_bus *get_system_bus(void) {
    if (!g_system_bus) {
        int r = sd_bus_open_system(&g_system_bus);
        if (r < 0) {
            log_error("logind: failed to open system bus: %s", strerror(-r));
            return NULL;
        }
    }
    return g_system_bus;
}

void logind_close(void) {
    if (g_system_bus) {
        sd_bus_unref(g_system_bus);
        g_system_bus = NULL;
    }
}
```

ListSessions returns `a(susso)`. Iterate like:

```c
sd_bus_message_enter_container(reply, 'a', "(susso)");
while (sd_bus_message_enter_container(reply, 'r', "susso") > 0) {
    const char *sid, *user, *seat, *obj;
    uint32_t uid;
    sd_bus_message_read(reply, "susso", &sid, &uid, &user, &seat, &obj);
    // ... query VTNr on obj ...
    sd_bus_message_exit_container(reply);
}
sd_bus_message_exit_container(reply);
```

Property queries on a session object path (e.g. `/org/freedesktop/login1/session/_336625`):

```c
// String property
char *type = NULL;
sd_bus_get_property_string(bus,
    "org.freedesktop.login1", obj_path,
    "org.freedesktop.login1.Session", "Type",
    &error, &type);
// Caller must free(type)

// uint32 property
uint32_t vt_nr = 0;
sd_bus_get_property_trivial(bus,
    "org.freedesktop.login1", obj_path,
    "org.freedesktop.login1.Session", "VTNr",
    &error, 'u', &vt_nr);

// bool property
int remote = 0;
sd_bus_get_property_trivial(bus,
    "org.freedesktop.login1", obj_path,
    "org.freedesktop.login1.Session", "Remote",
    &error, 'b', &remote);

// Leader is a uint32
uint32_t leader = 0;
sd_bus_get_property_trivial(bus,
    "org.freedesktop.login1", obj_path,
    "org.freedesktop.login1.Session", "Leader",
    &error, 'u', &leader);
```

Also implement `logind_session_free()`:

```c
void logind_session_free(logind_session *s) {
    free(s->session_id);  s->session_id = NULL;
    free(s->type);        s->type = NULL;
    free(s->class);       s->class = NULL;
    free(s->username);    s->username = NULL;
    free(s->seat);        s->seat = NULL;
}
```

**Step 3: Add `src/logind.c` to Makefile COMMON_SRC**

Add after `src/resolver.c`:

```makefile
COMMON_SRC = src/log.c src/notification.c src/protocol.c src/config.c src/engine.c src/resolver.c src/logind.c src/queue.c ...
```

**Step 4: Verify build**

Run: `make clean && make 2>&1`
Expected: Clean build, zero warnings. The new module compiles but isn't called yet.

Run: `make test 2>&1`
Expected: 192 passed, 0 failed.

**Step 5: Commit**

```
feat: logind module with sd-bus session-by-VT lookup
```

---

### Task 3: Add `logind_list_remote_sessions()` to logind.c

**Files:**
- Modify: `src/logind.c`

**Step 1: Implement `logind_list_remote_sessions()`**

Same ListSessions call as Task 2, but instead of matching by VT, filter by `Remote == true`. For each remote session, query all properties (Type, Class, Name, Seat, Remote, Leader, TTY, User, VTNr) and populate a `logind_session`.

Note: `ssh_delivery.c` also needs the TTY property. Add a `char *tty` field to `logind_session` in `logind.h`:

```c
typedef struct {
    char    *session_id;
    char    *type;
    char    *class;
    char    *username;
    char    *seat;
    char    *tty;           // e.g. "pts/3" — needed for SSH delivery
    uint32_t uid;
    uint32_t leader_pid;
    uint32_t vt;
    bool     remote;
} logind_session;
```

Update `logind_session_free()` to also `free(s->tty)`.

**Step 2: Verify build and tests**

Run: `make clean && make 2>&1`
Expected: Clean build.

Run: `make test 2>&1`
Expected: 192 passed.

**Step 3: Commit**

```
feat: logind_list_remote_sessions for SSH session discovery
```

---

### Task 4: Swap `engine.c` to use logind module

**Files:**
- Modify: `src/engine.c`

**Step 1: Replace `context_init_from_logind()`**

Replace the entire function body. Current implementation does:
1. `popen("loginctl list-sessions")` — parse session IDs
2. For each session, `popen("loginctl show-session %s -p VTNr --value")` — find matching VT
3. `popen("loginctl show-session %s")` — parse all properties

New implementation:
1. Call `logind_get_session_by_vt(vt, &session)`
2. Copy fields into the `session_context`

```c
#include "logind.h"

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

    logind_session session;
    if (logind_get_session_by_vt(vt, &session) < 0) {
        log_debug("context_init_from_logind: no session found for VT %u", vt);
        return;
    }

    log_debug("context_init_from_logind: VT %u -> session %s", vt, session.session_id);

    // Transfer fields (replace defaults)
    free((void *)ctx->session_type);
    ctx->session_type = strdup(session.type ? session.type : "");

    free((void *)ctx->session_class);
    ctx->session_class = strdup(session.class ? session.class : "");

    free((void *)ctx->username);
    ctx->username = strdup(session.username ? session.username : "");

    free((void *)ctx->seat);
    ctx->seat = strdup(session.seat ? session.seat : "");

    ctx->uid = session.uid;
    ctx->remote = session.remote;

    log_debug("context_init_from_logind: type=%s class=%s user=%s(%u) "
              "seat=%s remote=%d",
              ctx->session_type, ctx->session_class, ctx->username,
              ctx->uid, ctx->seat, ctx->remote);

    logind_session_free(&session);
}
```

Remove the now-unused helpers: `safe_strdup()` and `parse_loginctl_prop()` (only if they are not used elsewhere — verify with grep first).

Remove `#include <stdio.h>` if no longer needed (was used for popen/fgets).

**Step 2: Verify build and tests**

Run: `make clean && make 2>&1`
Expected: Clean build.

Run: `make test 2>&1`
Expected: 192 passed. (Resolver tests use mock probes, so they don't hit logind.)

**Step 3: Manual test**

Run: `sudo bash -c './build/lnotifyd --debug'`
Expected: Should log `context_init_from_logind: VT N -> session ...` with the same session info as before, but now via sd-bus instead of loginctl subprocesses.

**Step 4: Commit**

```
refactor: context_init_from_logind uses sd-bus via logind module
```

---

### Task 5: Swap `ssh_delivery.c` to use logind module

**Files:**
- Modify: `src/daemon/ssh_delivery.c`

**Step 1: Replace `ssh_find_qualifying_ptys()`**

Replace the popen-based session discovery with `logind_list_remote_sessions()`. The function currently:
1. `popen("loginctl list-sessions")` — parse session IDs
2. For each, `popen("loginctl show-session %s -p Remote -p Name ...")` — get properties
3. Filter by remote=yes, user qualification, LNOTIFY_SSH env, pty writability

New implementation:
1. Call `logind_list_remote_sessions(sessions, MAX)` — returns already-filtered remote sessions
2. For each session, apply the same user/env/pty filters

```c
#include "logind.h"

int ssh_find_qualifying_ptys(const lnotify_config *cfg,
                             ssh_pty_info *ptys, int max_ptys) {
    logind_session sessions[64];
    int session_count = logind_list_remote_sessions(sessions, 64);

    int count = 0;

    for (int i = 0; i < session_count && count < max_ptys; i++) {
        logind_session *s = &sessions[i];

        // Must have a TTY
        if (!s->tty || !s->tty[0]) {
            logind_session_free(s);
            continue;
        }

        pid_t leader = (pid_t)s->leader_pid;
        if (leader <= 0) {
            logind_session_free(s);
            continue;
        }

        // Check user qualification
        if (!user_qualifies(s->username, (uid_t)s->uid, cfg)) {
            log_debug("ssh: user %s does not qualify", s->username);
            logind_session_free(s);
            continue;
        }

        // Check LNOTIFY_SSH env var for opt-out
        char *client_modes = get_lnotify_ssh_modes(leader);
        if (client_modes && strcmp(client_modes, "none") == 0) {
            log_debug("ssh: session %s opted out (LNOTIFY_SSH=none)",
                      s->session_id);
            free(client_modes);
            logind_session_free(s);
            continue;
        }
        free(client_modes);

        // Build pty path
        char pty_path[64];
        if (strncmp(s->tty, "/dev/", 5) == 0) {
            snprintf(pty_path, sizeof(pty_path), "%s", s->tty);
        } else {
            snprintf(pty_path, sizeof(pty_path), "/dev/%s", s->tty);
        }

        // Verify pty exists and is writable
        if (access(pty_path, W_OK) != 0) {
            log_debug("ssh: %s not writable, skipping", pty_path);
            logind_session_free(s);
            continue;
        }

        // Add to results
        ssh_pty_info *info = &ptys[count];
        snprintf(info->pty_path, sizeof(info->pty_path), "%s", pty_path);
        info->session_leader = leader;
        info->uid = (uid_t)s->uid;
        snprintf(info->username, sizeof(info->username), "%s", s->username);

        log_debug("ssh: found qualifying pty %s (user=%s, leader=%d)",
                  pty_path, s->username, (int)leader);
        count++;

        logind_session_free(s);
    }

    // Free any remaining sessions we didn't consume
    for (int i = count; i < session_count; i++) {
        // Already freed in the loop above for skipped entries,
        // but some may remain if count == max_ptys
    }

    return count;
}
```

Note: be careful with the free logic — each `logind_session` returned by `logind_list_remote_sessions` needs to be freed. Consider freeing inside the loop for each entry regardless of whether it was used. The code above already does this.

Actually, the loop logic needs a fix — we're freeing in each branch, but if `count == max_ptys` we break out of the loop early. Fix: free remaining sessions after the loop:

```c
    // Free remaining unfree'd sessions (if we hit max_ptys early)
    // Actually, all sessions in the loop are freed (either in continue branches
    // or after adding to results). But if we break early due to max_ptys,
    // remaining sessions need freeing.
```

Simplest approach: iterate all sessions, do the filtering, and free each one at the end of each loop iteration regardless.

Remove the now-unused `#include <ctype.h>` if `isspace()` is no longer needed (it was used for parsing loginctl output).

**Step 2: Verify build and tests**

Run: `make clean && make 2>&1`
Expected: Clean build.

Run: `make test 2>&1`
Expected: 192 passed.

**Step 3: Commit**

```
refactor: ssh_find_qualifying_ptys uses sd-bus via logind module
```

---

### Task 6: Refactor D-Bus engine — `dbus_call_as_user()` with sd-bus

**Files:**
- Modify: `include/engine_dbus.h`
- Modify: `src/engine_dbus.c`

**Step 1: Replace public API in `engine_dbus.h`**

```c
#ifndef LNOTIFY_ENGINE_DBUS_H
#define LNOTIFY_ENGINE_DBUS_H

#include "engine.h"

#include <stdint.h>
#include <systemd/sd-bus.h>

extern engine engine_dbus;

// Callback type for D-Bus operations on a user's session bus.
typedef int (*dbus_user_fn)(sd_bus *bus, void *userdata);

// Run a D-Bus operation on a user's session bus.
// Same-user: opens user bus directly and calls fn.
// Cross-user: fork+setresuid, then opens user bus and calls fn in child.
// Returns 0 on success, non-zero on failure.
int dbus_call_as_user(uint32_t target_uid, dbus_user_fn fn, void *userdata);

#endif
```

**Step 2: Implement `dbus_call_as_user()` in `engine_dbus.c`**

Same-user path:
```c
sd_bus *bus = NULL;
int r = sd_bus_open_user(&bus);
if (r < 0) return r;
r = fn(bus, userdata);
sd_bus_unref(bus);
return r;
```

Cross-user path (fork+setresuid, same pattern as current `dbus_run_as_user` but with sd-bus instead of shell):
```c
pid_t pid = fork();
if (pid == 0) {
    // Child: become target user
    char xdg[64];
    snprintf(xdg, sizeof(xdg), "/run/user/%u", target_uid);
    setenv("XDG_RUNTIME_DIR", xdg, 1);

    if (setresgid((gid_t)target_uid, ...) < 0) _exit(127);
    if (setresuid(target_uid, ...) < 0) _exit(127);

    sd_bus *bus = NULL;
    if (sd_bus_open_user(&bus) < 0) _exit(1);

    int rc = fn(bus, userdata);
    sd_bus_unref(bus);
    _exit(rc < 0 ? 1 : 0);
}
// Parent: waitpid
```

**Step 3: Replace D-Bus probe**

Create a probe callback:
```c
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
```

Update `probe_has_dbus_notifications()` in `engine.c`:
```c
static void probe_has_dbus_notifications(session_context *ctx) {
    int rc = dbus_call_as_user(ctx->uid, introspect_notifications, NULL);
    ctx->has_dbus_notifications = (rc >= 0);
    log_debug("probe: has_dbus_notifications = %d", ctx->has_dbus_notifications);
}
```

**Step 4: Replace D-Bus render**

Create a notify callback:
```c
typedef struct {
    const char *title;
    const char *body;
    int32_t timeout_ms;
} dbus_notify_args;

static int send_notification(sd_bus *bus, void *userdata) {
    dbus_notify_args *args = userdata;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;

    // Notify signature: "susssasa{sv}i"
    // app_name, replaces_id, app_icon, summary, body, actions, hints, timeout
    //
    // For empty arrays (actions and hints), use the low-level API to
    // open and close empty containers.
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

    r = sd_bus_call(bus, m, -1, &error, &reply);
    sd_bus_message_unref(m);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return r;
}
```

Update `dbus_render()` to use `dbus_call_as_user()` + retry:
```c
static bool dbus_render(const notification *notif, const session_context *ctx) {
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
```

Update `dbus_exec_with_retry()` to accept callback+userdata instead of a command string:
```c
static bool dbus_exec_with_retry(uint32_t target_uid, dbus_user_fn fn, void *userdata) {
    int delay_ms = DBUS_RETRY_BASE_MS;
    for (int attempt = 1; attempt <= DBUS_RETRY_MAX_ATTEMPTS; attempt++) {
        int rc = dbus_call_as_user(target_uid, fn, userdata);
        if (rc >= 0) {
            log_debug("engine_dbus: succeeded on attempt %d (uid=%u)",
                      attempt, target_uid);
            return true;
        }
        if (attempt < DBUS_RETRY_MAX_ATTEMPTS) {
            log_debug("engine_dbus: failed (attempt %d/%d, uid=%u), retrying in %dms",
                      attempt, DBUS_RETRY_MAX_ATTEMPTS, target_uid, delay_ms);
            usleep((useconds_t)delay_ms * 1000);
            delay_ms = (int)(delay_ms * DBUS_RETRY_MULTIPLIER);
        }
    }
    log_error("engine_dbus: failed after %d attempts (uid=%u)",
              DBUS_RETRY_MAX_ATTEMPTS, target_uid);
    return false;
}
```

**Step 5: Delete dead code**

Remove from `engine_dbus.c`:
- `shell_escape()` — no longer needed
- `build_gdbus_command()` — no longer needed
- `get_dbus_addr_from_proc()` — sd_bus_open_user after setresuid handles bus discovery
- `dbus_run_as_user()` — replaced by `dbus_call_as_user()`

**Step 6: Verify build and tests**

Run: `make clean && make 2>&1`
Expected: Clean build, zero warnings.

Run: `make test 2>&1`
Expected: 192 passed.

**Step 7: Manual test**

Run daemon as root, send notification as user:
```
sudo bash -c './build/lnotifyd --debug'
./build/lnotify -t "Test" "sd-bus refactor works"
```

Expected: D-Bus notification appears. Daemon log shows `engine_dbus: succeeded on attempt 1 (uid=1000)`.

**Step 8: Commit**

```
refactor: D-Bus engine uses sd-bus directly, eliminates gdbus/shell calls
```

---

### Task 7: Replace tmux `system()` with `fork`/`execlp`

**Files:**
- Modify: `src/engine_terminal.c`

**Step 1: Create a `run_tmux()` helper**

Replace `system()` with a helper that does `fork`/`execlp`:

```c
// Run a tmux command without going through a shell.
// argv is a NULL-terminated array of arguments (argv[0] should be "tmux").
// Returns 0 on success, -1 on failure.
static int run_tmux(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        log_error("ssh: fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        // Suppress stderr (replaces 2>/dev/null from the system() calls)
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execvp("tmux", argv);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}
```

**Step 2: Replace display-popup call**

Replace the `system()` call at line ~211 with:

```c
    // Build the popup command string (tmux runs this via shell internally)
    char popup_cmd[1024];
    snprintf(popup_cmd, sizeof(popup_cmd), "echo '%s'; sleep %d",
             msg, duration_secs);
    // Note: msg goes through tmux's internal shell, but we control the content
    // (it's from the notification title/body). Single quotes in msg would break
    // the echo, but this is no worse than before and a known v1 limitation.

    char width_str[8], height_str[8];
    snprintf(width_str, sizeof(width_str), "%d", 50);
    snprintf(height_str, sizeof(height_str), "%d", 6);

    char *popup_argv[] = {
        "tmux", "-S", tmux_socket,
        "display-popup", "-w", width_str, "-h", height_str, "-E",
        popup_cmd, NULL
    };
    int rc = run_tmux(popup_argv);
```

**Step 3: Replace display-message call**

Replace the `system()` call at line ~232 with:

```c
    char duration_str[16];
    snprintf(duration_str, sizeof(duration_str), "%d", duration_secs * 1000);

    char *msg_argv[] = {
        "tmux", "-S", tmux_socket,
        "display-message", "-d", duration_str, msg, NULL
    };
    int rc = run_tmux(msg_argv);
```

**Step 4: Delete `escape_single_quotes()`**

The function at lines 144-157 is no longer needed — arguments are passed directly to tmux via argv, not through a shell.

Remove both calls to `escape_single_quotes()` (lines ~203 and ~225) and the `escaped` buffers.

Note: the display-popup `-E` argument (`echo 'msg'; sleep N`) is still interpreted by tmux's internal shell. Single quotes in the notification message could break it. This is a pre-existing v1 limitation — the old code had the same issue despite escaping (it only escaped for the outer shell, not tmux's inner shell). Consider this a known limitation to address in a future task.

**Step 5: Verify build and tests**

Run: `make clean && make 2>&1`
Expected: Clean build.

Run: `make test 2>&1`
Expected: 192 passed.

**Step 6: Commit**

```
refactor: tmux commands use fork/exec instead of system()
```

---

### Task 8: Cleanup and verification

**Files:**
- Modify: various (as needed)

**Step 1: Verify zero subprocess calls remain**

Run: `grep -rn 'system(\|popen(\|execl("/bin/sh"' src/`
Expected: No matches.

Note: `execvp` and `execlp` in the tmux helper and the D-Bus cross-user fork are expected — these don't go through a shell. `execl("/bin/sh"` specifically should have zero matches.

**Step 2: Remove dead includes**

Check each modified file for now-unused includes:
- `engine.c`: remove `#include <stdio.h>` if `popen`/`fgets` no longer used
- `engine_dbus.c`: remove `#include <fcntl.h>` (was for `get_dbus_addr_from_proc`), remove `#include <stdio.h>` if unused
- `ssh_delivery.c`: remove `#include <ctype.h>` if `isspace` no longer used

Use: `make clean && make 2>&1` after each removal to verify.

**Step 3: Add `logind_close()` to daemon shutdown**

In `src/daemon/main.c`, add `logind_close()` call in the cleanup section (near line ~545, after `config_free()`):

```c
// Close logind sd-bus connection
logind_close();
```

Add `#include "logind.h"` at the top of `daemon/main.c`.

**Step 4: Full test pass**

Run: `make clean && make 2>&1`
Expected: Clean build, zero warnings.

Run: `make test 2>&1`
Expected: 192 passed, 0 failed.

**Step 5: Full manual test**

Test as root daemon:
```
sudo bash -c './build/lnotifyd --debug'
./build/lnotify -t "Final Test" "All subprocess calls eliminated"
```
Expected: D-Bus notification appears. Daemon log shows sd-bus paths, no loginctl/gdbus subprocesses.

Test dry-run:
```
./build/lnotify --dry-run "test"
```
Expected: Diagnostic output with engine selection, probes, etc.

**Step 6: Commit**

```
refactor: cleanup dead code and verify zero subprocess calls
```

---

### Task 9: Update documentation

**Files:**
- Modify: `docs/ARCHITECTURE.md`
- Modify: `docs/DECISIONS.md`

**Step 1: Update ARCHITECTURE.md**

- Add `libsystemd` to tech stack/dependencies
- Update Key Files table: add `logind.h`/`logind.c`, update descriptions for `engine.c`, `engine_dbus.c`, `ssh_delivery.c`, `engine_terminal.c`
- Add a "Dependencies" section noting `libsystemd-dev` is required
- Update the D-Bus engine description to mention sd-bus instead of gdbus

**Step 2: Update DECISIONS.md**

Add decision D33 for the sd-bus refactor with rationale captured from this session:

- User's rationale: "since C is a low-level language below the shell, it feels weird for it to be reaching up into shell territory to do stuff. that *feels* wrong; anything the shell can do, C should be able to do. you build shells from C, not the other way around."
- Decision: Replace all subprocess calls with native sd-bus + fork/exec
- Empirical finding: root cannot access user's D-Bus session bus directly (fork+setresuid still required)

**Step 3: Commit**

```
docs: update architecture and decisions for sd-bus refactor
```
