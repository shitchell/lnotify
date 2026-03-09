---
title: Design Spec v2
description: Full design specification — engine vtable, probe pipeline, notification data model, wire protocol, SSH terminal notifications, config format, daemon modes.
when: Implementing any component, understanding the full system design, checking interface contracts
tags: [design, specification, engines, protocol, ssh, config]
---

# lnotify Design Specification v2

Universal Linux toast notification system that works across virtual consoles, Wayland, X11, SSH terminals, and any display environment.

## Core Architecture

Three components:

- **`lnotifyd`** — daemon that owns the notification queue and rendering. Listens on a Unix socket. Detects the active session type via a layered probe pipeline and selects the appropriate rendering engine. Monitors VT switches and re-routes notifications accordingly. Additively notifies qualifying SSH terminal sessions.
- **`lnotify`** — thin CLI client. Connects to the daemon's Unix socket, sends a notification, returns immediately (fire-and-forget).
- **D-Bus listener** (optional, inside daemon) — implements `org.freedesktop.Notifications` spec so existing tools (`notify-send`, etc.) work. Only activates if D-Bus is available. Loaded via `dlopen`.

## Language & Build

- **C** — chosen for natural framebuffer/ioctl affinity, zero runtime deps, smallest binary.
- **Font rendering** — bitmap font for v1, FreeType for production.
- **Target architectures** — x86_64 + ARM (Raspberry Pi). Both are first-class.
- **Dynamic loading** — `dlopen` for `libwayland-client`, `libxcb`, D-Bus libs. Single binary works everywhere; backends activate only if their libs exist at runtime.
- **License** — WTFPL.

## Daemon Modes

### Per-user mode (default)
- Runs as the invoking user
- Socket at `$XDG_RUNTIME_DIR/lnotify.sock`
- Detects only its own sessions
- Framebuffer access via `video` group membership
- Config at `~/.config/lnotify/config`

### System mode (`lnotifyd --system`)
- Runs as root
- Socket at `/run/lnotify.sock`
- Full cross-session visibility
- Maps active VT to session owner, delegates to appropriate engine
- For D-Bus calls on another user's session: `fork` + `setresuid`/`setresgid` to become target user, set `DBUS_SESSION_BUS_ADDRESS`, make call directly (no sudo dependency)
- Config at `/etc/lnotify.conf` (per-user config can override style values but not security-relevant settings like `ssh_groups`, `listen`)

## Notification Data Model

```c
typedef struct {
    uint32_t    id;             // daemon-assigned, monotonically increasing
    char       *title;          // optional (NULL = no title)
    char       *body;           // required
    uint8_t     priority;       // 0=low, 1=normal, 2=critical
    int32_t     timeout_ms;     // -1 = use config default
    char       *app;            // optional, e.g. "cron", "backup-agent"
    char       *group_id;       // optional, for dedup (same group_id replaces)
    uint32_t    origin_uid;     // daemon-captured via SO_PEERCRED
    uint64_t    ts_sent;        // wall clock ms, client-provided
    uint64_t    ts_received;    // wall clock ms, daemon-assigned on receipt
    uint64_t    ts_mono;        // monotonic ms, daemon-assigned, internal only
} notification;
```

**Timestamps:**
- `ts_sent` — wall clock (Unix epoch ms), set by client, travels on the wire, comparable across machines. Used for display ("sent 3 minutes ago").
- `ts_received` — wall clock, set by receiving daemon, on the wire for downstream forwarding. Delta from `ts_sent` shows delivery latency.
- `ts_mono` — monotonic clock, set by receiving daemon, never on the wire. Used only for local timeout/expiration math. Immune to NTP jumps and clock corrections.

**Dedup behavior:** When a notification arrives with a non-NULL `group_id` matching a currently-displayed or queued notification, it replaces that notification. The replaced notification's timeout resets to the new one's value. Enables progress-style updates (`lnotify --group backup "50%"` → `lnotify --group backup "75%"`) as a single toast that refreshes.

## Wire Protocol (Client → Daemon)

Length-prefixed binary, no serialization library. Fixed header followed by variable-length string fields.

```
[total_len: uint32]
[field_mask: uint16]     // bitfield: which optional fields are present
[priority: uint8]
[timeout_ms: int32]
[ts_sent: uint64]        // wall clock ms
[title_len: uint16] [title: bytes]      // if FIELD_TITLE set
[body_len: uint16]  [body: bytes]       // always present
[app_len: uint16]   [app: bytes]        // if FIELD_APP set
[group_len: uint16] [group_id: bytes]   // if FIELD_GROUP set
```

The `field_mask` provides forward-compatibility — old clients don't set new bits, daemon uses defaults for missing fields. `id`, `ts_received`, `ts_mono`, and `origin_uid` are never sent by the client.

## CLI Interface

```bash
lnotify "body text"                              # minimal
lnotify -t "Title" "body text"                   # with title
lnotify -t "Title" -p critical "body text"       # priority
lnotify --app cron --group backup "Backup: 75%"  # app + dedup
lnotify --timeout 10000 "long notification"      # custom timeout
```

## Engine Architecture

### Engine vtable

The core abstraction is the **engine** — a pluggable backend that knows how to detect whether it's relevant and how to render a notification for a given session.

```c
typedef enum {
    ENGINE_ACCEPT,
    ENGINE_REJECT,
    ENGINE_NEED_PROBE,
} engine_detect_result;

typedef enum {
    PROBE_HAS_DBUS_NOTIFICATIONS,
    PROBE_COMPOSITOR_NAME,
    PROBE_HAS_FRAMEBUFFER,
    PROBE_TERMINAL_CAPABILITIES,
    PROBE_FOREGROUND_PROCESS,
} probe_key;

struct engine {
    const char *name;
    int priority;               // lower = tried first

    engine_detect_result (*detect)(const session_context *ctx);
    bool (*render)(const notification *notif, const session_context *ctx);
    void (*dismiss)(void);
};
```

### Default engine priority

1. **dbus** — probes for `org.freedesktop.Notifications`, sends via D-Bus call
2. **wlr-layer-shell** — (future) creates overlay surface on wlroots compositors
3. **framebuffer** — direct `/dev/fb0` writes with verification and defense
4. **terminal** — writes to SSH pty sessions (OSC → tmux → cursor overlay → plain text)
5. **queue** — always accepts, stores for later delivery

The priority order lives in a config struct in code. V1 config file doesn't expose it, but the struct is there for future configurability.

Shared rendering logic (rounded rectangles, text drawing, etc.) lives in utility modules. Engines compose from shared pieces rather than reimplementing them.

### Engine registration limit

Engine tracking uses a `uint32_t` bitfield for the rejection mask, limiting to 32 engines. If more are ever needed, this can be widened to `uint64_t` or replaced with a `uint8_t rejected[MAX_ENGINES]` array.

## Session Context & Detection Pipeline

The session context accumulates information about a session across detection phases. It starts with cheap logind data and grows only as engines request probes.

```c
typedef struct {
    // Phase 0: always available (from logind)
    uint32_t    vt;
    uint32_t    uid;
    const char *username;
    const char *session_type;       // "wayland", "x11", "tty", ""
    const char *session_class;      // "user", "greeter", "lock-screen"
    const char *seat;               // "seat0" or ""
    bool        remote;

    // Probed fields (populated on demand)
    bool        has_dbus_notifications;
    const char *compositor_name;
    bool        has_framebuffer;
    const char *terminal_type;
    bool        terminal_supports_osc;
    const char *foreground_process;

    // Probe bookkeeping
    uint32_t    probes_completed;   // bitfield of probe_key
    probe_key   requested_probe;    // set by engine returning NEED_PROBE
} session_context;
```

### Resolver loop

```
build_context_from_logind(ctx)
engines_rejected = 0

for each engine in priority order:
    if engines_rejected & (1 << engine_index):
        continue

    result = engine->detect(ctx)

    if result == ACCEPT:
        selected_engine = engine
        break

    if result == REJECT:
        engines_rejected |= (1 << engine_index)
        continue

    if result == NEED_PROBE:
        if probe not yet run:
            run_probe(ctx, ctx->requested_probe)
            // loop continues from current engine,
            // rejected engines skipped automatically
        else:
            // probe already ran, engine still can't decide
            engines_rejected |= (1 << engine_index)
            continue
```

Key behaviors:
- Context is built **once per VT switch** (or per notification in per-user mode), reused for all engines
- Probes run **at most once** per context — the bitfield prevents redundant work
- When a probe runs, re-evaluation starts from the **requesting engine**, not the top — engines above already decided
- Rejected engines are **skipped on subsequent iterations**
- Worst case: `num_engines × num_probes` iterations (in practice 2-3 passes)

### Probe cost tiers

| Probe | Cost | Method |
|-------|------|--------|
| `HAS_DBUS_NOTIFICATIONS` | cheap (~5ms) | D-Bus introspect on session bus |
| `HAS_FRAMEBUFFER` | trivial | `access("/dev/fb0", W_OK)` |
| `COMPOSITOR_NAME` | cheap | Check `$WAYLAND_DISPLAY` socket or `/proc` scan |
| `TERMINAL_CAPABILITIES` | medium | `$TERM`/`$TERM_PROGRAM` check, optional XTVERSION query with timeout |
| `FOREGROUND_PROCESS` | cheap | `/proc/{pid}/stat` field 8 (tpgid) |

## VT Switch Detection

### Primary: `poll()` on sysfs
- `poll()` on `/sys/class/tty/tty0/active` — fires immediately on every VT switch, no ownership required, no compositor conflicts, zero latency.

### Fallback: timed read
- Periodic read of the same sysfs file — covers kernels where poll doesn't work on sysfs.

### Enrichment: logind
- Query logind via D-Bus (`loginctl show-session`) for session type, class, owner, VT number on each switch. Populates the session context.

## Rendering Backends

### D-Bus engine

Sends `org.freedesktop.Notifications.Notify` on the session's D-Bus. The compositor's own notification server handles rendering, positioning, and dismissal.

- Probes for `org.freedesktop.Notifications` before committing
- Retry with exponential backoff (200ms base, 1.5x, max 5 attempts) to handle session bus not ready during compositor launch
- For cross-user delivery in system mode: `fork` + `setresuid` to target user
- Notifications are self-dismissing (notification server handles timeout)

### Framebuffer engine

Direct pixel writes to `/dev/fb0` for raw TTY sessions.

- 32bpp BGRA pixel format (from `ioctl FBIOGET_VSCREENINFO`)
- Rounded rectangle toast with configurable colors, border, padding, radius
- Save/restore region underneath for clean dismissal
- Bitmap font for v1, FreeType for production

**Verification and defense:**
- Read-after-write verification at +16ms and +50ms (sampling border pixels, 20 points, >80% match threshold)
- Sustained defense: re-check every 200ms for `timeout / 2`
- Re-render if clobbered; give up after 3 consecutive failures
- Background defense thread for remaining notification lifetime
- If defense fails, notification moves to queue

**Framebuffer is skipped** when the session's compositor bypasses `/dev/fb0` (Wayland, X11 with DRM/KMS). The engine's `detect` function checks `session_type` and rejects accordingly.

### wlr-layer-shell engine (future)

For wlroots-based compositors (Sway, etc.) that lack a notification daemon. Uses `wlr-layer-shell-unstable-v1` protocol to create an always-visible overlay surface. The compositor cooperates by protocol contract.

Would also enable lnotifyd to register as `org.freedesktop.Notifications` on the session bus, acting as both the notification daemon and renderer.

### Queue (universal fallback)

Always accepts. Stores notification with remaining timeout. On next VT switch, drains queue through the full engine pipeline. Expired notifications are dropped on dequeue.

## SSH Terminal Notifications

SSH terminal notification is **additive** — it runs alongside whatever engine rendered on the local display. When a notification fires, the daemon checks for qualifying SSH sessions and delivers to their ptys.

### Routing rules

The daemon notifies SSH sessions based on config:

```ini
ssh_users = guy
ssh_groups =
```

For a per-user daemon, only that user's sessions are visible (config keys are irrelevant). For a system daemon, `ssh_users` and `ssh_groups` control which SSH sessions receive notifications. Both default to empty — only explicit configuration enables SSH delivery.

### Client-side opt-out

SSH clients can control notification modes via environment variable:

```bash
export LNOTIFY_SSH=overlay,text    # only these modes
export LNOTIFY_SSH=none            # opt out entirely
export LNOTIFY_SSH=all             # everything (default if unset)
```

Requires `AcceptEnv LNOTIFY_SSH` in sshd_config. If not configured, the variable isn't forwarded and the daemon uses its defaults.

Server-side global control via config:

```ini
ssh_modes = osc,overlay,text
```

### Rendering fallback chain (per pty)

```
1. OSC escape sequences (OSC 9 / OSC 777)
   - Detected via $TERM / $TERM_PROGRAM / XTVERSION probe (cached per session)
   - Terminal renders a native notification popup
   - Zero visual disruption

2. tmux display-popup / display-message
   - Detected via $TMUX env var
   - display-popup (tmux 3.2+) for floating overlay, display-message as fallback
   - Native to tmux, doesn't interfere with pane content

3. Cursor overlay
   - Save cursor, move to top-right, draw ANSI-colored box, restore cursor
   - Works on virtually all terminals
   - Respects full-screen app detection (see below)

4. Plain text
   - Write a styled ANSI line to the pty
   - Universal fallback, always works
```

### Full-screen app detection

Check the pty's foreground process group via `/proc/{session_leader}/stat` field 8 (tpgid). Look up the process name of the foreground group leader. Match against a configurable list:

```ini
ssh_fullscreen_apps = vim nvim less man htop top nano emacs
ssh_notify_over_fullscreen = false
```

When `ssh_notify_over_fullscreen = false` and a full-screen app is detected, the notification is **held** (not dropped) and delivered when the user returns to a shell prompt. Detection: poll the foreground process group periodically, deliver when it changes to the shell.

### Terminal capability caching

OSC/XTVERSION probes run once per pty session and the result is cached. Terminal capabilities don't change mid-session.

## Configuration

**Format:** Key=value flat file, one pair per line. `#` comments. No sections for v1.

**Color format:** `#RRGGBBAA` hex strings. The `#` prefix discriminates from future formats (e.g., `rgb(R, G, B)`).

**V1 config keys:**

```ini
# ── Display ──────────────────────────────────────
default_timeout = 5000
position = top-right
font_path = /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
font_size = 14

# ── Toast style ──────────────────────────────────
bg_color = #282828E6
fg_color = #FFFFFFFF
border_color = #64A0FFFF
border_width = 2
border_radius = 12
padding = 20
margin = 30

# ── SSH terminal notifications ───────────────────
ssh_modes = osc,overlay,text
ssh_fullscreen_apps = vim nvim less man htop top nano emacs
ssh_notify_over_fullscreen = false
ssh_groups =
ssh_users =

# ── Network (future, off by default) ────────────
# listen =
# upstream =
```

**Internal config struct (not exposed in v1 config file):**

Engine priority ordering lives in the config struct, ready to be surfaced in the config file when needed.

**Config precedence:** hardcoded defaults → config file → command-line flags. One pass at startup, immutable after. SIGHUP reload is a future flex point.

## Future Flex Points (designed for, not v1)

### Upstream/downstream daemon mesh

`lnotifyd` instances on remote servers push notifications to a local `lnotifyd` over TCP. Config:

```ini
upstream = workserver1:9999
upstream = workserver2:9999
listen = 0.0.0.0:9999
```

Wire format is the same as the Unix socket protocol. `ts_sent` is preserved across hops. Auth mechanism TBD (likely mutual TLS or pre-shared key).

**Time-sync:** Upstream/downstream connections include a periodic timestamp exchange (NTP-lite) to estimate clock offset. Correction factor stored per connection, applied to `ts_sent` for display accuracy. Does not affect timeout/expiration logic.

### Notification stacking

Queue is already a list. Render function takes a single notification in v1. Adding stacking is "render N from the front" with layout logic for positioning multiple toasts.

### Priority-based styling

Priority field exists in data model. V1 treats all priorities the same. Future: different colors/persistence/sound per priority level.

### Engine priority config

The engine ordering struct is ready. Exposing it via config file is straightforward when needed.

### SIGHUP config reload

Config struct is immutable after startup. Reload would rebuild the struct and swap atomically.

### Additional color formats

`#` prefix discriminates hex. Parser can be extended to handle `rgb()`, `hsl()`, named colors, etc.

## Open Questions / Needs Future Research

- **GDM greeter notifications** — `session_class=greeter` identifies it, but no rendering path exists. DRM overlay planes (hardware-dependent), custom GDM extension, or accept queue behavior.
- **Multiple monitors** — toast position per-monitor or primary only? Framebuffer engine would need multi-head awareness.
- **Notification actions** (buttons, click-to-dismiss) — D-Bus spec supports actions. Framebuffer would need input handling (GPM integration?).
- **tmux/screen nesting** — cursor overlay writes to the outer pty. tmux `display-popup` is the preferred path. screen equivalent needs research.
- **Upstream/downstream auth** — mutual TLS? Pre-shared key? Token-based? Not yet designed.

## Empirical Findings (from prototyping)

See `docs/plans/2026-03-09-lessons-learned.md` for detailed findings. Key takeaways:

- `poll()` on `/sys/class/tty/tty0/active` is the best VT detection method
- Wayland compositors bypass `/dev/fb0` entirely — framebuffer writes are invisible on compositor VTs
- GDM's greeter is a full Wayland session (`Type=wayland`, `Class=greeter`) that tears down and relaunches on VT switches
- Custom GTK4 windows on foreign Wayland sessions suffer PHANTOM rendering — compositor reports frames but never displays them
- D-Bus `org.freedesktop.Notifications` is the correct approach for GUI sessions
- Root cannot directly connect to another user's session bus — must authenticate as target user
- Background defense threads must not call through `dismiss_current()` — they clean up directly under the lock to avoid self-join deadlock
