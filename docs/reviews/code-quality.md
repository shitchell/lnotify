# Code Quality Review: lnotify

**Date:** 2026-03-09
**Branch:** dev (commit 02864af)
**Reviewer scope:** All files under `src/` and `include/`, plus `Makefile`

## Summary

The lnotify codebase is well-organized, cleanly written, and demonstrates solid C fundamentals. The code compiles with `-Wall -Wextra -Wpedantic` at zero warnings, all 193 tests pass, and the module boundaries are sensible. The issues below are ranked by severity to guide prioritization. None are showstoppers, but several are worth addressing for production hardening.


## Critical Issues

### C-01: `session_context` uses `const char *` for heap-allocated strings, requiring casts to free

**Files:**
- `/home/guy/code/git/github.com/shitchell/lnotify/include/engine.h` lines 29-30, 38-41
- `/home/guy/code/git/github.com/shitchell/lnotify/src/engine.c` lines 37-47, 64-72

**Description:** The `session_context` struct declares several fields as `const char *` (username, session_type, session_class, seat, compositor_name, terminal_type, foreground_process), but these are populated with `strdup()` and freed with `free((void *)...)`. The `const` qualifier gives a false impression that these are borrowed pointers, and every free site requires a cast to suppress compiler diagnostics.

**Suggested fix:** Change these fields to `char *`. The `const` correctness should be enforced at the API boundary (function parameters), not on owned struct fields.

```c
// engine.h session_context
char *username;
char *session_type;
char *session_class;
char *seat;
char *compositor_name;
char *terminal_type;
char *foreground_process;
```

### C-02: `dbus_call_as_user()` sets GID to the target UID value instead of resolving the user's actual GID

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_dbus.c` line 84

**Description:** The cross-user fork path calls `setresgid(target_uid, target_uid, target_uid)`. This assumes the user's primary GID equals their UID, which is true on many Linux systems by convention but is not guaranteed (e.g., users sharing a primary group, LDAP-provisioned accounts). If the GID does not match, the child process runs with the wrong group identity, which can cause permission issues or security surprises.

**Suggested fix:** Use `getpwuid(target_uid)` to look up the correct `pw_gid` before calling `setresgid()`:

```c
struct passwd *pw = getpwuid(target_uid);
if (!pw) _exit(127);
if (setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) < 0) _exit(127);
```

### C-03: `terminal_render_overlay()` leaks a forked child's pty file descriptor

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_terminal.c` lines 346-376

**Description:** The overlay dismiss logic forks a child process that sleeps, then writes to `pty_fd` and calls `_exit()`. However, the parent continues and eventually closes `pty_fd` in the caller (`ssh_delivery.c` line 365). The child inherits the open fd but the parent's close does not affect the child's copy. While the child does `_exit()` which closes all fds, the child holds `pty_fd` open during the entire sleep period. More critically, no SIGCHLD handler is registered and the parent does not `waitpid()` on this child, so the child becomes a zombie until `lnotifyd` exits.

**Suggested fix:** Either:
1. Set `signal(SIGCHLD, SIG_IGN)` in the daemon to auto-reap, or
2. Install a SIGCHLD handler that calls `waitpid(-1, NULL, WNOHANG)`, or
3. Double-fork so `init` adopts the grandchild.

### C-04: Protocol uses native byte order (assumes little-endian)

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/protocol.c` lines 10-55

**Description:** The `write_u16`, `write_u32`, `write_u64`, and their read counterparts use `memcpy` from/to native types, which produces host-byte-order wire data. The comments say "little-endian" but the code does not perform any byte swapping. On a big-endian host (or if the daemon and client ever run on different architectures, e.g., via a network socket in a future version), this would silently produce corrupt data.

**Suggested fix:** Since this is a local IPC protocol and both sides run on the same machine, this is acceptable for v1. However, add a comment explicitly stating the assumption, and consider adding a compile-time assertion:

```c
// This protocol uses native byte order. Both client and daemon must run
// on the same host and architecture. A future network-capable protocol
// would need explicit endian conversion.
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "lnotify protocol assumes little-endian byte order");
```


## Important Issues

### I-01: `engine_fb.c` duplicates font size clamping and spacing calculation between `fb_compute_geometry()` and `fb_draw_toast()`

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_fb.c` lines 174-203 and 207-243

**Description:** Both functions independently compute `title_px`, `body_px`, `title_h`, `body_h`, and `line_spacing` with identical logic. If one is updated and the other is not, the geometry and drawing will disagree, producing misaligned text.

**Suggested fix:** Extract the layout calculation into a small struct and helper function:

```c
typedef struct {
    int title_px, body_px;
    int title_h, body_h;
    int line_spacing;
} toast_layout;

static toast_layout compute_layout(const lnotify_config *cfg) { ... }
```

Then call it once and pass it to both functions.

### I-02: `config_set()` uses `atoi()` for integer parsing with no validation

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/config.c` lines 113-122, 134-141

**Description:** `atoi()` returns 0 for non-numeric input and has no error reporting. A user config entry like `font_size = abc` silently sets the font size to 0, which later requires defensive clamping downstream (e.g., `if (title_px < 8) title_px = 8`). Negative values are also silently accepted for fields that should be non-negative (border_width, padding, margin, font_size, default_timeout).

**Suggested fix:** Use `strtol()` with error checking, and validate ranges:

```c
static int parse_positive_int(const char *value, const char *key, int fallback) {
    char *end;
    long v = strtol(value, &end, 10);
    if (*end != '\0' || v < 0 || v > INT_MAX) {
        log_debug("config: invalid integer for %s: %s", key, value);
        return fallback;
    }
    return (int)v;
}
```

### I-03: `engine_fb.c` creates a fresh `lnotify_config` with defaults instead of using the daemon's config

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_fb.c` lines 500-502

**Description:** The `fb_render()` function calls `config_defaults(&cfg)` to create a local config rather than using the daemon's loaded configuration. This means user-configured colors, font sizes, padding, margin, position, and border settings are completely ignored by the framebuffer engine. The TODO comment on line 500 acknowledges this.

**Suggested fix:** Either pass the config through the engine vtable (add a `const lnotify_config *cfg` parameter to `render()`), or use a global config pointer set during daemon initialization. The vtable approach is cleaner and avoids global state.

### I-04: `read_proc_env()` uses a fixed 8KiB buffer that may truncate large environments

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_terminal.c` lines 20-49

**Description:** `/proc/{pid}/environ` can be much larger than 8192 bytes for processes with many or large environment variables. A single read of 8191 bytes may miss the target variable. Additionally, the function performs a single `read()` call, which is not guaranteed to return all available data even if the buffer is large enough.

**Suggested fix:** Read in a loop until EOF or a reasonable cap (e.g., 256KiB), or use `mmap` for the proc file. At minimum, document the limitation.

### I-05: Build system recompiles everything on any source change (no object file caching)

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/Makefile` lines 29-36

**Description:** The Makefile compiles all source files directly into the final binary in a single `$(CC)` invocation. This means any change to any source file recompiles the entire project. For a ~20-file codebase this is currently tolerable (~2-3 seconds), but it will scale poorly and is unusual for a production C project.

**Suggested fix:** Add `.o` intermediate rules:

```makefile
COMMON_OBJ = $(COMMON_SRC:.c=.o)
DAEMON_OBJ = src/daemon/main.o src/daemon/socket.o src/daemon/ssh_delivery.o $(COMMON_OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

build/lnotifyd: $(DAEMON_OBJ) | build
	$(CC) -o $@ $(DAEMON_OBJ) $(LDFLAGS)
```

Also add proper header dependency tracking with `-MMD -MP` and `-include $(wildcard *.d)`.

### I-06: Client links all daemon code including engine implementations it never uses

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/Makefile` line 19

**Description:** `CLIENT_SRC` includes all of `COMMON_SRC`, which pulls in the engine implementations (`engine_fb.c`, `engine_dbus.c`, `engine_terminal.c`), the font backends, `logind.c`, `queue.c`, `render_util.c`, and `resolver.c`. The client binary only needs `protocol.c`, `notification.c`, `log.c`, `config.c`, and `socket.c`. This bloats the client binary and creates unnecessary dependencies (e.g., requiring libsystemd, freetype, fontconfig at link time for the client).

**Suggested fix:** Define a separate `CLIENT_COMMON_SRC` with only the files the client actually needs:

```makefile
CLIENT_COMMON_SRC = src/log.c src/notification.c src/protocol.c src/config.c
CLIENT_SRC = src/client/main.c src/daemon/socket.c $(CLIENT_COMMON_SRC)
```

Then the client can be built without `-lsystemd` and optional font libraries.

### I-07: `MAX_ENGINES` is 32 but `resolver_select()` uses `uint32_t` for the rejected bitfield

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/resolver.c` line 16

**Description:** The rejected bitfield is `uint32_t`, which supports exactly 32 engines. If `MAX_ENGINES` is ever changed, the bitfield width must change too. This coupling is implicit and fragile.

**Suggested fix:** Add a compile-time assertion:

```c
_Static_assert(MAX_ENGINES <= 32, "rejected bitfield is uint32_t, max 32 engines");
```

### I-08: `notification_init()` does not handle `strdup()` failure for body (which is documented as required)

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/notification.c` lines 16-23

**Description:** If `strdup(body)` returns NULL due to allocation failure, `n->body` is silently set to NULL. Downstream code (e.g., `protocol_serialize`) requires body to be non-NULL and will return an error, but the caller has no way to know that `notification_init` partially failed.

**Suggested fix:** Either return an error code from `notification_init()`, or document that callers must check `n->body != NULL` after init.

### I-09: `socket_handle_client()` closes `client_fd` on some error paths but the header doc says "caller must close"

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/socket.c` lines 88-100

**Description:** On error, the function calls `close(client_fd)` and returns -1. On success, the caller is expected to close it. This asymmetry is documented in the header comment but is easy to get wrong. The daemon's event loop does `close(client_fd)` after a successful call (line 554), which is correct, but if anyone adds error handling that also closes, there will be double-close bugs.

**Suggested fix:** Either always close on error (current behavior, but clearly document it), or never close (let the caller always handle it). Consistency matters more than which choice is made.


## Minor Issues / Suggestions

### M-01: `probe_name()` in daemon `main.c` duplicates `PROBE_COUNT` enum awareness

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/main.c` lines 114-123

**Description:** This function must be updated manually whenever a new `probe_key` is added. Since it is only used for dry-run diagnostic output, it is low risk, but a `PROBE_COUNT` default case would catch the sentinel cleanly (it already has a `default` case).

**Suggested fix:** No action needed, but consider adding a compile-time check or `X-macro` pattern if the probe enum grows.

### M-02: `word_in_list()` is duplicated conceptually -- similar token-matching logic exists in `config.c`'s `parse_bool()`

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/ssh_delivery.c` lines 23-45

**Description:** `word_in_list()` is a general-purpose utility but lives in `ssh_delivery.c`. If other modules need similar functionality (e.g., config validation of position strings), they would duplicate it.

**Suggested fix:** Consider moving `word_in_list()` to a shared utility module if it gains more callers.

### M-03: `config_parse_color()` only accepts 9-character `#RRGGBBAA` format

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/config.c` line 58

**Description:** Common color formats like `#RGB`, `#RRGGBB` (no alpha), and `#RGBA` are rejected. Users familiar with CSS color notation may expect 6-digit hex colors to work.

**Suggested fix:** Consider supporting `#RRGGBB` (defaulting alpha to 0xFF). This is a low-priority UX improvement.

### M-04: `tmux display-popup` command is vulnerable to shell injection via notification content

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_terminal.c` lines 206-213

**Description:** The notification title/body is embedded into a shell command string (`echo '...'`) that tmux executes. Single quotes in the notification content would break the quoting. The code has a comment acknowledging this as a "pre-existing v1 limitation."

**Suggested fix:** Escape single quotes in the message string before embedding, or use `printf '%s'` with the message passed as an argument rather than interpolated into the command string.

### M-05: Magic number 5000 for default timeout appears in multiple locations

**Files:**
- `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_dbus.c` line 155
- `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/ssh_delivery.c` line 313
- `/home/guy/code/git/github.com/shitchell/lnotify/src/config.c` line 81

**Description:** The default timeout of 5000ms is hardcoded in three places. If the default changes, all must be found and updated.

**Suggested fix:** Define `LNOTIFY_DEFAULT_TIMEOUT_MS 5000` in `config.h` and reference it everywhere.

### M-06: `config_defaults()` strdup's an empty string for `font_path`

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/config.c` line 84

**Description:** `cfg->font_path = strdup("")` allocates a 1-byte heap string just to represent "not set." Using `NULL` would be more idiomatic and consistent with how `socket_path` (line 102) and other optional fields are handled. The `font_init()` code already checks `font_path && *font_path`, so NULL would work.

**Suggested fix:** Use `cfg->font_path = NULL` instead of `strdup("")`.

### M-07: `LNOTIFY_VERSION` is only defined via Makefile `-D` flag

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/Makefile` line 10

**Description:** If someone compiles a source file directly (e.g., during IDE integration or debugging), `LNOTIFY_VERSION` will be undefined and cause a compilation error. The daemon and client both reference it.

**Suggested fix:** Add a fallback definition in a header:

```c
#ifndef LNOTIFY_VERSION
#define LNOTIFY_VERSION "unknown"
#endif
```

### M-08: No test coverage for font, engine_fb, engine_dbus, engine_terminal, logind, or socket modules

**Files:** `/home/guy/code/git/github.com/shitchell/lnotify/tests/`

**Description:** The test suite covers notification, protocol, config, resolver, queue, and render_util. The untested modules include significant logic: font scaling, framebuffer rendering, D-Bus cross-user fork, SSH session discovery, terminal escape sequences, logind session queries, and socket lifecycle. These are understandably harder to unit test (hardware/system dependencies), but some can be tested with mocks or in-process abstractions.

**Suggested fix (prioritized):**
1. Font bitmap: test `get_char_bitmap()` and `bitmap_text_width()` -- pure functions, easy to test.
2. Protocol edge cases: test truncated buffers, oversized strings, unknown field_mask bits.
3. Socket: test `socket_default_path()` logic with mocked environment variables.

### M-09: `engine_fb.c` lacks alpha blending for toast background

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_fb.c` lines 116-128

**Description:** The default background color has alpha 0xE6 (~90% opaque), but `fb_draw_rounded_rect()` writes the BGRA values directly without blending against the existing framebuffer content. The alpha channel is stored but has no visual effect -- the toast is fully opaque regardless of the alpha value.

**Suggested fix:** Add alpha blending when `color->a < 255`:

```c
if (color->a < 255) {
    uint8_t inv = 255 - color->a;
    pixel[0] = (uint8_t)((bgra[0] * color->a + pixel[0] * inv) / 255);
    // ... same for channels 1, 2
}
```

Note: the FreeType backend already does alpha blending for glyph rendering (font_freetype.c lines 66-70), so the pattern already exists in the codebase.


## Architecture Observations (Not Issues)

These are not problems, just notes on design choices that are worth documenting for future contributors.

1. **Engine vtable pattern is clean and extensible.** Adding a new engine requires only a new `.c` file, an `extern engine` declaration, and registration in `init_engines()`. The resolver's probe-based lazy evaluation is a good design.

2. **Thread safety model is clear.** The queue uses `pthread_mutex_t`, the framebuffer engine has its own mutex, and the daemon main loop is single-threaded except for the defense thread. The boundaries are well-defined.

3. **Config ownership semantics are consistent.** All string fields are heap-allocated, `config_free()` is idempotent, and the load-over-defaults pattern works well.

4. **Protocol is simple and adequate for v1.** The length-prefixed string encoding and field_mask approach is clean. The dry-run transport flag reusing the field_mask is clever and avoids a separate message type.


## Build and Test Health

- **Compiler warnings:** Zero warnings with `-Wall -Wextra -Wpedantic` -- excellent.
- **Test suite:** 193 tests, all passing. Coverage is good for the core data-path modules.
- **Build time:** Full rebuild is fast (single-pass compilation), but incremental builds would benefit from object file caching (see I-05).
