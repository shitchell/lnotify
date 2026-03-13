# Security Review: lnotify Codebase

**Date:** 2026-03-09
**Reviewer:** Claude Opus 4.6 (automated security review)
**Scope:** All source files under `src/` and `include/`
**Branch:** `dev` (commit `02864af`)
**Context:** The daemon (`lnotifyd`) runs as root in system mode and processes user-supplied data from a Unix socket. It writes to framebuffers, ptys, and forks child processes that change UIDs.

---

## Executive Summary

The codebase is generally well-structured with good defensive practices in several areas (SO_PEERCRED for origin tracking, bounds checking in framebuffer drawing, protocol length validation). However, running as root while processing arbitrary user input through a world-writable socket creates a meaningful attack surface. The most significant findings relate to command injection via tmux, unbounded queue growth for denial of service, and socket permission model weaknesses.

**Issue counts by severity:**
- Critical: 3
- Important: 9
- Minor: 7

---

## Critical Issues

### C01. Command injection via tmux display-popup

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_terminal.c`, lines 204-225
**CWE:** CWE-78 (OS Command Injection)

The `terminal_render_tmux()` function constructs a shell command string using `snprintf` and passes it as a tmux `-E` argument. The tmux `display-popup -E` flag runs the argument via the user's shell. The notification title and body are interpolated directly into a `echo '...'` command string:

```c
snprintf(popup_cmd, sizeof(popup_cmd), "echo '%s'; sleep %d",
         msg, duration_secs);
```

A notification body containing `'; rm -rf / #` would break out of the single-quoted echo and execute arbitrary commands. Since the daemon runs as root and the tmux command is executed via `execvp` (inheriting root privileges unless the child drops privileges first), this is a root-level command injection.

The code even acknowledges this with a comment: "Note: single quotes in msg would break the echo, but this is a pre-existing v1 limitation."

**Suggested fix:** Either (a) escape single quotes in the message by replacing `'` with `'\''`, (b) write the message to a temporary file and have the popup read it, or (c) use `display-message` exclusively (which does not invoke a shell). Given the root context, option (c) is safest for v1.

---

### C02. Zombie process accumulation (denial of service)

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_terminal.c`, lines 346-381
**CWE:** CWE-400 (Uncontrolled Resource Consumption)

`terminal_render_overlay()` forks a child process for each notification's dismiss timer but never waits for it. The comment says "let the child run in the background (it will be reaped by SIGCHLD or init)" -- but the daemon does NOT install a `SIGCHLD` handler with `SA_NOCLDWAIT`, and it does not call `waitpid()` for these children. Each notification delivery to an SSH pty creates a zombie process.

An attacker who can send notifications rapidly (the socket is world-writable) can exhaust the process table with zombies, which on Linux has a per-user limit (`/proc/sys/kernel/pid_max`). Since the daemon runs as root, this could exhaust the system PID space entirely.

**Suggested fix:** Either (a) set `signal(SIGCHLD, SIG_IGN)` or use `SA_NOCLDWAIT` in the sigaction setup (but be careful this does not conflict with `dbus_call_as_user` which uses `waitpid`), or (b) double-fork so the child is reparented to init, or (c) maintain a list of child PIDs and reap them periodically.

---

### C03. Unbounded notification queue -- memory exhaustion DoS

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/queue.c`, lines 71-101
**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/socket.c`, line 60 (world-writable socket)
**CWE:** CWE-770 (Allocation of Resources Without Limits or Throttling)

The notification queue (`queue_push`) has no maximum size limit. Any local user can connect to the world-writable socket and flood the daemon with notifications that get queued when no engine is available (or when the queue engine is selected). Each notification allocates heap memory for the node and all string fields.

Since `timeout_ms` is user-controlled and can be set to `-1` (never expire) or to extremely large values, queued notifications will never be garbage-collected by `queue_pop_live`. A sustained flood with `timeout_ms=-1` and unique `group_id` values would cause unbounded heap growth until OOM kills the daemon (or worse, other root processes).

**Suggested fix:** Add a configurable maximum queue size (e.g., 1000 entries). When the limit is reached, either drop the oldest notification or reject the new one. Also consider rate-limiting connections per UID using `SO_PEERCRED`.

---

## Important Issues

### I01. World-writable Unix socket without rate limiting

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/socket.c`, line 60
**CWE:** CWE-732 (Incorrect Permission Assignment for Critical Resource)

The socket is created with `chmod(path, 0666)`, making it writable by any local user. While `SO_PEERCRED` captures the sender's UID, nothing prevents any user from flooding the daemon with requests. There is no:
- Per-UID rate limiting
- Connection throttling
- Maximum concurrent connection limit
- Authentication or authorization checks

The socket backlog is only 5 (`listen(fd, 5)`), which provides some implicit throttling but also means legitimate notifications could be dropped during a flood.

**Suggested fix:** For system-mode daemons, consider using group-based socket permissions (e.g., `0660` with a `lnotify` group). Implement per-UID rate limiting (e.g., max 10 notifications per second per UID). Increase the listen backlog for legitimate burst handling.

---

### I02. Synchronous client handling blocks the event loop

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/main.c`, lines 526-557
**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/socket.c`, lines 82-101
**CWE:** CWE-400 (Uncontrolled Resource Consumption)

The daemon handles clients synchronously in the main event loop. `socket_handle_client` reads from the client socket in a loop until EOF. A malicious client can connect, send partial data, and never close the connection. The daemon will block indefinitely in the `read()` loop (line 86-101 of socket.c), unable to process VT switch events or other notifications.

**Suggested fix:** Set a read timeout on the client socket using `setsockopt(SO_RCVTIMEO)` or use `poll()` with a timeout for the client read loop. A reasonable timeout would be 1-5 seconds.

---

### I03. No validation of user-supplied `font_path` in config

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/config.c`, line 119
**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/font_freetype.c`, lines 131-135
**CWE:** CWE-22 (Path Traversal)

The `font_path` config value is passed directly to `FT_New_Face()` without any validation or sanitization. While the config file itself requires file-system access to modify, in system mode the config is at `/etc/lnotify.conf` which is root-only. However, if a user-mode daemon reads `$XDG_CONFIG_HOME/lnotify/config`, and an attacker can control that file (e.g., through a symlink attack on the config directory), they could point `font_path` at an arbitrary file.

FreeType has historically had multiple CVEs for malformed font file parsing (CVE-2020-15999 being a notable example). Pointing FreeType at a crafted file could lead to code execution.

**Suggested fix:** Validate that `font_path` points to a file with a recognized font extension (`.ttf`, `.otf`, `.woff`). Consider using `realpath()` and checking that the resolved path is within expected font directories. At minimum, log the resolved path.

---

### I04. No validation of `socket_path` from config -- symlink attack

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/socket.c`, lines 23-73
**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/socket.c`, line 36 (unlink before bind)
**CWE:** CWE-367 (TOCTOU Race Condition), CWE-59 (Improper Link Resolution)

`socket_listen()` calls `unlink(path)` then `bind()` then `chmod(path, 0666)`. If the socket path is in a world-writable directory (like `/tmp/lnotify.sock` -- the fallback path), there is a TOCTOU race:

1. Daemon calls `unlink("/tmp/lnotify.sock")` -- removes existing file
2. Attacker creates a symlink: `/tmp/lnotify.sock -> /etc/shadow`
3. Daemon calls `bind()` -- this will fail because `/etc/shadow` exists, but if the attacker times it differently with `chmod`:
4. Daemon calls `chmod("/tmp/lnotify.sock", 0666)` -- if the symlink existed before unlink, or if bind creates through the symlink, this could chmod the target

More practically: an attacker could create a symlink at the socket path pointing to a file they want the daemon to unlink (since the daemon runs as root, `unlink` succeeds on any file).

**Suggested fix:** Use `O_NOFOLLOW` semantics. Before unlinking, check with `lstat()` that the path is a socket (not a symlink). Better yet, for `/tmp` paths, create a private directory first with restricted permissions (`/tmp/lnotify-XXXXX/`), or avoid `/tmp` entirely and require `XDG_RUNTIME_DIR` or `/run`.

---

### I05. `dbus_call_as_user` uses UID as GID

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_dbus.c`, line 84
**CWE:** CWE-269 (Improper Privilege Management)

```c
if (setresgid(target_uid, target_uid, target_uid) < 0) _exit(127);
```

The code sets the group ID to the target UID value. While on many systems the primary GID matches the UID (user private groups), this is not guaranteed. If a user has UID 1000 but primary GID 1001, the forked child would run with GID 1000 (which might be a different group entirely), potentially gaining unintended group privileges or losing expected ones.

**Suggested fix:** Look up the user's actual primary GID using `getpwuid(target_uid)` and use that for `setresgid()`.

---

### I06. /proc/pid/environ read has race condition and truncation

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_terminal.c`, lines 20-49
**CWE:** CWE-367 (TOCTOU Race Condition)

`read_proc_env()` reads up to 8192 bytes from `/proc/{pid}/environ`. Two issues:

1. **Race condition:** The PID obtained from logind could be recycled by the time the daemon reads `/proc/{pid}/environ`. The daemon (running as root) would then read the environment of an unrelated process, potentially making security decisions (LNOTIFY_SSH opt-out) based on the wrong process.

2. **Truncation:** If the environment block is larger than 8192 bytes (common with large PATH, conda environments, etc.), the LNOTIFY_SSH variable might not be found even though it is set, causing the user's opt-out preference to be silently ignored.

**Suggested fix:** For the truncation issue, either read in a loop until EOF or use a larger buffer (or `mmap`). For the PID race, verify the process owner (by checking `/proc/{pid}/status` for Uid) matches the expected session UID before trusting the environment data.

---

### I07. Integer overflow in protocol string length field

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/protocol.c`, line 60
**CWE:** CWE-190 (Integer Overflow)

In `write_string()`:
```c
uint16_t slen = (uint16_t)strlen(str);
```

If a string is longer than 65535 bytes, this silently truncates the length. The subsequent `memcpy(buf + 2, str, slen)` would only copy the first `(strlen(str) % 65536)` bytes, but the header would declare a different length than what was actually intended. On the serialize side this causes data corruption rather than a security issue because `total` is computed with the full `strlen()` and the buffer size check would likely fail.

On the deserialize side, the length is read correctly as uint16, so it is inherently bounded. However, the mismatch between serialized total_len and actual field lengths could cause a deserializer to read past the intended boundary of one field into the next.

**Suggested fix:** Validate that `strlen(str) <= UINT16_MAX` in `write_string()` and return -1 if exceeded. Also add an explicit check in `protocol_serialize()` for string field lengths.

---

### I08. Forked overlay child inherits root and uses pty_fd after parent closes it

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_terminal.c`, lines 346-376
**CWE:** CWE-269 (Improper Privilege Management)

The overlay dismiss child process (forked at line 347) inherits the daemon's root privileges. It sleeps for the timeout duration, then writes to the pty fd. This child:
- Runs as root for the entire sleep duration
- Holds open a pty file descriptor that the parent also closes (line 365 of ssh_delivery.c) -- the child's fd is a dup inherited from fork, so it remains valid
- Does not drop privileges before writing to the user's pty

Additionally, the parent closes `pty_fd` at line 365 of `ssh_delivery.c`, but the child still has its own copy and writes to it after sleeping. This works due to fork semantics but is fragile.

**Suggested fix:** The child should drop privileges to the pty owner's UID/GID before sleeping. Close all unnecessary file descriptors in the child (server socket, VT sysfs fd, etc.). Consider using a timer-based approach instead of forking.

---

### I09. No input sanitization on notification strings written to terminals

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/engine_terminal.c`, lines 67-136, 256-382, 388-418
**CWE:** CWE-150 (Improper Neutralization of Escape, Meta, or Control Sequences)

Notification title and body strings are written directly to SSH ptys without sanitizing ANSI escape sequences. A malicious notification sender could embed terminal escape sequences in the title/body that:
- Change the terminal title (OSC 0/2)
- Modify terminal settings (e.g., `\033c` resets the terminal)
- Inject clipboard content on terminals that support OSC 52
- Write arbitrary content to the screen that persists after the overlay dismiss
- Exploit terminal emulator vulnerabilities through crafted escape sequences

This is especially concerning because any local user can send notifications through the world-writable socket, and those notifications are then written to other users' terminals.

**Suggested fix:** Strip or escape all control characters (bytes < 0x20 except \n, and 0x7F) and escape sequences (anything matching `\033[...` or `\033]...`) from title and body before writing to ptys. A simple filter that removes bytes in the range 0x00-0x1F (except 0x0A for newline) and 0x7F would prevent the most dangerous attacks.

---

## Minor Issues

### M01. Static buffer in `socket_default_path` is not thread-safe

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/socket.c`, lines 152-170
**CWE:** CWE-362 (Race Condition)

`socket_default_path()` uses a `static char path_buf[256]` and the header comment explicitly says "not thread-safe, do not free." Currently the daemon is single-threaded for socket handling, but the defense thread in engine_fb.c runs concurrently. If socket path resolution were ever called from the defense thread context, this would be a data race.

**Suggested fix:** Accept a caller-provided buffer, or document the single-threaded constraint more prominently.

---

### M02. `config_load` does not validate integer ranges

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/config.c`, lines 113-114, 134-141
**CWE:** CWE-20 (Improper Input Validation)

Integer config values like `default_timeout`, `font_size`, `border_width`, `padding`, and `margin` are parsed with `atoi()` and stored without range validation. Setting `font_size=0` causes division in `bitmap_text_width` (`pixel_size / FONT_HEIGHT`), `padding=-99999` could cause integer underflow in geometry calculations, and `default_timeout=0` combined with certain code paths could cause tight loops.

**Suggested fix:** Clamp integer values to reasonable ranges after parsing (e.g., `font_size` in [8, 200], `padding` in [0, 500], `default_timeout` in [100, 300000]).

---

### M03. `handle_dry_run` response buffer can silently truncate

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/main.c`, lines 131-255
**CWE:** CWE-120 (Buffer Overflow)

The dry-run handler uses a fixed 4096-byte response buffer. `snprintf` return values are added to `pos` without checking if truncation occurred. If `snprintf` returns a value larger than `remaining` (indicating truncation), `pos` advances past the actual buffer content. While `snprintf` itself is safe (it does not write past the buffer), subsequent `snprintf` calls would receive a `remaining` that could go negative (cast to a very large `size_t`), though `snprintf` handles this safely by writing nothing.

The real issue is that `pos` could exceed `sizeof(response)`, and the final `write()` at line 243 would use this inflated `pos` as the write length, reading past the response buffer.

**Suggested fix:** After each `snprintf`, clamp `pos` to `sizeof(response) - 1` and check if `remaining <= 0` to stop appending.

---

### M04. `atoi()` used for security-irrelevant but potentially confusing parsing

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/main.c`, line 106
**CWE:** CWE-20 (Improper Input Validation)

`read_vt_number()` uses `atoi()` to parse the VT number from sysfs. While the sysfs file is trusted kernel output, `atoi` returns 0 on parse failure which is the same as "no VT found." This is handled correctly (VT 0 is treated as failure), but `strtoul` with error checking would be more robust.

---

### M05. Potential information disclosure in log messages

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/daemon/socket.c`, lines 138-145
**CWE:** CWE-532 (Information Exposure Through Log Files)

The daemon logs the full notification body and title at INFO level:
```c
log_info("received notification: title=\"%s\" body=\"%s\" ...");
```

If notifications contain sensitive content (passwords, tokens, private messages), this data would appear in system logs. With systemd journal in system mode, these logs are readable by users in the `systemd-journal` group.

**Suggested fix:** Log notification metadata (priority, timeout, uid) at INFO level but log the actual title/body content only at DEBUG level.

---

### M06. Missing `O_CLOEXEC` on file descriptors

**Files:** Multiple -- `socket.c` line 29, `engine_fb.c` line 507, `engine_terminal.c` line 24, `engine_terminal.c` line 153
**CWE:** CWE-403 (Exposure of File Descriptor to Unintended Control Sphere)

File descriptors opened by the daemon (server socket, framebuffer device, `/proc` files, `/dev/null` in `run_tmux`) do not use `O_CLOEXEC` or `SOCK_CLOEXEC`. When the daemon forks children (for D-Bus calls, tmux commands, overlay dismiss), all open file descriptors are inherited. The forked child in `dbus_call_as_user` (which changes to a different user) inherits the server socket fd, the framebuffer mmap, and any other open descriptors.

**Suggested fix:** Use `SOCK_CLOEXEC` flag when creating the server socket (`socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)`). Use `O_CLOEXEC` flag on all `open()` calls. Alternatively, explicitly close inherited fds in child processes.

---

### M07. Client timeout strtol overflow

**File:** `/home/guy/code/git/github.com/shitchell/lnotify/src/client/main.c`, lines 100-106
**CWE:** CWE-190 (Integer Overflow)

The client parses `--timeout` with `strtol()` but does not check for `ERANGE` (overflow). A value like `--timeout 99999999999999` would overflow `long` on 32-bit systems, and the cast `(int32_t)val` would silently truncate on 64-bit systems.

**Suggested fix:** Check `errno == ERANGE` after `strtol()` and validate `val <= INT32_MAX`.

---

## Items Reviewed and Found Acceptable

The following areas were reviewed and found to have adequate security measures:

- **Protocol deserialization bounds checking:** `protocol_deserialize()` properly validates `total_len` against `buflen`, checks header size minimums, and bounds-checks each string field read against `total_len`. This prevents buffer over-reads.

- **Framebuffer pixel bounds checking:** All framebuffer drawing functions (`fb_draw_rounded_rect`, `fb_draw_rounded_border`, `bitmap_draw_text`, `freetype_draw_text`, `render_fill_rect`) clip coordinates to screen/buffer bounds before writing pixels. The `fb_save_region` and `fb_restore_region` functions also perform bounds checking.

- **SO_PEERCRED usage:** The daemon correctly uses `SO_PEERCRED` to capture the originating UID of each notification. While this is currently only used for logging, it provides the foundation for future authorization checks.

- **D-Bus privilege separation:** `dbus_call_as_user()` correctly forks, sets both GID and UID (albeit with the wrong GID value -- see I05), and runs the D-Bus operation in an isolated child process. The parent waits for completion.

- **`run_tmux` uses `execvp` instead of `system()`:** The tmux execution helper avoids shell injection for the argument vector itself by using `execvp` rather than `system()`. However, the `display-popup -E` flag still invokes a shell (see C01).

- **Protocol message size limit:** `socket_handle_client` enforces a 64 KiB maximum message size (`MAX_MSG_SIZE`), preventing unbounded reads from a single client connection.

---

## Recommendations Summary

**Immediate (pre-release):**
1. Fix C01 (tmux command injection) -- remove `display-popup -E` with interpolated user data
2. Fix C02 (zombie accumulation) -- add proper child reaping
3. Fix C03 (unbounded queue) -- add a queue size limit

**Short-term:**
4. Fix I02 (client read timeout) -- add SO_RCVTIMEO or poll-based timeout
5. Fix I04 (symlink race on socket path) -- validate path before unlink
6. Fix I09 (terminal escape injection) -- sanitize notification strings before pty writes
7. Fix I01 (rate limiting) -- add per-UID throttling

**Medium-term:**
8. Fix I05 (GID mismatch) -- look up actual GID from passwd
9. Fix I03 (font_path validation) -- sanitize font paths
10. Fix M06 (CLOEXEC) -- add close-on-exec to all file descriptors
11. Fix M03 (dry-run buffer overflow) -- clamp snprintf position tracking
