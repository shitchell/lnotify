---
title: Architecture
description: High-level architecture overview of lnotify — components, engine system, data flow, and key interfaces. Updated as implementation progresses.
when: Starting implementation, understanding how components fit together, adding new engines or features
tags: [architecture, engines, daemon, overview]
---

# lnotify Architecture

Universal Linux toast notification system. Three components:

- **`lnotifyd`** — daemon that owns the notification queue and rendering
- **`lnotify`** — thin CLI client (fire-and-forget over Unix socket)
- **D-Bus listener** — optional component inside daemon for `notify-send` compatibility

## Component Diagram

```
                    ┌──────────────────────────────────────────┐
                    │              lnotifyd                     │
                    │                                          │
 lnotify CLI ──────►│  Unix Socket ──► Deserialize ──► Resolver │
                    │                                    │     │
 notify-send ──────►│  D-Bus Listener ──────────────────►│     │
  (optional)        │                                    ▼     │
                    │                              ┌──────────┐│
                    │  VT Monitor ────────────────►│ Engine   ││
                    │  (poll sysfs)                │ Pipeline ││
                    │                              └────┬─────┘│
                    │         ┌──────────────────────────┤      │
                    │         ▼          ▼         ▼     ▼      │
                    │      D-Bus    Framebuffer  SSH   Queue    │
                    │      Engine    Engine    Terminal         │
                    └──────────────────────────────────────────┘
```

## Engine System

Engines implement a vtable:

```c
struct engine {
    const char *name;
    int priority;
    engine_detect_result (*detect)(session_context *ctx);
    bool (*render)(const notification *notif, const session_context *ctx);
    void (*dismiss)(void);
};
```

`detect()` returns `ENGINE_ACCEPT`, `ENGINE_REJECT`, or `ENGINE_NEED_PROBE`.

### Default Priority Order

1. **dbus** — sends `org.freedesktop.Notifications.Notify` via session D-Bus
2. **framebuffer** — direct `/dev/fb0` pixel writes (raw TTY only)
3. **terminal** — writes to SSH pty sessions (OSC → tmux → overlay → text)
4. **queue** — universal fallback, stores for later delivery

### Detection Pipeline

Session context starts with free logind data, grows via lazy probes:

```
logind data (free) → engine requests probe → probe runs → engine re-evaluates
```

Probes: `HAS_DBUS_NOTIFICATIONS`, `COMPOSITOR_NAME`, `HAS_FRAMEBUFFER`, `TERMINAL_CAPABILITIES`, `FOREGROUND_PROCESS`

### Resolver Loop (`resolver_select`)

The resolver iterates engines in array order (priority order) and selects the first that accepts:

1. Skip engines already marked rejected (rejection bitfield)
2. Call `engine->detect(ctx)`:
   - `ENGINE_ACCEPT` — return this engine immediately
   - `ENGINE_REJECT` — mark rejected in bitfield, continue to next
   - `ENGINE_NEED_PROBE` — read `ctx->requested_probe`, run the probe if not already completed, then re-evaluate the same engine. If probe was already completed (engine asked again), treat as reject to prevent infinite loops.

The resolver accepts a `probe_fn` callback for dependency injection. Production code passes `context_run_probe`; tests inject a mock that marks probes complete without system calls. Passing NULL defaults to `context_run_probe`.

## Data Flow

```
1. Notification arrives (socket or D-Bus)
2. Daemon captures origin_uid, sets ts_received, ts_mono
3. Check group_id dedup against current/queued notifications
4. Build session context for active VT
5. Run engine resolver (probe pipeline)
6. Selected engine renders
7. Additionally: SSH delivery to qualifying pty sessions
8. On VT switch: dismiss current, drain queue through pipeline
```

## Key Files

_Updated as implementation progresses. Files marked with [exists] are implemented._

| File | Purpose | Status |
|------|---------|--------|
| `Makefile` | Build system (daemon, client, tests) | [exists] |
| `include/log.h` | Logging module (debug/info/error with timestamps) | [exists] |
| `src/log.c` | Logging implementation | [exists] |
| `include/engine.h` | Engine vtable, session context, probe_key enum | [exists] |
| `src/engine.c` | Session context init (logind), probe dispatch, context cleanup | [exists] |
| `include/lnotify.h` | Notification struct, timestamps, shared constants | [exists] |
| `src/notification.c` | Notification init, free, expiration, dedup | [exists] |
| `tests/test_util.h` | Shared test macros (extracted from test_main.c) | [exists] |
| `include/protocol.h` | Wire format serialize/deserialize | [exists] |
| `src/protocol.c` | Protocol implementation (serialize/deserialize) | [exists] |
| `include/config.h` | Config struct (lnotify_color, lnotify_config) and parser | [exists] |
| `src/config.c` | Config parser: defaults, file loading, color parsing, free | [exists] |
| `tests/test_config.c` | Config tests (defaults, file parse, colors, whitespace, booleans) | [exists] |
| `include/resolver.h` | Engine resolver loop API | [exists] |
| `src/resolver.c` | Resolver implementation (probe pipeline, rejection tracking) | [exists] |
| `tests/test_resolver.c` | Resolver tests with mock engines (24 assertions) | [exists] |
| `src/daemon/main.c` | Daemon entry point, VT monitor, event loop | [exists] (stub) |
| `src/daemon/socket.c` | Unix socket listener | planned |
| `src/daemon/ssh_delivery.c` | SSH session discovery and pty delivery | planned |
| `src/client/main.c` | CLI entry point | [exists] (stub) |
| `include/queue.h` | Thread-safe notification queue (FIFO, dedup, expiration) | [exists] |
| `src/queue.c` | Queue implementation (linked list, mutex-protected) | [exists] |
| `tests/test_queue.c` | Queue tests (enqueue/dequeue, dedup, expiration, FIFO) | [exists] |
| `tests/test_main.c` | Test runner with minimal assertion framework | [exists] |
| `include/render_util.h` | Shared rendering utilities (geometry, color conversion, fill) | [exists] |
| `src/render_util.c` | Rendering utilities implementation | [exists] |
| `include/font_bitmap.h` | Embedded 8x8 bitmap font API | [exists] |
| `src/font_bitmap.c` | Bitmap font data (printable ASCII 32-126) | [exists] |
| `tests/test_render_util.c` | Rendering utility tests (43 assertions) | [exists] |
| `include/engine_fb.h` | Framebuffer engine header (exports `engine_framebuffer`) | [exists] |
| `src/engine_fb.c` | Framebuffer engine: detect, render, dismiss with defense thread | [exists] |
| `tests/manual/test_fb.sh` | Manual framebuffer test instructions (requires raw TTY) | [exists] |
| `include/engine_dbus.h` | D-Bus engine header (exports `engine_dbus`) | [exists] |
| `src/engine_dbus.c` | D-Bus engine: detect, render via gdbus, cross-user fork+setresuid | [exists] |
| `tests/manual/test_dbus.sh` | Manual D-Bus test instructions (requires GUI session) | [exists] |
| `include/engine_queue.h` | Queue engine header (exports `engine_queue`) | [exists] |
| `src/engine_queue.c` | Queue engine: universal fallback, always accepts, pushes to g_queue | [exists] |

## Implementation Status

**Task 1 complete:** Project scaffolding is in place. Makefile builds `lnotifyd` (daemon) and `lnotify` (client) into `build/`. Logging module provides timestamped debug/info/error output to stderr. Test harness compiles and runs with `make test`. Daemon and client are stubs that log startup and exit.

**Task 2 complete:** Notification data model (`notification` struct) with creation, string ownership, expiration tracking, remaining timeout calculation, and group_id-based dedup. Test macros extracted to shared `tests/test_util.h`. All 14 notification tests pass.

**Task 3 complete:** Wire protocol serialize/deserialize with field_mask support. Binary format: fixed 19-byte header (total_len, field_mask, priority, timeout_ms, ts_sent) followed by length-prefixed strings (title, body, app, group_id). Forward-compatible: unknown field_mask bits are ignored. 34 protocol tests pass covering round-trips, error cases, field combinations, and forward-compatibility.

**Task 4 complete:** Config parser with `#RRGGBBAA` color support and all v1 config keys. Key=value flat file format, `#` comments, whitespace-tolerant. `config_defaults()` populates all fields from design spec defaults. `config_load()` overrides from file, skipping malformed/unknown lines (logged at debug). `config_free()` cleans up heap strings. 48 config tests pass covering defaults, file parsing, color edge cases, whitespace handling, and boolean parsing.

**Task 5 complete:** Engine vtable (`engine` struct with detect/render/dismiss function pointers), session context struct, and probe infrastructure. `context_init_from_logind()` queries `loginctl` to populate session properties (type, class, user, seat, remote) for a given VT. `context_run_probe()` dispatches probes via switch statement: `PROBE_HAS_DBUS_NOTIFICATIONS` (gdbus introspect), `PROBE_HAS_FRAMEBUFFER` (access check), others stubbed. Probe bitfield prevents redundant work. Tests bypass probes by setting context fields directly (validated in Task 6). `context_free()` cleans up all heap-allocated strings.

**Task 6 complete:** Engine resolver loop (`resolver_select`) iterates engines in priority order, handles `ENGINE_NEED_PROBE` by running probes and re-evaluating, tracks rejections via bitfield, and prevents infinite loops when a probe is already completed. Probe function is injectable via `probe_fn` callback — tests use a mock that marks probes done without system calls; production code passes `context_run_probe` (or NULL to default). 24 resolver tests cover: first-accept, skip-rejected, all-reject, probe-then-accept, probe-negative-fallback, rejected-skipped-after-probe, pre-completed-probe, probe-then-reject, empty list, null probe_fn, and multi-probe scenarios.

**Task 7 complete:** Thread-safe notification queue (`notif_queue`) using a singly-linked list protected by a pthread mutex. `queue_push` deep-copies notifications and performs group_id dedup (scans for matching group_id and replaces in-place). `queue_pop` returns the front entry (FIFO). `queue_pop_live` drains expired notifications and returns the first live one (or NULL if all expired). `queue_size` returns the current count. 10 queue tests cover: enqueue/dequeue, group_id dedup replacement, expired notification skipping, and FIFO ordering. Total: 146 tests passing.

**Task 9 complete:** Framebuffer engine (`engine_framebuffer`) — first real engine implementation. Detects by rejecting wayland/x11 sessions and probing for `/dev/fb0` write access. Renders toast directly into mmap'd framebuffer: rounded rectangle background, border, bitmap font text. Saves/restores the region underneath for clean dismissal. Read-after-write verification at +16ms/+50ms (20-point border pixel sampling, >80% match threshold). Background defense thread re-checks every 200ms for `timeout/2` duration, re-renders if clobbered. After 3 consecutive defense failures, the thread cleans up directly under the mutex (not through `dismiss()` -- avoids self-join deadlock per LESSONS-LEARNED.md) and pushes the notification to `g_queue` for later delivery. Global `g_queue` added to `queue.h`/`queue.c` for engine fallback use. Manual test only (requires real framebuffer hardware).

**Task 10 complete:** D-Bus notification engine (`engine_dbus`) — highest-priority engine, tried first. Detects by accepting only wayland/x11 sessions and probing for `org.freedesktop.Notifications` on the session bus. Renders by shelling out to `gdbus call` with the Notify method. For same-user delivery, executes gdbus directly. For cross-user delivery (system daemon running as root, target session owned by a different user), uses `fork()` + `setresgid()`/`setresuid()` to become the target user, discovers `DBUS_SESSION_BUS_ADDRESS` from `/proc/{leader}/environ`, sets `XDG_RUNTIME_DIR`, then execs gdbus in the child process. Retry with exponential backoff: 200ms base, 1.5x multiplier, max 5 attempts (handles slow notification server startup). Dismiss is a no-op — the notification server handles its own timeout/dismissal. Shell-escapes title and body to prevent injection. All 189 existing tests still pass.

**Task 11 complete:** Queue engine (`engine_queue`) — simplest engine, universal fallback registered last (priority 100). `detect()` always returns `ENGINE_ACCEPT`. `render()` pushes the notification to `g_queue` via `queue_push()` and logs it. `dismiss()` is a no-op. This ensures every notification is captured even when no display engine can handle it. All 189 existing tests still pass.

**Task 8 complete:** Shared rendering utilities that engines compose from. Pure functions with no side effects: `color_to_bgra` (RGBA-to-BGRA conversion for framebuffer byte order), `point_in_rounded_rect` (hit-test with corner arc clipping), `compute_toast_geometry` (position string to screen coordinates with margin), `text_width` (string width at given scale using 8x8 font), `render_fill_rect` (solid rectangle fill into a BGRA buffer with stride and clipping). Embedded 8x8 bitmap font covers all printable ASCII (32-126) with `get_char_bitmap()` returning per-character row data. Font data adapted from the Python prototype. 43 render utility tests pass covering color conversion, rounded-rect hit-testing (center, corners, edges, zero-radius), all four toast positions plus unknown-position fallback, text width edge cases (empty, NULL), font bitmap lookups (printable, space, fallback to '?'), and fill-rect pixel verification. Total: 189 tests passing.

## Design References

- Full design spec: `docs/plans/2026-03-09-lnotify-design-v2.md`
- Decisions with rationale: `docs/DECISIONS.md`
- Empirical findings: `docs/LESSONS-LEARNED.md`
- Implementation plan: `docs/plans/2026-03-09-lnotify-implementation.md`
