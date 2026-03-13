# Thread Safety and Concurrency Review

Reviewed: 2026-03-09
Scope: All source files under `src/` and `include/`
Branch: `dev` (commit 02864af)

## Summary

The lnotify daemon is primarily single-threaded (poll-based event loop) with one secondary thread: the framebuffer defense thread in `engine_fb.c`. The notification queue (`queue.c`) uses pthread mutexes defensively. Overall thread safety is reasonable for the current architecture, but several issues exist -- some critical in the framebuffer engine, and several important patterns that would become dangerous if the architecture evolves toward more concurrency.

**Issue counts:** 3 Critical, 6 Important, 4 Minor


## Critical Issues

### C1. Defense thread calls `queue_push` while holding `fb_mutex` -- deadlock risk and lock ordering violation

**File:** `src/engine_fb.c`, line 467
**Severity:** Critical

The defense thread holds `fb_mutex` and calls `queue_push(&g_queue, &defense_notif)`. `queue_push` acquires `g_queue.lock` internally. Meanwhile, the main thread's event loop calls `dispatch_notification` -> `queue_push` (acquires `g_queue.lock`) and can later call `fb_dismiss` (acquires `fb_mutex`). This creates a potential lock ordering inversion:

- Defense thread: holds `fb_mutex`, then acquires `g_queue.lock` (line 467)
- Main thread: could hold `g_queue.lock` (inside `drain_queue` -> `dispatch_notification` -> `queue_push`), then `dispatch_notification` returns, then later `fb_dismiss` acquires `fb_mutex`

In the current code flow, the main thread does not hold `g_queue.lock` when calling `fb_dismiss`, so this is not a live deadlock **today**. However, it is a fragile invariant -- any future change that calls `fb_dismiss` from within a queue-locked context would deadlock. More critically, the defense thread is accessing `g_queue` from a different thread than the main event loop without any documented lock ordering contract.

**Suggested fix:** Release `fb_mutex` before calling `queue_push`, or copy the notification data under lock and push after unlocking:

```c
// Inside defense_thread_fn, at the 3-consecutive-failures path:
notification copy;
notification_init(&copy, defense_notif.title, defense_notif.body);
copy.priority = defense_notif.priority;
copy.timeout_ms = defense_notif.timeout_ms;
copy.ts_mono = defense_notif.ts_mono;

fb_cleanup_unlocked();
defense_running = false;
pthread_mutex_unlock(&fb_mutex);

queue_push(&g_queue, &copy);
notification_free(&copy);
return NULL;
```

Separately, establish and document a lock ordering policy (e.g., `g_queue.lock` must always be acquired before `fb_mutex`, or vice versa).

---

### C2. `defense_running` and `defense_stop` accessed without lock from `fb_render`

**File:** `src/engine_fb.c`, lines 609-610, 625-631
**Severity:** Critical

In `fb_render`, `defense_stop` and `defense_running` are set at lines 609-610 while `fb_mutex` is held (good). But between lines 625-633, `pthread_create` is called while still holding `fb_mutex`. If it fails, `defense_running` is set to `false` (line 629) -- this is fine because the thread never started. However, the real issue is that `fb_render` does NOT check whether a previous defense thread is still running before starting a new one.

If two notifications arrive in rapid succession, the first call to `fb_render` starts a defense thread. The second call to `fb_render` does not call `fb_dismiss` first (the daemon sets `g_active_engine` but does not dismiss the previous notification before dispatching a new one through the same engine). This means:

1. The first defense thread is still running, referencing `defense_notif`, `saved_region`, `fb_map`, etc.
2. `fb_render` overwrites all of these static variables under `fb_mutex`
3. The first defense thread wakes up, acquires `fb_mutex`, and operates on the new state -- which may be inconsistent with its `consecutive_failures` counter and expectations

**Suggested fix:** At the top of `fb_render`, check if `defense_running` is true and dismiss the existing notification first:

```c
static bool fb_render(const notification *notif, const session_context *ctx) {
    (void)ctx;
    lnotify_config cfg;
    config_defaults(&cfg);

    // Dismiss any existing active notification first
    if (defense_running) {
        // Must release and re-acquire to avoid deadlock with dismiss
        fb_dismiss();
    }

    pthread_mutex_lock(&fb_mutex);
    // ... rest of render
```

Alternatively, have the daemon's `dispatch_notification` call `g_active_engine->dismiss()` before rendering a new notification via the same engine.

---

### C3. FreeType `ft_lib` and `ft_face` are unprotected static globals

**File:** `src/font_freetype.c`, lines 12-13
**Severity:** Critical

`ft_lib` and `ft_face` are file-scope static globals with no mutex protection. They are used by:
- `freetype_text_width()` -- calls `FT_Set_Pixel_Sizes` and `FT_Load_Char` (mutates `ft_face` internal state)
- `freetype_draw_text()` -- calls `FT_Set_Pixel_Sizes` and `FT_Load_Char` with `FT_LOAD_RENDER` (mutates glyph slot)

These functions are called from:
- `fb_render` (main thread, under `fb_mutex`)
- `fb_draw_toast` called from `defense_thread_fn` (defense thread, under `fb_mutex`)

Because both call sites happen to be under `fb_mutex`, this is **not a live race today**. However, `g_font` is a global and `fb_compute_geometry` / `fb_draw_toast` call through it. If any other engine or code path ever calls `g_font.text_width` or `g_font.draw_text` without holding `fb_mutex`, FreeType's internal state (particularly the glyph slot, which is shared across all operations on `ft_face`) would be corrupted.

FreeType's documentation explicitly states that `FT_Face` objects are not thread-safe -- concurrent calls to `FT_Load_Char` on the same face cause undefined behavior.

**Suggested fix:** Either:
1. Add a dedicated `font_mutex` in `font_freetype.c` and lock around every `ft_face` operation, or
2. Document that all `g_font.*` calls must occur under `fb_mutex` (fragile, couples font to framebuffer), or
3. Create per-thread `FT_Face` instances (most robust but more complex)

Option 1 is recommended:

```c
static pthread_mutex_t ft_mutex = PTHREAD_MUTEX_INITIALIZER;

static int freetype_text_width(const char *text, int pixel_size) {
    if (!text || !*text) return 0;
    pthread_mutex_lock(&ft_mutex);
    FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)pixel_size);
    int width = 0;
    for (const char *p = text; *p; p++) {
        if (FT_Load_Char(ft_face, (FT_ULong)*p, FT_LOAD_DEFAULT) != 0)
            continue;
        width += (int)(ft_face->glyph->advance.x >> 6);
    }
    pthread_mutex_unlock(&ft_mutex);
    return width;
}
```


## Important Issues

### I1. Signal handler calls no async-signal-unsafe functions, but `cleanup()` via `atexit` could be problematic

**File:** `src/daemon/main.c`, lines 68-71, 73-78
**Severity:** Important

The signal handler itself (`signal_handler`) is correct -- it only sets a `volatile sig_atomic_t`. However, the `cleanup()` function at line 73 calls `log_info()`, which uses `fprintf`/`vfprintf`/`fflush`. This function is registered nowhere as an `atexit` handler in the current code (it is called manually at line 592), which is fine. But if it were ever registered via `atexit()` or called from a signal context, it would invoke async-signal-unsafe functions.

The current code is correct but worth noting: `cleanup()` must never be called from a signal handler.

**Suggested fix:** Add a comment documenting this constraint:

```c
// WARNING: This function is NOT async-signal-safe (uses log_info/fprintf).
// Must only be called from normal program flow, never from a signal handler.
static void cleanup(void) {
```

---

### I2. `g_system_bus` cached sd-bus connection has no thread safety

**File:** `src/logind.c`, lines 10-21
**Severity:** Important

`g_system_bus` is a static global that is lazily initialized in `get_system_bus()` and freed in `logind_close()`. There is no mutex protecting either operation. Currently all access is from the main thread, but the architecture does not enforce this. If the defense thread (or any future thread) ever calls a logind function, the lazy init would be a race condition.

Additionally, `sd_bus` objects are not thread-safe by default. `sd_bus_call_method` on a shared `sd_bus*` from multiple threads simultaneously would corrupt internal state.

**Suggested fix:** Either add a mutex, or document that all logind functions must be called from the main thread only. The latter is sufficient for the current architecture.

---

### I3. `socket_default_path` uses a static buffer -- not thread-safe

**File:** `src/daemon/socket.c`, lines 152-170
**Severity:** Important

The function returns a pointer to a `static char path_buf[256]`. The header `socket.h` line 23 correctly documents "not thread-safe, do not free." In the current single-threaded usage pattern this is fine, but the function signature gives no hint to future callers that it uses shared static storage. If called from two threads, the buffer would be clobbered.

**Suggested fix:** Since this is called exactly once at startup, the current approach is acceptable. For robustness, consider having the caller pass a buffer:

```c
// Alternative: caller-provided buffer
const char *socket_default_path(bool system_mode, char *buf, size_t buflen);
```

---

### I4. `g_font` global is written during init and read from multiple contexts

**File:** `src/font.c`, lines 5-17; `include/font.h`, line 17
**Severity:** Important

`g_font` is a global `font_backend` struct whose function pointers are set once during `font_init()` (at startup) and then read through the framebuffer engine's render and defense threads. Since initialization happens before any threads are created, this is safe under the current architecture. However, `g_font` has no `const` qualifier and nothing prevents it from being modified after initialization.

**Suggested fix:** After `font_init()` completes, the struct is effectively immutable. Document this invariant. Consider adding a comment in the header:

```c
// Set once by font_init() at startup. Read-only after initialization.
// NOT safe to modify after threads may be running.
extern font_backend g_font;
```

---

### I5. `active_config` pointer shared between main thread and defense thread

**File:** `src/engine_fb.c`, lines 56, 476, 612-622
**Severity:** Important

`active_config` is a pointer to a heap-allocated `lnotify_config` that is set in `fb_render` (main thread, under `fb_mutex`) and read in `defense_thread_fn` (defense thread, under `fb_mutex`). The mutex discipline appears correct -- both accesses are under `fb_mutex`. However, in `fb_render` lines 612-622, the code does:

```c
if (active_config) {
    config_free(active_config);
    free(active_config);
}
active_config = malloc(sizeof(lnotify_config));
if (active_config) {
    config_defaults(active_config);
}
```

This replaces the active config but only copies defaults, not the actual `cfg` local variable's values. The defense thread will re-render using `config_defaults()` values rather than whatever config was used for the initial render. This is a correctness issue more than a thread safety issue, but it means the defense re-render could produce a differently-styled toast than the original.

**Suggested fix:** Deep-copy the actual config used for rendering, not just defaults. This requires either a `config_dup()` function or manual field copying.

---

### I6. Zombie child processes from `terminal_render_overlay`

**File:** `src/engine_terminal.c`, lines 346-381
**Severity:** Important

`terminal_render_overlay` forks a child process to clear the overlay after a timeout. The parent returns immediately without calling `waitpid`, and the comment says "it will be reaped by SIGCHLD or init." However, the daemon does not install a `SIGCHLD` handler with `SA_NOCLDWAIT`, nor does it set `signal(SIGCHLD, SIG_IGN)`. This means the child will become a zombie until the daemon exits.

In a long-running daemon, repeated SSH overlay deliveries could accumulate many zombie processes.

**Suggested fix:** Set `SIGCHLD` to `SIG_IGN` or use `SA_NOCLDWAIT` in the daemon's signal setup:

```c
// In main(), after the SIGINT/SIGTERM handler setup:
struct sigaction sa_chld;
memset(&sa_chld, 0, sizeof(sa_chld));
sa_chld.sa_handler = SIG_IGN;
sa_chld.sa_flags = SA_NOCLDWAIT;
sigaction(SIGCHLD, &sa_chld, NULL);
```

Alternatively, double-fork so the child is reparented to init immediately.


## Minor Issues

### M1. `log_debug_enabled` global is not declared `volatile` or atomic

**File:** `include/log.h`, line 7; `src/log.c`, line 6
**Severity:** Minor

`log_debug_enabled` is a plain `bool` set at startup and read from both the main thread and the defense thread (via `log_debug` calls in `defense_thread_fn`). Since it is only written once before any threads are created, this is safe in practice on all architectures with proper memory ordering from `pthread_create`. However, for documentation clarity and defensive coding, it could be declared `volatile` or `_Atomic`.

---

### M2. `fb_render` sleeps with mutex released, creating a window for state changes

**File:** `src/engine_fb.c`, lines 581-598
**Severity:** Minor

The verification sequence in `fb_render` does:
1. Unlock `fb_mutex` (line 581)
2. Sleep 16ms (line 582)
3. Re-lock `fb_mutex` (line 583)
4. Check `fb_verify_visible()` (line 585)

During the 16ms+ window, another thread could theoretically modify the framebuffer state. In the current architecture, no other thread is running at this point (the defense thread hasn't been created yet), so this is safe. But the pattern is fragile.

**Suggested fix:** Add a comment documenting why this is safe:

```c
// NOTE: No defense thread running yet, so releasing the mutex here is safe.
// The only other accessor of fb state would be fb_dismiss from the main thread,
// but we are ON the main thread.
pthread_mutex_unlock(&fb_mutex);
usleep(16 * 1000);
pthread_mutex_lock(&fb_mutex);
```

---

### M3. `defense_running` should be checked atomically in `fb_dismiss`

**File:** `src/engine_fb.c`, lines 646-655
**Severity:** Minor

In `fb_dismiss`, the code checks `defense_running` under `fb_mutex`, sets `defense_stop = true`, then releases the mutex to call `pthread_join`. This is correct because:
1. The defense thread checks `defense_stop` under `fb_mutex`
2. `pthread_join` will block until the thread exits

However, after `pthread_join` returns, the code re-acquires `fb_mutex` and sets `defense_running = false`. The defense thread itself also sets `defense_running = false` (lines 438, 470) before returning. This double-write is benign but redundant.

**Suggested fix:** Let `fb_dismiss` be the sole writer of `defense_running = false` after a `pthread_join`, and remove the redundant writes in the defense thread. Or accept the redundancy and add a comment explaining it.

---

### M4. `getgrnam` in `user_in_group` is not thread-safe

**File:** `src/daemon/ssh_delivery.c`, lines 51-59
**Severity:** Minor

`getgrnam` returns a pointer to a static buffer and is not thread-safe. In the current single-threaded call path from the main event loop, this is fine. If SSH delivery were ever moved to a worker thread, this would need to use `getgrnam_r`.

**Suggested fix:** No immediate action needed. Add a comment noting the thread-safety limitation.


## Architecture Observations

### Positive patterns

1. **The notification queue has proper mutex discipline.** All public `queue_*` functions acquire `g_queue.lock` before accessing internal state, and unlock on all return paths including error paths. The `queue_destroy` correctly locks before clearing and unlocks before destroying the mutex.

2. **The framebuffer engine's mutex usage is generally correct.** All access to `fb_map`, `saved_region`, `fb_fd`, and other framebuffer state goes through `fb_mutex`. Error paths in `fb_render` consistently unlock before returning.

3. **The signal handler is minimal and correct.** Setting `volatile sig_atomic_t g_running = 0` is the textbook-correct approach. The `SA_RESTART` flag is intentionally not set so `poll()` returns with `EINTR`.

4. **`fork()`-based isolation for D-Bus cross-user calls.** Using `fork()` + `setresuid` to open another user's session bus avoids sharing `sd_bus` objects across privilege boundaries and provides natural isolation.

### Recommendations for future work

1. **Document a lock ordering policy.** As the codebase grows, establish a clear ordering: e.g., always acquire `g_queue.lock` before `fb_mutex`, or vice versa. Document this in a header or architecture doc.

2. **Consider making the defense thread use a condition variable** instead of polling with `usleep(200ms)`. This would allow `fb_dismiss` to wake the thread immediately rather than waiting up to 200ms for it to check `defense_stop`.

3. **Add `SIGCHLD` handling** to prevent zombie accumulation from the overlay dismiss children and any future forked processes.

4. **If adding more engines with threads**, consider a per-engine mutex pattern rather than relying on the global event loop being single-threaded. The current architecture works because only the framebuffer engine has a thread, but this won't scale.
