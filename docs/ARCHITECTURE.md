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
| `include/engine.h` | Engine vtable, session context, probe_key enum | planned |
| `include/lnotify.h` | Notification struct, timestamps, shared constants | planned |
| `include/protocol.h` | Wire format serialize/deserialize | planned |
| `include/config.h` | Config struct and parser | planned |
| `include/resolver.h` | Engine resolver loop | planned |
| `src/daemon/main.c` | Daemon entry point, VT monitor, event loop | [exists] (stub) |
| `src/daemon/socket.c` | Unix socket listener | planned |
| `src/daemon/ssh_delivery.c` | SSH session discovery and pty delivery | planned |
| `src/client/main.c` | CLI entry point | [exists] (stub) |
| `tests/test_main.c` | Test runner with minimal assertion framework | [exists] |

## Implementation Status

**Task 1 complete:** Project scaffolding is in place. Makefile builds `lnotifyd` (daemon) and `lnotify` (client) into `build/`. Logging module provides timestamped debug/info/error output to stderr. Test harness compiles and runs with `make test`. Daemon and client are stubs that log startup and exit.

## Design References

- Full design spec: `docs/plans/2026-03-09-lnotify-design-v2.md`
- Decisions with rationale: `docs/DECISIONS.md`
- Empirical findings: `docs/LESSONS-LEARNED.md`
- Implementation plan: `docs/plans/2026-03-09-lnotify-implementation.md`
