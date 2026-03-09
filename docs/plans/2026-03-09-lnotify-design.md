# lnotify Design Specification

Universal Linux toast notification system that works across virtual consoles, Wayland, X11, and any display environment.

## Core Architecture

Three components:

- **`lnotifyd`** — daemon that owns the notification queue and rendering. Listens on a Unix socket. Detects the active session type and selects the appropriate rendering backend. Monitors VT switches and re-routes notifications accordingly.
- **`lnotify`** — thin CLI client. Connects to the daemon's Unix socket, sends a notification, returns immediately (fire-and-forget).
- **D-Bus listener** (optional, inside daemon) — implements `org.freedesktop.Notifications` spec so existing tools (`notify-send`, etc.) work. Only activates if D-Bus is available. Loaded via `dlopen`.

## Language & Build

- **C** — chosen for natural framebuffer/ioctl affinity, zero runtime deps, smallest binary.
- **Font rendering** — FreeType (C-native). Cross-compilation for ARM requires FreeType cross-compiled too.
- **Target architectures** — x86_64 + ARM (Raspberry Pi). Both are first-class.
- **Dynamic loading** — `dlopen` for `libwayland-client`, `libxcb`, D-Bus libs. Single binary works everywhere; backends activate only if their libs exist at runtime.
- **License** — WTFPL.

## Daemon Modes

### Per-user mode (default)
- Runs as the user
- Socket at `$XDG_RUNTIME_DIR/lnotify.sock`
- Detects only its own sessions
- Framebuffer access via `video` group membership
- Config at `~/.config/lnotify/config`

### System mode (`lnotifyd --system`)
- Runs as root
- Socket at `/run/lnotify.sock`
- Full cross-session visibility
- Maps active VT to session owner, delegates to appropriate backend
- When rendering on another user's session, runs D-Bus calls as that user (`sudo -u`)
- Config at `/etc/lnotify.conf` (per-user config can override)

## Notification Data Model

```
id:        uint32   (daemon-assigned, monotonically increasing)
title:     string   (optional, short bold header)
body:      string   (required, main message text)
priority:  uint8    (0=low, 1=normal, 2=critical; v1 treats all the same)
timeout:   int32    (milliseconds, -1 = use config default)
timestamp: uint64   (daemon-assigned on receipt)
```

Wire format: length-prefixed header with field lengths, then string payloads. No serialization library.

## Configuration

Key=value format (flat file). Path to INI sections later if needed.

```ini
default_timeout = 5000
position = top-right
font_path = /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
font_size = 14
bg_color = 28282800E6
fg_color = FFFFFFFF
border_color = 64A0FFFF
border_width = 2
border_radius = 12
padding = 20
margin = 30
```

All style values are configurable from the start — no magic numbers in rendering code. Config struct is populated at startup with defaults, passed throughout.

## Session Detection

### VT switch detection
- **Primary**: `select.poll()` / `poll()` on `/sys/class/tty/tty0/active` — fires immediately on VT switch, no ownership required, no compositor conflicts.
- **Fallback**: timed read of the same sysfs file (covers kernels where poll doesn't work on sysfs).
- **Enrichment**: query logind via D-Bus for session type + owner on the new VT.

### Session-to-backend mapping
On each VT switch:
1. Read `/sys/class/tty/tty0/active` to get the active VT
2. Query logind (`loginctl show-session`) for session on that VT — get UID, session type, username
3. Look up the session type in the engine config table
4. Dispatch to the appropriate backend

### VT-to-session table
The daemon maintains a mapping of VTs to sessions, rebuilt on each VT switch by querying logind. Handles:
- Multiple Wayland sessions on different VTs (even same user)
- GDM greeter on its own VT
- Raw TTYs with no session
- Sessions appearing/disappearing (compositor launch/teardown)

## Rendering Backend Architecture

### Engine config (per session type)
Each engine declares two static properties:

```c
struct engine_config {
    bool has_dbus;                    // session has a notification server
    bool conflicts_with_framebuffer;  // compositor bypasses /dev/fb0
};
```

Known engines:

| Session type | has_dbus | conflicts_with_framebuffer |
|-------------|----------|---------------------------|
| wayland     | true     | true                      |
| x11         | true     | true                      |
| (raw tty)   | false    | false                     |

### Fallback chain

For each notification on the active VT:

```
1. ENGINE BACKEND (if GUI session detected):
   - Look up session type in engine table
   - If engine has D-Bus: send org.freedesktop.Notifications.Notify
     via the session's D-Bus (running as the session owner)
   - Retry with exponential backoff (200ms base, 1.5x, max 5 attempts)
   - Handles session bus not ready yet (compositor still launching)

2. FRAMEBUFFER (if engine doesn't conflict with fb0):
   - Write toast directly to /dev/fb0 (mmap + pixel writes)
   - Read-after-write verification at +16ms and +50ms
   - Sustained defense: re-check every 200ms for DURATION/2
   - Re-render if clobbered; give up after 3 consecutive failures
   - Background defense thread continues for remaining notification lifetime

3. QUEUE (last resort):
   - Notification stored in queue with remaining timeout
   - On next VT switch, drain queue through the full fallback chain
   - Expired notifications are dropped on dequeue
```

### Why D-Bus for GUI sessions (not custom Wayland/X11 windows)
- Wayland compositors have full authority to suppress foreign windows
- GDM's GNOME Shell accepts Wayland client connections and reports frame renders but never actually displays the window (PHANTOM rendering)
- D-Bus `org.freedesktop.Notifications` is the compositor's own notification mechanism — it cooperates by design
- Root cannot directly connect to another user's D-Bus session bus; must run as target user via `sudo -u {username} env DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{UID}/bus gdbus call --session ...`

### Framebuffer rendering
- 32bpp BGRA pixel format (read from `ioctl FBIOGET_VSCREENINFO`)
- Rounded rectangle toast with configurable colors, border, padding
- Bitmap font for v1, FreeType for production
- Save/restore region underneath toast for clean dismissal
- Verification samples border pixels (distinctive color, 20 sample points, >80% match threshold)
- Modern compositors (Wayland, X11 with DRM/KMS) do NOT write to `/dev/fb0` — the framebuffer is only the actual display on raw TTYs

### Toast appearance
- Rounded rectangle, dark background, colored border
- Title (bold, larger) + body (regular, smaller)
- Position: top-right by default (configurable)
- Timeout: configurable per-notification, default from config
- Translucency: alpha channel in bg_color; honored by Wayland/X11, ignored by framebuffer

## IPC Protocol

Client → daemon over Unix socket:
- `$XDG_RUNTIME_DIR/lnotify.sock` (per-user)
- `/run/lnotify.sock` (system mode)

Simple length-prefixed binary messages. No serialization library.

## Future Design Points (v1 deferred, interfaces designed for)

- **Notification stacking** — queue is already a list; render function takes a notification list (v1 passes one). Adding stacking = "render N from the front."
- **Priority levels** — field exists in notification struct (v1 ignores). Future: style different priorities differently, persistence for critical, etc.
- **Notification persistence** — critical notifications that persist until dismissed.
- **X11 backend** — `_try_x11` stub exists in dispatch table. Implementation via `dlopen("libxcb.so")`.
- **SSH notification delivery** — mentioned but not yet designed.

## Open Questions / Needs Further Discussion

- **GDM greeter notifications**: GDM's GNOME Shell does not register `org.freedesktop.Notifications` on its session bus. Currently these get queued. Possible future approaches: DRM overlay planes (hardware-dependent), custom GDM extension, or accept the queue behavior.
- **Multiple monitors**: not discussed. Toast position per-monitor or primary only?
- **Notification actions** (buttons, click-to-dismiss): not discussed. D-Bus spec supports actions; framebuffer would need input handling.
- **Sound/urgency hints**: D-Bus spec supports hints dict; not yet designed.
- **Config hot-reload**: daemon re-reads config on SIGHUP? Or only on restart?
