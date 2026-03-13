# Codebase Hardening: Review Fixes Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Fix all critical and important issues from the 4 code review reports in `docs/reviews/`.

**Architecture:** Fixes are grouped by theme and ordered so each task is independently committable. Security-critical fixes come first. Each task targets a small set of related files. All fixes must maintain zero warnings and 193+ passing tests.

**Tech Stack:** C11, pthreads, libsystemd, FreeType2, fontconfig. Build: make. Tests: custom test_util.h framework.

**Review reports:** `docs/reviews/security.md`, `docs/reviews/memory-safety.md`, `docs/reviews/thread-safety.md`, `docs/reviews/code-quality.md`

---

## Phase 1: Security-Critical Fixes

### Task 1: Fix tmux command injection (Security C01)

**Files:**
- Modify: `src/engine_terminal.c` (lines ~204-225)

**What:** The `terminal_render_tmux()` function interpolates notification body into a shell command passed to `tmux display-popup -E`. Any user can inject arbitrary commands executed as root.

**Fix:** Replace `display-popup -E "echo '...'"` with `display-message` which does not invoke a shell. If `display-popup` is needed for visual reasons, write the message to a temp file and have the popup `cat` it — but `display-message` is safest for v1. Alternatively, escape all single quotes in the message by replacing `'` with `'\''` before interpolation.

**Test:** Add a test that notification bodies containing shell metacharacters (`'; rm -rf / #`, `` `whoami` ``, `$(id)`) are safely handled. Verify manually that the notification still renders.

**Commit:** `fix(security): prevent command injection via tmux display-popup`

---

### Task 2: Fix terminal escape sequence injection (Security I09)

**Files:**
- Create: `src/sanitize.c`, `include/sanitize.h`
- Modify: `src/engine_terminal.c` (all pty write paths: overlay, OSC, text modes)
- Modify: `Makefile` (add sanitize.c to COMMON_SRC)

**What:** Notification title/body are written directly to other users' terminals without sanitizing ANSI escape sequences. Malicious user can reset terminals, inject clipboard content, exploit terminal emulator vulns.

**Fix:** Create a `sanitize_for_terminal(const char *input)` function that strips bytes 0x00-0x1F (except 0x0A newline) and 0x7F. Call it on title and body before any pty write. Return a heap-allocated sanitized copy (caller frees).

**Test:** Write tests in `tests/test_sanitize.c` verifying:
- Normal ASCII passes through unchanged
- Control chars (0x01-0x1F except \n) are stripped
- ESC sequences (`\033[31m`, `\033]0;evil\007`) are stripped
- DEL (0x7F) is stripped
- Newlines preserved
- NULL/empty input handled
- Register suite in `tests/test_main.c`

**Commit:** `fix(security): sanitize notification strings before terminal writes`

---

### Task 3: Fix zombie process accumulation (Security C02, Memory C-03, Thread I-06)

**Files:**
- Modify: `src/daemon/main.c` (signal setup section, ~line 410)
- Modify: `src/engine_terminal.c` (overlay dismiss fork, ~line 346)
- Modify: `src/engine_dbus.c` (verify `dbus_call_as_user` waitpid is compatible)

**What:** `terminal_render_overlay()` forks children that are never reaped, creating zombies. Three reviewers flagged this.

**Fix:** Use double-fork pattern in `terminal_render_overlay()`: fork once, in child fork again, parent of inner fork `_exit(0)` immediately, grandchild does the sleep+dismiss work and is reparented to init. The outer parent calls `waitpid(child)` immediately (child exits instantly). This avoids needing `SA_NOCLDWAIT` which would conflict with `dbus_call_as_user`'s `waitpid`.

**Test:** Manual test — send 10 rapid notifications to SSH pty, verify no zombie processes with `ps aux | grep defunct`.

**Commit:** `fix: prevent zombie accumulation from overlay dismiss forks`

---

### Task 4: Add queue size limit (Security C03)

**Files:**
- Modify: `include/queue.h` (add max_size field or constant)
- Modify: `src/queue.c` (enforce limit in `queue_push`)
- Modify: `tests/test_queue.c` (add test for limit)

**What:** Queue has no size limit. Combined with world-writable socket, allows memory exhaustion DoS.

**Fix:** Add `#define QUEUE_MAX_SIZE 1000` in queue.h. In `queue_push`, if `q->count >= QUEUE_MAX_SIZE`, drop the oldest notification (pop head, free it, log warning) before pushing the new one. This preserves newest notifications.

**Test:** Add test that pushes 1001 notifications and verifies count stays at 1000. Verify the oldest was dropped.

**Commit:** `fix(security): cap notification queue at 1000 entries`

---

### Task 5: Add client socket read timeout (Security I02)

**Files:**
- Modify: `src/daemon/socket.c` (`socket_handle_client`, before read loop)

**What:** Synchronous client read blocks the event loop. Malicious client can hold the daemon hostage by connecting and never sending data.

**Fix:** Set `SO_RCVTIMEO` to 2 seconds on the client fd before reading:
```c
struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
```

**Test:** Manual test — `nc -U /run/lnotify.sock` and wait, verify daemon doesn't hang.

**Commit:** `fix(security): add 2s read timeout on client socket`

---

### Task 6: Fix setresgid UID-as-GID bug (Security I05, Code Quality C-02)

**Files:**
- Modify: `src/engine_dbus.c` (`dbus_call_as_user`, ~line 84)

**What:** `setresgid(target_uid, ...)` uses UID as GID. Wrong for users whose primary GID != UID.

**Fix:**
```c
struct passwd *pw = getpwuid(target_uid);
if (!pw) _exit(127);
if (setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) < 0) _exit(127);
```
Add `#include <pwd.h>` if not already present.

**Test:** Existing D-Bus tests should still pass. Manual verification on system with non-matching UID/GID if available.

**Commit:** `fix: use actual GID from passwd in dbus_call_as_user`

---

## Phase 2: Memory Safety & Resource Management

### Task 7: Add NULL checks after all strdup calls (Memory C1, C2, C3, I1)

**Files:**
- Modify: `src/config.c` (`set_str`, `config_defaults`)
- Modify: `src/queue.c` (`node_from_notif`)
- Modify: `src/notification.c` (`notification_init`)
- Modify: `src/engine.c` (`context_init_from_logind`)

**What:** 16+ unchecked strdup calls across the codebase. OOM → NULL dereference.

**Fix:**
- `set_str`: strdup first, only free old value if strdup succeeds (as shown in memory-safety report)
- `config_defaults`: add post-hoc NULL check after all strdup calls, return -1 on failure. Change signature to `int config_defaults(...)`. Update all callers.
- `node_from_notif`: check strdup returns, free node and return NULL on failure. Check return in `queue_push`.
- `notification_init`: check body strdup, return error indicator. Change signature to `int notification_init(...)`. Update callers.
- `context_init_from_logind`: check strdup returns, log and leave NULL on failure.

**Test:** Existing tests should still pass. The strdup failures are OOM paths that are hard to unit-test, but the signature changes need caller updates verified by compilation.

**Commit:** `fix: add NULL checks for all strdup calls`

---

### Task 8: Fix protocol string length overflow (Memory I3, Security I07)

**Files:**
- Modify: `src/protocol.c` (`write_string`, `protocol_serialize`)

**What:** `uint16_t slen = (uint16_t)strlen(str)` silently truncates strings > 65535 bytes, corrupting the wire format.

**Fix:** In `write_string`, check `strlen(str) > UINT16_MAX` and return -1. In `protocol_serialize`, propagate the error.

**Test:** Add test in `tests/test_protocol.c` that attempts to serialize a string > 65535 bytes and verifies it returns an error.

**Commit:** `fix: reject strings exceeding uint16 max in protocol serialization`

---

### Task 9: Add O_CLOEXEC / SOCK_CLOEXEC to all file descriptors (Security M06)

**Files:**
- Modify: `src/daemon/socket.c` (server socket creation)
- Modify: `src/engine_fb.c` (framebuffer open)
- Modify: `src/engine_terminal.c` (proc file opens, /dev/null opens)
- Modify: `src/daemon/main.c` (VT sysfs open)

**What:** Forked children inherit all open fds. The D-Bus child running as a different user inherits the server socket, framebuffer mmap, etc.

**Fix:** Add `SOCK_CLOEXEC` to socket(), `O_CLOEXEC` to all open() calls.

**Commit:** `fix: set close-on-exec on all file descriptors`

---

## Phase 3: Thread Safety & Correctness

### Task 10: Fix defense thread lock ordering and reentrant render (Thread C1, C2)

**Files:**
- Modify: `src/engine_fb.c`

**What:** (1) Defense thread calls `queue_push` while holding `fb_mutex` — lock ordering violation. (2) `fb_render` doesn't check for/dismiss existing defense thread before starting new one.

**Fix for C1:** Copy notification data under lock, release `fb_mutex`, then push to queue:
```c
notification copy;
notification_init(&copy, defense_notif.title, defense_notif.body);
// ... copy fields ...
fb_cleanup_unlocked();
defense_running = false;
pthread_mutex_unlock(&fb_mutex);
queue_push(&g_queue, &copy);
notification_free(&copy);
return NULL;
```

**Fix for C2:** At the top of `fb_render`, if `defense_running` is true, call `fb_dismiss()` first (before acquiring lock) to cleanly shut down the previous defense thread.

**Commit:** `fix: resolve defense thread lock ordering and reentrant render`

---

### Task 11: Fix `const char *` for owned strings in session_context (Code Quality C-01)

**Files:**
- Modify: `include/engine.h` (session_context struct)
- Modify: `src/engine.c` (context_init_from_logind, context_free — remove `(void *)` casts)

**What:** Fields like `username`, `session_type` etc. are `const char *` but heap-allocated, requiring ugly casts to free.

**Fix:** Change to `char *`. Remove all `(void *)` casts in `context_free`.

**Commit:** `refactor: use char* for owned strings in session_context`

---

## Phase 4: Code Quality & Hardening

### Task 12: Extract font size calculation helper (Code Quality I-01)

**Files:**
- Modify: `src/engine_fb.c` (`fb_compute_geometry`, `fb_draw_toast`)

**What:** Font size, spacing, and height calculations are duplicated between `fb_compute_geometry()` and `fb_draw_toast()`.

**Fix:** Extract a `fb_font_metrics` struct and a `fb_calc_font_metrics(cfg)` helper that both functions call.

**Commit:** `refactor: extract shared font metrics calculation in engine_fb`

---

### Task 13: Add byte-order static assertion (Code Quality C-04)

**Files:**
- Modify: `src/protocol.c`

**What:** Protocol uses native byte order but comments claim little-endian. No compile-time check.

**Fix:** Add at top of protocol.c:
```c
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "lnotify protocol assumes little-endian byte order");
```

**Commit:** `fix: add compile-time byte order assertion for wire protocol`

---

### Task 14: Validate integer config values (Security M02)

**Files:**
- Modify: `src/config.c` (`config_set` or a new `config_validate` called after loading)

**What:** Integer config values (`font_size`, `padding`, `timeout`, etc.) are parsed with `atoi()` and never range-checked. `font_size=0` causes division issues, negative padding causes geometry bugs.

**Fix:** After parsing, clamp values: `font_size` [8, 200], `padding` [0, 500], `margin` [0, 500], `border_width` [0, 50], `border_radius` [0, 100], `default_timeout` [100, 300000].

**Test:** Add config tests verifying out-of-range values are clamped.

**Commit:** `fix: validate and clamp integer config values`

---

### Task 15: Fix dry-run response buffer overflow (Security M03)

**Files:**
- Modify: `src/daemon/main.c` (`handle_dry_run`)

**What:** `snprintf` return values are added to `pos` without checking for truncation. If response exceeds 4096 bytes, `pos` overshoots and the final `write()` reads past the buffer.

**Fix:** After each `snprintf`, clamp: `if (pos >= (int)sizeof(response)) pos = (int)sizeof(response) - 1;` and break out of the formatting loop.

**Commit:** `fix: prevent buffer overread in dry-run response formatting`

---

### Task 16: Separate COMMON_SRC for client vs daemon (Code Quality I-06)

**Files:**
- Modify: `Makefile`

**What:** Client binary links all daemon code (engines, fonts, logind) it never uses, inflating binary size.

**Fix:** Split COMMON_SRC into SHARED_SRC (protocol, notification, config, log, socket) and DAEMON_SRC (engines, font, resolver, logind, queue). Client uses SHARED_SRC only.

**Commit:** `refactor: separate client and daemon source lists in Makefile`

---

## Execution Notes

- Each task is independently committable and testable
- Run `make clean && make && make test` after every task
- Both `FREETYPE=1` and `FREETYPE=0` builds must pass
- Security fixes (Phase 1) should be prioritized — deploy those first if needed
- Some tasks may reveal additional issues — note them but don't scope-creep
