# Memory Safety and Resource Management Review

**Reviewer:** Claude Opus 4.6
**Date:** 2026-03-09
**Branch:** dev (commit 02864af)
**Scope:** All files under `src/` and `include/`

---

## Summary

The codebase is generally well-structured with consistent cleanup patterns. Most functions handle error paths correctly with appropriate `goto` or early-return cleanup. The review identified 16 issues: 3 Critical, 7 Important, and 6 Minor.

---

## Critical Issues

### C1. Missing NULL checks after strdup in `config_defaults` and `context_init_from_logind`

**File:** `src/config.c` lines 82-103, `src/engine.c` lines 20-23, 38-44

**Description:** `strdup()` can return NULL on allocation failure, but the return values are never checked. If any of these fail, subsequent code will dereference NULL pointers. In `config_defaults`, 8 `strdup` calls go unchecked. In `context_init_from_logind`, 8 more go unchecked (4 initial defaults + 4 replacements).

**Suggested fix:** Check each `strdup` return value. For `config_defaults`, return an error code (change signature to `int config_defaults(...)` returning -1 on OOM). For `context_init_from_logind`, log the failure and leave the field NULL. At minimum, add a single post-hoc check:

```c
// After all strdup calls in config_defaults:
if (!cfg->position || !cfg->font_name || !cfg->font_path ||
    !cfg->ssh_modes || !cfg->ssh_fullscreen_apps ||
    !cfg->ssh_groups || !cfg->ssh_users || !cfg->engine_priorities) {
    config_free(cfg);
    return -1;
}
```

### C2. Missing NULL check after strdup in `set_str`

**File:** `src/config.c` line 26

**Description:** `set_str` frees the old value then assigns `strdup(value)`. If `strdup` returns NULL, the field becomes NULL. This silently drops config values and can cause NULL dereferences downstream when code assumes config strings are non-NULL (e.g., `strcmp(ctx->session_type, "wayland")` in `engine_dbus.c` line 29 does guard for NULL, but `word_in_list` and many `snprintf` calls do not).

**Suggested fix:** Check for NULL and log a warning, or keep the old value:

```c
static void set_str(char **field, const char *value) {
    char *new_val = strdup(value);
    if (!new_val) {
        log_error("set_str: strdup failed, keeping old value");
        return;
    }
    free(*field);
    *field = new_val;
}
```

### C3. Missing NULL check after strdup in `node_from_notif`

**File:** `src/queue.c` lines 13-15

**Description:** `node_from_notif` calls `strdup` for `title`, `body`, `app`, and `group_id` without checking return values. If `body` strdup fails, the node will have `notif.body == NULL`, which violates the invariant that body is always non-NULL in a valid notification. This could cause NULL dereference when the notification is later rendered or logged.

**Suggested fix:** Check each strdup result and free/return NULL on failure:

```c
node->notif.body = n->body ? strdup(n->body) : NULL;
if (n->body && !node->notif.body) {
    notification_free(&node->notif);
    free(node);
    return NULL;
}
```

---

## Important Issues

### I1. `notification_init` does not check strdup return for body

**File:** `src/notification.c` line 19

**Description:** `notification_init` calls `strdup(body)` but body can be NULL per the function signature (`body ? strdup(body) : NULL`). However, the protocol and header comment say "body is required." If `strdup` fails on a non-NULL body, `n->body` becomes NULL silently, and later serialization (`protocol_serialize`) will reject it. The real concern is callers that proceed to use the notification without checking whether body is actually set.

**Suggested fix:** Return an error code from `notification_init` when body strdup fails, or at minimum document that callers must check `n->body != NULL` after calling.

### I2. File descriptor leak in `terminal_render_overlay` child process

**File:** `src/engine_terminal.c` lines 346-376

**Description:** The `fork()` in `terminal_render_overlay` creates a child that inherits the parent's file descriptors (the server socket, sysfs fd, any open pty fds from the loop). The child only uses `pty_fd` but never closes the inherited fds before `_exit(0)`. While `_exit` will close them, the child sleeps for potentially 5+ seconds holding them open. More critically, the parent never calls `waitpid` on this child -- the comment says it will be "reaped by SIGCHLD or init." If SIGCHLD is not handled (and daemon/main.c does not set up a SIGCHLD handler), these become zombie processes, one per overlay notification.

**Suggested fix:** Either:
1. Set `signal(SIGCHLD, SIG_IGN)` in the daemon to auto-reap children, or
2. Double-fork so the child is immediately orphaned and reaped by init:

```c
pid_t pid = fork();
if (pid == 0) {
    // Double-fork to avoid zombies
    pid_t pid2 = fork();
    if (pid2 != 0) _exit(0);  // first child exits immediately
    // Grandchild does the work
    usleep((useconds_t)timeout * 1000);
    (void)write(pty_fd, clear_buf, (size_t)clear_n);
    _exit(0);
}
if (pid > 0) waitpid(pid, NULL, 0);  // reap first child immediately
```

### I3. Integer overflow risk in `protocol_serialize` size calculation

**File:** `src/protocol.c` lines 93-101

**Description:** The total size is computed by adding `strlen()` results to `PROTOCOL_HEADER_SIZE`. If a malicious or buggy caller provides extremely long strings (multiple GB), the `size_t` additions could theoretically overflow on 32-bit systems, making `total` wrap to a small value. Then `buflen < total` would pass, and `write_string` calls would write past the buffer. On 64-bit systems this is not exploitable in practice, but the `write_string` helper truncates string length to `uint16_t` (line 60: `uint16_t slen = (uint16_t)strlen(str)`), silently truncating strings longer than 65535 bytes without returning an error.

**Suggested fix:** Add a length check in `write_string`:

```c
static ssize_t write_string(uint8_t *buf, size_t remaining, const char *str) {
    size_t raw_len = strlen(str);
    if (raw_len > UINT16_MAX) return -1;  // string too long
    uint16_t slen = (uint16_t)raw_len;
    ...
}
```

### I4. `active_config` leaked if `pthread_create` fails in `fb_render`

**File:** `src/engine_fb.c` lines 625-631

**Description:** If `pthread_create` fails at line 625, `defense_running` is set to false but the function returns `true` (line 635). The toast is displayed and `active_config` has been allocated, but nobody will ever call `fb_dismiss` because the daemon only calls dismiss on VT switch or shutdown, and the defense thread is not running to auto-dismiss. The toast pixels will be left on screen permanently, and `active_config` will leak when the next `fb_render` call overwrites the pointer (though the code does check and free it at line 613).

Actually, reviewing more carefully: `fb_dismiss` is called on VT switch or shutdown, so `active_config` will be freed. The real issue is the toast remaining on screen with no auto-dismiss. This is a logic/UX issue more than a memory leak, since `fb_dismiss` will eventually clean it up.

**Severity downgrade to Minor.** No action strictly required for memory safety, but worth noting for robustness.

### I5. `defense_notif` not freed in defense thread timeout path before `fb_cleanup_unlocked`

**File:** `src/engine_fb.c` lines 436-440

**Description:** This is actually handled correctly -- `fb_cleanup_unlocked()` at line 386 calls `notification_free(&defense_notif)` at line 402. No issue here upon closer review.

**Retracted.**

### I5 (replacement). `read_proc_env` truncates large environments silently

**File:** `src/engine_terminal.c` lines 20-49

**Description:** `read_proc_env` reads at most 8191 bytes from `/proc/{pid}/environ`. If the environment is larger (common for processes with many environment variables or long PATH values), the target variable may be cut off or missed entirely. The `strlen(p)` call on line 39 could also read past the 8192-byte buffer if the final NUL-terminated entry was truncated mid-entry (the last entry would not be NUL-terminated). The code does add a NUL at `buf[nread]` (line 32), so `strlen` is bounded, but a truncated entry's value would be wrong.

**Suggested fix:** After `buf[nread] = '\0'`, verify the loop does not match a truncated final entry by checking `p + entry_len + 1 <= end` before advancing. Consider using a larger buffer or dynamic allocation for correctness with large environments.

### I6. Zombie processes from `dbus_call_as_user` child (cross-user path)

**File:** `src/engine_dbus.c` lines 70-107

**Description:** The `dbus_call_as_user` cross-user fork correctly calls `waitpid`. However, this function is called from `dbus_exec_with_retry` which may call it up to 5 times. Each call blocks on `waitpid`, which is correct. No issue here.

**Retracted -- this is handled correctly.**

### I6 (replacement). `snprintf` truncation in `terminal_render_overlay` not checked before write

**File:** `src/engine_terminal.c` lines 295-328

**Description:** Multiple `snprintf` calls build up the ANSI escape sequence in `buf[4096]`. The variable `n` accumulates the return values, but `snprintf` returns the number of characters that *would have been* written (not the actual number written). If the output is truncated, `n` will exceed `sizeof(buf)`, and subsequent `snprintf` calls will compute negative `remaining` values, cast to `size_t`, becoming huge. The truncation check at line 330 (`if (n >= (int)sizeof(buf))`) catches this after the fact, but prior `snprintf` calls with the bogus `remaining` could write out of bounds depending on compiler behavior.

Actually, looking more carefully: each `snprintf` call uses `sizeof(buf) - (size_t)n` as the size. If `n >= sizeof(buf)`, this wraps to a huge `size_t` value, and `snprintf` could potentially write out of bounds. In practice, modern glibc `snprintf` implementations check the actual buffer boundary, but this is undefined behavior per the C standard.

**Suggested fix:** Add an intermediate check after each snprintf block, or cap `n` at `sizeof(buf)` after each call:

```c
if (n >= (int)sizeof(buf)) {
    log_error("ssh: overlay buffer overflow");
    return false;
}
```

### I7. `dbus_call_as_user` uses target_uid for both UID and GID

**File:** `src/engine_dbus.c` line 84

**Description:** `setresgid(target_uid, target_uid, target_uid)` sets the GID to the numeric UID value. This is incorrect -- users and groups can have different IDs. The child process will run with the wrong group, which could cause D-Bus bus open to fail or succeed with wrong permissions.

**Suggested fix:** Look up the user's primary GID via `getpwuid`:

```c
struct passwd *pw = getpwuid(target_uid);
if (!pw) _exit(127);
if (setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) < 0) _exit(127);
if (setresuid(target_uid, target_uid, target_uid) < 0) _exit(127);
```

This is not strictly a memory safety issue, but it is a correctness bug found during the review that could affect security.

---

## Minor Issues

### M1. `fb_save_region` does not check for negative `saved_geom.x`

**File:** `src/engine_fb.c` lines 269-276

**Description:** If `saved_geom.x` is negative (possible if margin > screen width in `compute_toast_geometry`), then `fb_map + sy * fb_stride + saved_geom.x * 4` computes a pointer before the start of the mmap'd region. The `if (saved_geom.x < 0) continue;` check at line 275 skips the copy but only after computing the `src` pointer at line 269. This is technically UB (pointer arithmetic outside allocated bounds) even though the pointer is never dereferenced.

**Suggested fix:** Move the `saved_geom.x < 0` check before the pointer computation.

### M2. `fb_restore_region` same issue as M1

**File:** `src/engine_fb.c` lines 287-298

**Description:** Same negative `saved_geom.x` issue as `fb_save_region`.

### M3. Static buffer in `socket_default_path` is not thread-safe

**File:** `src/daemon/socket.c` lines 152-170

**Description:** The header comment correctly notes "not thread-safe, do not free." The daemon is currently single-threaded for socket operations, so this is fine. Noted for future awareness if threading is added.

### M4. `config_load` does not detect truncated lines

**File:** `src/config.c` lines 179-184

**Description:** If a config file line exceeds 1023 characters, `fgets` will not include the newline, and the next `fgets` call will read the remainder as a separate "line." This could cause a long value to be silently truncated and the remainder to be misinterpreted as a new key=value pair. Unlikely in practice for notification config, but worth documenting.

### M5. `log_msg` format string uses `%ld` for `tv_sec` which is `time_t`

**File:** `src/log.c` line 11

**Description:** `ts.tv_sec` is of type `time_t`, which is not guaranteed to be `long`. On some platforms it could be `long long`. This could produce incorrect log timestamps.

**Suggested fix:** Cast explicitly: `(long)ts.tv_sec`.

### M6. `protocol_serialize` string length truncation to uint16

**File:** `src/protocol.c` line 60

**Description:** As noted in I3, `strlen` result is silently truncated to `uint16_t`. Strings longer than 65535 bytes will be serialized with wrong length, corrupting the wire format. This overlaps with I3 but is worth calling out separately as it affects both serialize and deserialize consistency.

---

## Areas Reviewed and Found Correct

The following patterns were reviewed and found to be properly implemented:

- **mmap/munmap pairing** in `engine_fb.c`: `fb_cleanup_unlocked` correctly munmaps and closes the fd. All error paths in `fb_render` either clean up or never reach the mmap.
- **Queue mutex lifecycle**: `queue_init` / `queue_destroy` correctly init and destroy the mutex. The destroy happens after draining all nodes.
- **`notification_free` idempotency**: Sets pointers to NULL after free, safe to call multiple times.
- **`config_free` idempotency**: Same pattern, all fields set to NULL.
- **`logind_session_free` idempotency**: Same pattern.
- **`context_free`**: Frees all heap-allocated strings and zeroes the struct.
- **Protocol deserialization cleanup**: The `fail` label calls `notification_free` which handles partially-populated notifications.
- **Daemon shutdown ordering**: The daemon correctly dismisses active engines, frees context, closes fds, destroys queue, cleans up fonts, frees config, and closes logind bus -- in a safe order.
- **`sd_bus_error` and `sd_bus_message` cleanup**: All logind.c D-Bus calls properly free errors and unref messages in all paths.
- **Client `main.c` cleanup**: Properly frees notification and closes fd on all error and success paths.
- **SSH delivery `ssh_deliver`**: Properly closes pty_fd and frees client_modes on all paths through the per-pty loop.
- **`ssh_find_qualifying_ptys`**: Properly calls `logind_session_free` for every session, including skipped ones.

---

## Recommendations

1. **Priority:** Fix C1-C3 (NULL checks after allocation). These are the most likely to cause crashes under memory pressure.
2. **Priority:** Fix I7 (UID used as GID). This is a correctness bug that could cause cross-user D-Bus delivery to fail.
3. **Priority:** Fix I2 (zombie processes from overlay dismiss). Add `signal(SIGCHLD, SIG_IGN)` early in daemon startup.
4. **Consider:** Fix I3/M6 (string length validation in protocol). Add `strlen > UINT16_MAX` guard in `write_string`.
5. **Consider:** Fix I6 (snprintf overflow in overlay rendering). Add intermediate bounds checks.
6. **Low priority:** Address M1-M5 for defense in depth.
