---
title: Lessons Learned
description: Empirical findings from prototyping — VT detection, framebuffer behavior, Wayland PHANTOM rendering, D-Bus cross-user access, threading bugs.
when: Implementing engines, debugging framebuffer or D-Bus issues, understanding why a design choice was made, encountering unexpected behavior (update this doc with new findings)
tags: [empirical, prototyping, framebuffer, dbus, wayland, threading]
---

# lnotify Lessons Learned

Discoveries from prototyping the VT-aware notification system. These are empirical findings from testing on a PureOS/Debian system running GNOME Wayland with GDM.

## VT Switch Detection

### What works
- **`poll()` on `/sys/class/tty/tty0/active`** fires immediately on every VT switch. No sudo required. No ownership of the TTY needed. Zero latency. This is the winner.
- **Timed read** of the same file also works as a fallback, catching every switch at the polling interval.
- **logind D-Bus signals** (`PropertiesChanged` on `org.freedesktop.login1.Seat`) fire on every switch with rich session data. ~50ms delay but gives session ID directly.
- **Session-to-VT mapping** via `loginctl show-session` reliably reports VTNr, Type (wayland/tty), Name, Active, and User for each session.

### What doesn't work
- **`loginctl show-session self`** doesn't update when your session becomes inactive (you're not on that VT to run the poll).
- **VT signals via `ioctl(VT_SETMODE)`** require owning the console fd. Only one process can hold `VT_SETMODE` per VT, and the compositor already claims it. Not viable for a notification daemon.

### Surprising findings
- **GDM's greeter runs its own Wayland session** on tty1 as user `Debian-gdm` (uid 123). logind reports it as `Type=wayland`. This is effectively the alice/bob multi-session scenario.
- **GDM's greeter is torn down** when you switch to your GNOME session (tty3), and relaunched when you switch back to tty1. The `/run/user/123/` directory appears and disappears.
- **Switching between two raw VTs** does not trigger GDM teardown. Only switching to another Wayland session does.

## Framebuffer (`/dev/fb0`)

### What works
- **Direct pixel writes on raw TTYs** — mmap + write works perfectly. Toast appears, verification confirms it, dismiss restores the region underneath.
- **Read-after-write verification** — sampling border pixels and comparing at +16ms/+50ms reliably detects if something overwrote our toast on raw TTYs.
- **Sustained defense** — re-checking every 200ms and re-rendering when clobbered works for the "compositor is launching" transition period. Gives up after 3 consecutive failures.

### Critical discovery: compositors don't use /dev/fb0
- **Modern Wayland compositors (GNOME Shell, GDM) use DRM/KMS directly** and completely bypass the legacy framebuffer device.
- **Capturing `/dev/fb0` while on a Wayland VT shows stale TTY content** — in our test, being on tty3 (GNOME Wayland) showed tty2's login prompt in the framebuffer.
- **This means framebuffer writes are invisible on compositor VTs.** The pixels go into a buffer nobody looks at.
- **Framebuffer verification passes falsely on compositor VTs** because nobody is overwriting our pixels — they're just in an orphaned buffer.
- **Implication**: framebuffer is ONLY a valid rendering path on raw TTYs (no compositor). Each engine must declare `conflicts_with_framebuffer` so the fallback chain knows to skip fb.

### Brief flash on VT switch to compositor VT
- When switching to tty1 (before GDM's greeter loads), there's a brief moment where the kernel exposes the raw framebuffer before the compositor takes over DRM.
- During this window, framebuffer writes ARE visible (for a fraction of a second).
- GDM's startup then overwrites them. Our sustained defense catches the clobber and re-renders, but GDM quickly wins.

## Wayland Rendering

### GTK4 window approach — doesn't work for foreign sessions
- **Connecting to another user's Wayland socket** works at the protocol level — GTK4 creates the window, the compositor accepts it.
- **`GdkFrameClock.after-paint` reports frame rendered** even when the compositor suppresses the window. GDM's GNOME Shell composites the frame but doesn't give our window visible screen real estate.
- **No way to detect suppression from inside the Wayland client.** The compositor has full authority and gives no feedback about actual visibility.
- **The "PHANTOM" problem**: frame callback fires (compositor did its job), but the window is invisible. Cross-checking the framebuffer also fails because the compositor doesn't write to fb0.

### GNOME's "python3 is ready" notification
- When a GTK app launches without a `.desktop` file, GNOME shows its own "ready" notification. This is GNOME's app tracking, not our toast.

## D-Bus Notifications

### What works
- **`org.freedesktop.Notifications.Notify`** via the session's D-Bus is the correct approach for GUI sessions. The compositor renders notifications through its own UI.
- **Root cannot directly connect** to another user's session bus. D-Bus enforces access control beyond file permissions. The error is "The connection is closed."
- **Fix**: run `gdbus` as the target user: `sudo -u {username} env DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{UID}/bus gdbus call --session ...`
- **`gdbus call --address`** also fails — must use `--session` with `DBUS_SESSION_BUS_ADDRESS` env var set.
- **Notification server on GNOME** reports: gnome-shell, GNOME, 3.38.6, spec 1.2. Supports: actions, body, body-markup, icon-static, persistence, sound.

### GDM's greeter has no notification server
- GDM's session bus exists (`/run/user/123/bus`) but `org.freedesktop.Notifications` is not registered.
- Error: `org.freedesktop.DBus.Error.ServiceUnknown: The name org.freedesktop.Notifications was not provided by any .service files`
- The greeter is a locked-down GNOME Shell that only shows the login UI.
- **This is currently an unsolvable gap** — no D-Bus notifications, no Wayland window rendering, framebuffer is invisible. Notifications to GDM's greeter get queued and delivered when the user returns to their own session.

## Queue Behavior

- **Queue drain on VT switch** works correctly — when switching from tty1 (greeter, queued) to tty3 (GNOME), the queued notification is delivered via D-Bus.
- **Expiration during queue** is handled — if the notification's timeout elapsed while queued, it's dropped on dequeue rather than shown stale.
- **Re-attempt through full fallback chain** on dequeue — the queued notification doesn't assume which backend to use, it re-evaluates the current VT.

## Threading

- **Background defense thread calling `dismiss_current()`** caused "cannot join current thread" crash. The thread tried to `join()` itself via `_cancel_timers()`.
- **Fix**: check `threading.current_thread()` before joining, and have the defense thread do its own cleanup directly with the lock instead of calling through `dismiss_current()`.
