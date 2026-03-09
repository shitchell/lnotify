# sd-bus Refactor: Eliminate Subprocess Calls

**Date:** 2026-03-09
**Status:** Design complete, pending implementation
**Motivation:** All `system()`, `popen()`, and `execl("/bin/sh")` calls are replaced with native C equivalents — `sd-bus` for D-Bus/logind, direct `fork`/`exec` for tmux.

## Empirical Findings

Tested cross-user D-Bus access in `prototype/dbus-root-test.c`:
- **Root cannot connect to a user's session bus directly** — D-Bus daemon rejects the connection at the protocol level ("Connection reset by peer"), even though root has file-level access to the socket.
- **`sd_bus_open_user()` fails as root** — no session bus exists for root.
- **fork+setresuid+`sd_bus_open_user()` works** — the existing cross-user pattern is still required.

## What Gets Replaced

### Category 1: loginctl queries → sd-bus to org.freedesktop.login1

| File | Current | Replacement |
|------|---------|-------------|
| `engine.c` `context_init_from_logind()` | `popen("loginctl list-sessions")`, `popen("loginctl show-session ... -p VTNr")`, `popen("loginctl show-session ...")` | `sd_bus_call_method()` on system bus: `ListSessions`, `Get` properties |
| `engine_dbus.c` `get_dbus_addr_from_proc()` | `popen("loginctl show-user ... -p Sessions")`, `popen("loginctl show-session ... -p Leader")` | `sd_bus_call_method()`: `ListSessions` filtered by uid, `Get` Leader property |
| `ssh_delivery.c` `ssh_find_qualifying_ptys()` | `popen("loginctl list-sessions")`, `popen("loginctl show-session ...")` | Same sd-bus calls as engine.c |

**Key insight:** logind runs on the **system bus**, which root can access directly. No fork/setresuid needed.

### Category 2: gdbus → sd-bus to org.freedesktop.Notifications

| File | Current | Replacement |
|------|---------|-------------|
| `engine.c` `probe_has_dbus_notifications()` | `dbus_run_as_user("gdbus introspect --session ...")` | `dbus_call_as_user()` with callback that calls `sd_bus_call_method()` for Introspect |
| `engine_dbus.c` `dbus_exec_with_retry()` | `dbus_run_as_user("gdbus call --session ...")` | `dbus_call_as_user()` with callback that calls `sd_bus_call_method()` for Notify |

**Cross-user pattern preserved:** fork+setresuid+`sd_bus_open_user()` in child, then native sd-bus calls (no shell).

### Category 3: tmux → fork/exec (no shell)

| File | Current | Replacement |
|------|---------|-------------|
| `engine_terminal.c` tmux display-popup | `system("tmux -S ... display-popup ...")` | `fork()`+`execlp("tmux", ...)` |
| `engine_terminal.c` tmux display-message | `system("tmux -S ... display-message ...")` | `fork()`+`execlp("tmux", ...)` |

## New Files

### `include/logind.h` / `src/logind.c`

Shared module wrapping sd-bus calls to `org.freedesktop.login1`. Replaces all loginctl subprocess calls across `engine.c`, `engine_dbus.c`, and `ssh_delivery.c`.

```c
typedef struct {
    char *session_id;      // e.g. "36625"
    char *type;            // "wayland", "x11", "tty"
    char *class;           // "user", "greeter"
    char *username;
    char *seat;            // "seat0"
    uint32_t uid;
    uint32_t leader_pid;
    uint32_t vt;
    bool remote;
} logind_session;

// Find the session on a given VT. Returns 0 on success, -1 if none.
int logind_get_session_by_vt(uint32_t vt, logind_session *out);

// List remote sessions. Returns count (up to max).
int logind_list_remote_sessions(logind_session *out, int max);

// Get leader PID for a user (for /proc environ scanning, needed until
// we can fully eliminate get_dbus_addr_from_proc).
int logind_get_user_leader(uint32_t uid, pid_t *out_pid);

// Free heap strings.
void logind_session_free(logind_session *s);
```

The system bus connection is opened on first use and cached as a file-scope static. Closed at daemon shutdown via a new `logind_close()` function.

### sd-bus call patterns

**ListSessions** returns `a(susso)` — array of structs: (session_id: string, uid: uint32, username: string, seat: string, object_path: string).

```c
sd_bus_call_method(bus,
    "org.freedesktop.login1",
    "/org/freedesktop/login1",
    "org.freedesktop.login1.Manager",
    "ListSessions",
    &error, &reply, "");
// Then iterate: sd_bus_message_enter_container(reply, 'a', "(susso)")
```

**Session properties** (Type, Class, VTNr, Remote, Leader, etc.) via the session object path returned by ListSessions:

```c
sd_bus_get_property_string(bus,
    "org.freedesktop.login1",
    "/org/freedesktop/login1/session/36625",
    "org.freedesktop.login1.Session",
    "Type", &error, &type_str);

sd_bus_get_property_trivial(bus,
    "org.freedesktop.login1",
    "/org/freedesktop/login1/session/36625",
    "org.freedesktop.login1.Session",
    "VTNr", &error, 'u', &vt_nr);
```

## Modified Files

### `engine_dbus.h` / `engine_dbus.c`

- `dbus_run_as_user(const char *cmd, uint32_t uid)` → `dbus_call_as_user(uint32_t uid, dbus_user_fn fn, void *userdata)`
- Callback-based: caller provides a function that receives an open `sd_bus *` and does its work.
- Same-user: open user bus directly, call fn.
- Cross-user: fork+setresuid, open user bus in child, call fn, exit.
- `shell_escape()` and `build_gdbus_command()` are deleted entirely.
- `get_dbus_addr_from_proc()` is deleted — `sd_bus_open_user()` after setresuid handles bus discovery.

```c
typedef int (*dbus_user_fn)(sd_bus *bus, void *userdata);

// Returns 0 on success. Cross-user uses fork+setresuid.
int dbus_call_as_user(uint32_t target_uid, dbus_user_fn fn, void *userdata);
```

**Probe callback:**
```c
static int probe_fn(sd_bus *bus, void *userdata) {
    (void)userdata;
    return sd_bus_call_method(bus,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.DBus.Introspectable",
        "Introspect", NULL, NULL, "");
}
```

**Notify callback:**
```c
typedef struct {
    const char *title;
    const char *body;
    int32_t timeout_ms;
} notify_args;

static int notify_fn(sd_bus *bus, void *userdata) {
    notify_args *args = userdata;
    return sd_bus_call_method(bus,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify", NULL, NULL,
        "susssasa{sv}i",
        "lnotify",            // app_name
        (uint32_t)0,          // replaces_id
        "",                   // app_icon
        args->title ? args->title : "",
        args->body ? args->body : "",
        0,                    // actions (empty array)
        0,                    // hints (empty dict)
        args->timeout_ms > 0 ? args->timeout_ms : 5000);
}
```

### `engine.c`

- `context_init_from_logind()` → calls `logind_get_session_by_vt()` instead of popen chains.
- `probe_has_dbus_notifications()` → calls `dbus_call_as_user(ctx->uid, probe_fn, NULL)`.

### `ssh_delivery.c`

- `ssh_find_qualifying_ptys()` → calls `logind_list_remote_sessions()` instead of popen chains.

### `engine_terminal.c`

- tmux `system()` calls → `fork()`/`execlp("tmux", ...)` helper.
- `escape_single_quotes()` deleted (no shell, no quoting needed).

### `Makefile`

- Add `src/logind.c` to `COMMON_SRC`.
- Add `$(shell pkg-config --cflags --libs libsystemd)` to `CFLAGS`/`LDFLAGS`.

## What Gets Deleted

- `shell_escape()` in `engine_dbus.c`
- `build_gdbus_command()` in `engine_dbus.c`
- `get_dbus_addr_from_proc()` in `engine_dbus.c` (sd_bus_open_user after setresuid handles this)
- `escape_single_quotes()` in `engine_terminal.c` (no shell quoting needed)
- All `popen()` calls in `engine.c`, `engine_dbus.c`, `ssh_delivery.c`
- All `system()` calls in `engine_dbus.c`, `engine_terminal.c`

## Build Dependency

`libsystemd-dev` (already installed). Link with `-lsystemd` via pkg-config.

## Testing

- Existing unit tests should pass unchanged (resolver tests use mock probes, config tests don't touch D-Bus)
- Integration test (`test_integration.sh`) will need the daemon to link against libsystemd
- Manual D-Bus test should be re-run to verify cross-user delivery still works
- New unit tests for `logind.c` functions (mock the sd-bus responses or test against live system)

## Migration Order

1. **`logind.c`** — new module, no existing code changes yet. Can be tested independently.
2. **`engine.c`** — swap `context_init_from_logind()` to use `logind.c`. Highest impact (hot path).
3. **`ssh_delivery.c`** — swap to use `logind.c`. Same pattern as step 2.
4. **`engine_dbus.c`** — replace gdbus calls with sd-bus, refactor `dbus_run_as_user` → `dbus_call_as_user`. Deletes the most code.
5. **`engine_terminal.c`** — swap tmux `system()` to `fork`/`exec`. Smallest change, independent of sd-bus.
6. **Cleanup** — remove dead code, verify zero `system()`/`popen()` calls remain.
