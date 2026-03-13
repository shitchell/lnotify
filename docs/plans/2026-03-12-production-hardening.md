# Production Hardening Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Fix all remaining issues from the 5-agent production-readiness review: error handling, memory safety, thread safety, resource management, and DRY violations.

**Architecture:** Tasks are grouped into 4 phases. Phase 1 creates shared utility helpers that later phases depend on. Phase 2 fixes error handling and safety bugs. Phase 3 fixes thread safety issues. Phase 4 applies DRY refactoring using the utilities from Phase 1. Each task is independently committable and tested.

**Tech Stack:** C11, pthreads, libsystemd, FreeType2, fontconfig. Build: make. Tests: custom test_util.h framework.

**Review reports:** These fixes address findings from `docs/reviews/` and the 5-agent production review (memory, thread safety, DRY, error handling, resource leaks).

---

## Phase 1: Shared Utilities

### Task 1: Create `replace_str()` helper

**Files:**
- Create: `src/strutil.c`, `include/strutil.h`
- Modify: `Makefile` (add strutil.c to SHARED_SRC)

**What:** The strdup-then-swap pattern (strdup new value, only free+replace old if strdup succeeds) is repeated across the codebase: `config.c:set_str`, `queue.c:node_replace` (3 blocks), `engine.c:context_init_from_logind` (4 blocks). Extract a shared helper.

**Fix:** Create `replace_str(char **dst, const char *src)` that:
- If `src` is NULL: `free(*dst); *dst = NULL; return 0;`
- If `src` is non-NULL: `char *dup = strdup(src); if (!dup) return -1; free(*dst); *dst = dup; return 0;`

Returns 0 on success, -1 on OOM (old value preserved).

**Test:** Write `tests/test_strutil.c` with tests:
- Replace non-NULL with non-NULL (old freed, new set)
- Replace non-NULL with NULL (old freed, set to NULL)
- Replace NULL with non-NULL (new set)
- Replace NULL with NULL (no-op)
- Register suite in `tests/test_main.c`

**Commit:** `feat: add replace_str() shared string utility`

---

### Task 2: Create `notification_copy()` helper

**Files:**
- Modify: `src/notification.c`, `include/lnotify.h`

**What:** Notification deep-copy (notification_init + manual scalar field assignment) is repeated in `engine_fb.c` (defense thread copy at ~line 490, defense setup at ~line 651) and `queue.c:node_from_notif` (~line 13). Extract a helper.

**Fix:** Add `int notification_copy(notification *dst, const notification *src)` to `notification.c`:
```c
int notification_copy(notification *dst, const notification *src) {
    if (notification_init(dst, src->title, src->body) < 0) return -1;
    dst->id         = src->id;
    dst->priority   = src->priority;
    dst->timeout_ms = src->timeout_ms;
    dst->origin_uid = src->origin_uid;
    dst->ts_sent    = src->ts_sent;
    dst->ts_received = src->ts_received;
    dst->ts_mono    = src->ts_mono;
    // Deep-copy optional string fields
    if (src->app) {
        dst->app = strdup(src->app);
        if (!dst->app) { notification_free(dst); return -1; }
    }
    if (src->group_id) {
        dst->group_id = strdup(src->group_id);
        if (!dst->group_id) { notification_free(dst); return -1; }
    }
    return 0;
}
```

Declare in `include/lnotify.h`.

**Test:** Add tests in `tests/test_notification.c`:
- Copy a notification with all fields set, verify deep copy (strcmp, different pointers)
- Copy with NULL title/app/group_id
- Verify original is unchanged after copy
- Free copy, verify original still valid

**Commit:** `feat: add notification_copy() for deep copying notifications`

---

### Task 3: Create `write_all()` helper

**Files:**
- Modify: `src/daemon/socket.c`, `include/socket.h`

**What:** The write-all-bytes-with-EINTR-retry loop is duplicated in `src/client/main.c` (~line 242) and `src/daemon/main.c` (~line 264). Extract a shared helper.

**Fix:** Add `ssize_t write_all(int fd, const void *buf, size_t len)` to `socket.c`:
```c
ssize_t write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return (ssize_t)len;
}
```

Declare in `include/socket.h`. Then replace the inline write loops in both `src/client/main.c` and `src/daemon/main.c` with calls to `write_all()`.

**Test:** Existing tests and manual testing cover this (the logic is unchanged).

**Commit:** `refactor: extract write_all() helper and deduplicate write loops`

---

### Task 4: Move `MAX_MSG_SIZE` to protocol.h

**Files:**
- Modify: `include/protocol.h` (add define)
- Modify: `src/daemon/socket.c` (remove local define, include protocol.h if needed)
- Modify: `src/client/main.c` (remove local define, include protocol.h if needed)

**What:** `#define MAX_MSG_SIZE 65536` is defined identically in two files.

**Fix:** Move the define to `include/protocol.h` alongside `PROTOCOL_HEADER_SIZE`. Remove the local defines.

**Commit:** `refactor: move MAX_MSG_SIZE to protocol.h`

---

## Phase 2: Error Handling & Safety

### Task 5: Replace `atoi()` with safe integer parsing

**Files:**
- Modify: `src/config.c` (lines 133, 141, 154, 156, 158, 160 — 6 atoi calls)
- Modify: `src/daemon/main.c` (`read_vt_number`, line 110)
- Modify: `src/daemon/ssh_delivery.c` (`ssh_check_fullscreen`, line 252)

**What:** `atoi()` has undefined behavior on overflow and provides no error detection. Config values are user-controlled input. The VT number and tpgid parsers read kernel-controlled input but should still use safe parsing for defense-in-depth.

**Fix:** Create a `static int safe_atoi(const char *str, int fallback)` helper in `config.c`:
```c
static int safe_atoi(const char *str, int fallback) {
    char *endp;
    errno = 0;
    long val = strtol(str, &endp, 10);
    if (endp == str || *endp != '\0' || errno == ERANGE
        || val < INT_MIN || val > INT_MAX)
        return fallback;
    return (int)val;
}
```

Replace all 6 `atoi(value)` calls in `config_set` with `safe_atoi(value, <default>)` where `<default>` is the sensible fallback for each field (e.g., `font_size` default 16, `default_timeout` default 5000, etc.). The `clamp_int` call still applies after.

For `read_vt_number` in `daemon/main.c` and `ssh_check_fullscreen` in `ssh_delivery.c`, replace `atoi(p)` with inline `strtol` + error check (return 0 / -1 on parse failure).

**Test:** Add config tests: set a config value to `"99999999999999"` (overflow), `"abc"` (non-numeric), `""` (empty). Verify the fallback is used and the value is clamped.

**Commit:** `fix: replace atoi() with safe strtol-based parsing`

---

### Task 6: Fix client `strtol` → `int32_t` truncation and unchecked strdup/shutdown

**Files:**
- Modify: `src/client/main.c` (lines ~106, ~153-154, ~258)

**What:** Three minor issues in the client:
1. Line ~106: `timeout_ms = (int32_t)val` after `strtol` — no overflow check for `long` → `int32_t`
2. Lines ~153-154: `strdup(app)` and `strdup(group_id)` unchecked for NULL
3. Line ~258: `shutdown(fd, SHUT_WR)` return value unchecked

**Fix:**
1. Add `if (val > INT32_MAX || val < INT32_MIN)` check before cast, print error and exit
2. Check strdup returns: if non-NULL input but strdup returns NULL, print error and exit
3. Check shutdown return: if < 0, `log_error` (don't exit — the notification was already sent)

**Commit:** `fix: client overflow check, strdup checks, shutdown check`

---

### Task 7: Add strdup NULL checks in logind session population

**Files:**
- Modify: `src/logind.c` (lines ~92-95 in `logind_get_session_by_vt`, lines ~202-205 in `logind_list_remote_sessions`)

**What:** 10 unchecked strdup calls across both functions. NULL dereference path exists: `ssh_find_qualifying_ptys` passes `s->username` to `strcmp` without checking.

**Fix:** After each strdup for `session_id`, `username`, `seat`, check for NULL. If any fail, log error and skip the session (continue to next in list, or return -1 for the single-session function). For the fallback `strdup("unknown")` calls, check and use a static fallback: `out->type = strdup("unknown"); if (!out->type) out->type = NULL;` (callers already handle NULL type).

**Commit:** `fix: add strdup NULL checks in logind session population`

---

### Task 8: Check `FT_Set_Pixel_Sizes` return value

**Files:**
- Modify: `src/font_freetype.c` (lines 18, 34)

**What:** `FT_Set_Pixel_Sizes()` can fail for fixed-size fonts. The return value is ignored, and subsequent glyph loading may produce garbage.

**Fix:** Check the return value at both call sites. If it fails:
- In `freetype_text_width` (line 18): return 0 (unknown width)
- In `freetype_draw_text` (line 34): log error and return without drawing

```c
if (FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)pixel_size) != 0) {
    log_error("freetype: FT_Set_Pixel_Sizes failed for size %d", pixel_size);
    return; // or return 0 for text_width
}
```

**Commit:** `fix: check FT_Set_Pixel_Sizes return value`

---

### Task 9: Fix overlay grandchild `clear_buf` snprintf overflow

**Files:**
- Modify: `src/engine_terminal.c` (lines ~368-391, the grandchild clear loop)

**What:** The grandchild builds `clear_buf[2048]` via multiple `snprintf` calls but never checks if `clear_n` exceeds the buffer. Same class of bug that was fixed in the dry-run handler (Task 15 of previous plan). If `clear_n >= sizeof(clear_buf)`, the `sizeof(clear_buf) - (size_t)clear_n` subtraction wraps and the final `write()` reads past the buffer.

**Fix:** Add clamping after each snprintf in the grandchild block, and break out of the loop if the buffer is full:
```c
clear_n += snprintf(clear_buf + clear_n,
                    sizeof(clear_buf) - (size_t)clear_n, ...);
if (clear_n >= (int)sizeof(clear_buf)) {
    clear_n = (int)sizeof(clear_buf) - 1;
    break;
}
```

**Commit:** `fix: prevent buffer overread in overlay dismiss clear sequence`

---

### Task 10: Close inherited fds in overlay grandchild

**Files:**
- Modify: `src/engine_terminal.c` (grandchild block, after second fork, ~line 366)

**What:** The overlay dismiss grandchild holds `pty_fd` and all inherited daemon fds open during its entire sleep period (up to 5+ seconds). While `O_CLOEXEC` protects against exec-based leakage, this grandchild never execs — it just sleeps and writes.

**Fix:** After the write at line ~391, close `pty_fd` before `_exit(0)`. This releases the pty fd immediately after use rather than holding it for the remaining sleep duration.

Actually, looking at the code flow: the grandchild sleeps FIRST then writes. So `pty_fd` is held open during the sleep, which is necessary. Instead, just add `close(pty_fd)` after the `write()` call, before `_exit(0)`.

**Commit:** `fix: close pty_fd in overlay grandchild after use`

---

### Task 11: Check `sigaction`, `clock_gettime` returns and fix log format portability

**Files:**
- Modify: `src/daemon/main.c` (lines ~441-442, sigaction calls)
- Modify: `src/log.c` (lines ~10-11, clock_gettime and format string)
- Modify: `src/notification.c` (lines ~6, 12, clock_gettime calls)

**What:** Three minor robustness issues:
1. `sigaction()` returns are unchecked (lines 441-442 in main.c)
2. `clock_gettime()` returns are unchecked (log.c, notification.c)
3. `log_msg` uses `%ld` for `tv_sec` which is `time_t` — not guaranteed to be `long` on all platforms

**Fix:**
1. Check `sigaction` returns, log error and exit on failure (signal handling is critical):
   ```c
   if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
       log_error("sigaction failed: %s", strerror(errno));
       return 1;
   }
   ```
2. Zero-initialize `struct timespec ts = {0}` before `clock_gettime` calls as defense (so timestamps are 0 on failure rather than garbage). No need to check return — it practically never fails on Linux.
3. Cast `ts.tv_sec` to `(long)` explicitly in the format string for portability.

**Commit:** `fix: check sigaction returns, defensive clock_gettime, portable log format`

---

### Task 12: Add `_Static_assert` for MAX_ENGINES in resolver

**Files:**
- Modify: `src/resolver.c` (near top, after includes)

**What:** `uint32_t rejected` is used as a bitfield with `1u << i`. If `MAX_ENGINES` ever exceeds 32, the shift is undefined behavior. Currently consistent (MAX_ENGINES is 32), but fragile.

**Fix:** Add:
```c
#include "engine.h"  // for MAX_ENGINES
_Static_assert(MAX_ENGINES <= 32,
               "resolver rejected bitfield requires MAX_ENGINES <= 32");
```

**Commit:** `fix: add static assert for MAX_ENGINES vs rejected bitfield width`

---

## Phase 3: Thread Safety

### Task 13: Make `defense_running` atomic

**Files:**
- Modify: `src/engine_fb.c` (line 47 declaration, and all read/write sites)

**What:** `defense_running` is a plain `bool` read by the main thread (line 531) without holding `fb_mutex`, while the defense thread writes it under `fb_mutex`. This is a data race per the C11 memory model (undefined behavior), though benign on x86.

**Fix:** Change declaration from `static bool defense_running = false;` to:
```c
#include <stdatomic.h>
static _Atomic bool defense_running = false;
```

All existing reads and writes work unchanged — C11 `_Atomic` makes all accesses sequentially consistent by default. No other code changes needed since `_Atomic bool` supports `=`, `if(...)`, etc. transparently.

**Commit:** `fix: make defense_running atomic to eliminate data race`

---

### Task 14: Atomic log output (single write per message)

**Files:**
- Modify: `src/log.c` (`log_msg` function, lines 8-16)

**What:** `log_msg` uses three separate `fprintf`/`vfprintf` calls for one log message. If the defense thread and main thread log simultaneously, their output can interleave (e.g., Thread A's timestamp followed by Thread B's message).

**Fix:** Build the entire log line in a local buffer, then emit with a single `write()`:
```c
static void log_msg(const char *level, const char *fmt, va_list args) {
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);

    char buf[1024];
    int pos = snprintf(buf, sizeof(buf), "[%6ld.%03lds] %s: ",
                       (long)ts.tv_sec, ts.tv_nsec / 1000000, level);
    if (pos > 0 && pos < (int)sizeof(buf)) {
        pos += vsnprintf(buf + pos, sizeof(buf) - (size_t)pos, fmt, args);
    }
    if (pos >= (int)sizeof(buf)) pos = (int)sizeof(buf) - 1;
    if (pos > 0 && buf[pos - 1] != '\n') {
        if (pos < (int)sizeof(buf) - 1) buf[pos++] = '\n';
    }
    write(STDERR_FILENO, buf, (size_t)pos);
}
```

This is atomic for messages under `PIPE_BUF` (4096 on Linux). The 1024-byte buffer is sufficient for any log message in this project.

Note: This also addresses the `clock_gettime` and format portability issues from Task 11's `log.c` changes — coordinate so they don't conflict. If Task 11 runs first, this task should adapt.

**Commit:** `fix: atomic log output to prevent interleaved messages`

---

## Phase 4: DRY Refactoring

### Task 15: Apply `replace_str()` to node_replace and context_init_from_logind

**Files:**
- Modify: `src/queue.c` (`node_replace`, lines ~43-82)
- Modify: `src/engine.c` (`context_init_from_logind`, lines ~44-60)

**What:** Both functions repeat the strdup-then-swap pattern that `replace_str()` (Task 1) now provides.

**Fix:**
- `node_replace`: Replace the 3 strdup-then-swap blocks (title, body, app) with `replace_str(&node->notif.title, n->title)` etc. Log on failure.
- `context_init_from_logind`: Replace the 4 strdup-then-swap blocks with `replace_str(&ctx->session_type, session.type ? session.type : "")` etc. Log on failure.

Also update `config.c:set_str` to use `replace_str` if the semantics match (check: `set_str` takes `const char **dst` while `replace_str` takes `char **dst` — after Task 11 from the previous plan changed const char* to char* in session_context, this may already be compatible). If `set_str` can be replaced, remove it and use `replace_str` instead.

**Test:** Existing tests should pass. The behavior is unchanged.

**Commit:** `refactor: use replace_str() in node_replace and context_init`

---

### Task 16: Apply `notification_copy()` to engine_fb and queue

**Files:**
- Modify: `src/engine_fb.c` (defense thread copy ~line 487, defense setup ~line 648)
- Modify: `src/queue.c` (`node_from_notif`, ~line 9)

**What:** Three places manually deep-copy notifications (notification_init + scalar fields). Use `notification_copy()` from Task 2.

**Fix:**
- `engine_fb.c` defense thread fallback (~line 487): Replace `notification_init(&copy, ...) + 3 scalar assignments` with `notification_copy(&copy, &defense_notif)`. Check return.
- `engine_fb.c` defense setup (~line 648): Replace `notification_init(&defense_notif, ...) + 3 scalar assignments` with `notification_copy(&defense_notif, notif)`. Check return.
- `queue.c` `node_from_notif` (~line 9): Replace the manual field-by-field copy with `notification_copy(&node->notif, n)`. Check return, free node on failure.

**Test:** Existing tests should pass. The behavior is unchanged.

**Commit:** `refactor: use notification_copy() for all notification deep copies`

---

### Task 17: Create sanitized_notif helper for terminal renderers

**Files:**
- Modify: `src/engine_terminal.c` (all 5 sanitize+alias blocks)
- Modify: `include/sanitize.h` (add struct and helper declarations)
- Modify: `src/sanitize.c` (add helper implementations)

**What:** Every terminal render function repeats the same 4-line pattern:
```c
char *title = sanitize_for_terminal(notif->title);
char *body  = sanitize_for_terminal(notif->body);
const char *t = title ? title : "";
const char *b = body  ? body  : "";
```
Plus a corresponding free at the end. This appears 5 times.

**Fix:** Add to `sanitize.h`/`sanitize.c`:
```c
typedef struct {
    char *title;       // heap-allocated sanitized title (or NULL)
    char *body;        // heap-allocated sanitized body (or NULL)
    const char *t;     // title or "" if NULL
    const char *b;     // body or "" if NULL
} sanitized_notif;

void sanitize_notif(sanitized_notif *s, const notification *n);
void sanitize_notif_free(sanitized_notif *s);
```

Implementation:
```c
void sanitize_notif(sanitized_notif *s, const notification *n) {
    s->title = sanitize_for_terminal(n->title);
    s->body  = sanitize_for_terminal(n->body);
    s->t = s->title ? s->title : "";
    s->b = s->body  ? s->body  : "";
}

void sanitize_notif_free(sanitized_notif *s) {
    free(s->title);
    free(s->body);
}
```

Replace all 5 blocks in `engine_terminal.c` with:
```c
sanitized_notif sn;
sanitize_notif(&sn, notif);
// ... use sn.t and sn.b ...
sanitize_notif_free(&sn);
```

**Test:** Existing sanitize and terminal tests should pass. Add a test in `test_sanitize.c` for the new helper.

**Commit:** `refactor: extract sanitized_notif helper for terminal renderers`

---

### Task 18: Extract `fb_render` cleanup into goto pattern

**Files:**
- Modify: `src/engine_fb.c` (`fb_render` function, 5 error-cleanup paths at ~lines 549-606)

**What:** `fb_render()` has 5 early-return error paths that each repeat:
```c
close(fb_fd);
fb_fd = -1;
pthread_mutex_unlock(&fb_mutex);
config_free(&cfg);
return false;
```

**Fix:** Replace all 5 error paths with `goto cleanup`:
```c
bool result = false;
// ... on each error:
goto cleanup;

// ... success path:
result = true;

cleanup:
    if (!result && fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
    pthread_mutex_unlock(&fb_mutex);
    config_free(&cfg);
    return result;
```

Ensure the success path sets up fb_fd/fb_map properly before the label so the cleanup doesn't close them on success.

**Commit:** `refactor: use goto cleanup pattern in fb_render`

---

### Task 19: Extract logind session property query helper

**Files:**
- Modify: `src/logind.c`

**What:** Both `logind_get_session_by_vt()` (~lines 99-132) and `logind_list_remote_sessions()` (~lines 209-241) repeat the same pattern for querying Type, Class, Remote, and Leader properties:
```c
sd_bus_error err = SD_BUS_ERROR_NULL;
r = sd_bus_get_property_string(bus, "org.freedesktop.login1", obj,
    "org.freedesktop.login1.Session", "PROP", &err, &out->field);
sd_bus_error_free(&err);
if (r < 0) out->field = strdup("unknown");
```

**Fix:** Extract a helper:
```c
static char *logind_get_session_str(sd_bus *bus, const char *obj,
                                     const char *prop) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char *val = NULL;
    int r = sd_bus_get_property_string(bus, "org.freedesktop.login1", obj,
                "org.freedesktop.login1.Session", prop, &err, &val);
    sd_bus_error_free(&err);
    if (r < 0 || !val) return strdup("unknown");
    return val;
}
```

Replace all property query blocks in both functions with single-line calls:
```c
out->type = logind_get_session_str(bus, obj, "Type");
out->session_class = logind_get_session_str(bus, obj, "Class");
```

Note: The strdup("unknown") fallback should also be NULL-checked per Task 7. Coordinate so Task 7's fixes are preserved.

**Commit:** `refactor: extract logind session property query helper`

---

### Task 20: Replace BGRA pixel writes with memcpy

**Files:**
- Modify: `src/engine_fb.c` (lines ~122-125 and ~160-163)
- Modify: `src/font_bitmap.c` (pixel write in `bitmap_draw_text`)
- Modify: `src/font_freetype.c` (full-alpha pixel write, ~line 62-65)
- Modify: `src/render_util.c` (lines ~112-115)

**What:** The 4-line BGRA pixel write pattern (`pixel[0]=bgra[0]; pixel[1]=bgra[1]; ...`) appears 5 times across 4 files. It can be replaced with `memcpy(pixel, bgra, 4)`.

**Fix:** Replace each 4-line block with `memcpy(pixel, bgra, 4)`. Add `#include <string.h>` where not already present.

Note: Do NOT replace the alpha-blended pixel writes in `font_freetype.c` (~lines 49-60) — those compute per-channel blending and are structurally different.

**Commit:** `refactor: use memcpy for BGRA pixel writes`

---

### Task 21: Extract clip-to-bounds rect helper

**Files:**
- Create or modify: `include/render_util.h`, `src/render_util.c`
- Modify: `src/engine_fb.c` (lines ~110-115 and ~138-143)

**What:** The clip-to-screen-bounds calculation is repeated 3 times across 2 files:
```c
int x0 = rx < 0 ? 0 : rx;
int y0 = ry < 0 ? 0 : ry;
int x1 = rx + rw;
int y1 = ry + rh;
if (x1 > buf_w) x1 = buf_w;
if (y1 > buf_h) y1 = buf_h;
```

**Fix:** Add to `render_util.h`/`render_util.c`:
```c
typedef struct { int x0, y0, x1, y1; } clip_rect;

static inline clip_rect clip_to_bounds(int x, int y, int w, int h,
                                        int buf_w, int buf_h) {
    clip_rect c;
    c.x0 = x < 0 ? 0 : x;
    c.y0 = y < 0 ? 0 : y;
    c.x1 = x + w > buf_w ? buf_w : x + w;
    c.y1 = y + h > buf_h ? buf_h : y + h;
    return c;
}
```

Replace the inline clipping in `fb_draw_rounded_rect`, `fb_draw_rounded_border`, and `render_fill_rect` with calls to `clip_to_bounds()`.

**Commit:** `refactor: extract clip_to_bounds rect helper`

---

## Execution Notes

- Each task is independently committable and testable
- Run `make clean && make && make test` after every task
- Both `FREETYPE=1` and `FREETYPE=0` builds must pass with zero warnings
- Phase 1 tasks (1-4) create utilities that Phase 4 tasks (15-21) depend on — execute in order
- Phase 2 (5-12) and Phase 3 (13-14) are independent of each other but depend on Phase 1 completing
- Tasks 11 and 14 both touch `log.c` — if both run, Task 14 should incorporate Task 11's changes
- Tasks 7 and 19 both touch `logind.c` — Task 7 (strdup checks) should run before Task 19 (DRY refactoring) so the refactoring preserves the safety checks
- Some tasks may reveal additional issues — note them but don't scope-creep
