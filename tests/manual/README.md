# Manual Tests

These tests require specific hardware or session environments that cannot be automated. Each test script contains detailed step-by-step instructions in its header comments.

## Prerequisites

All tests require a successful build: `make all`

## Test Scripts

### test_fb.sh — Framebuffer Engine

Tests direct framebuffer rendering on a raw TTY (no compositor).

**Requirements:**
- Root access
- A raw TTY (not under Wayland/X11)
- `/dev/fb0` writable

**Procedure:**
1. Build: `make daemon`
2. Switch to a raw TTY: `sudo chvt 2`
3. On tty2: `sudo ./build/lnotifyd --debug &`
4. From another terminal: `./build/lnotify -t "FB Test" "Hello framebuffer"`
5. Capture the screen: `sudo fbgrab /tmp/fb_toast.png`
6. Switch back: `sudo chvt 3` (or your GUI VT)
7. Inspect `/tmp/fb_toast.png`

**Expected output:**
- Toast notification visible in the captured screenshot
- Positioned at top-right (default config)
- Rounded corners, border, readable 8x8 bitmap font text
- Daemon logs: `dispatching via engine 'framebuffer'`

**Golden file capture:**
```bash
sudo fbgrab /tmp/fb_toast.png
cp /tmp/fb_toast.png tests/golden/fb/toast_default.png
```

### test_dbus.sh — D-Bus Engine

Tests notification delivery through the desktop notification daemon.

**Requirements:**
- A running Wayland or X11 session with a notification daemon (GNOME, KDE, etc.)

**Procedure — same-user:**
1. On your GUI session: `./build/lnotifyd --debug 2>&1 | tee /tmp/lnotifyd.log`
2. From another terminal: `./build/lnotify -t "D-Bus Test" "Hello from D-Bus"`
3. Observe: a desktop notification popup appears

**Expected output:**
- Desktop notification visible
- Daemon logs: `dispatching via engine 'dbus'`
- Daemon logs: `D-Bus notify succeeded` (or similar)

**Procedure — cross-user (system mode):**
1. As root: `sudo ./build/lnotifyd --system --debug 2>&1 | tee /tmp/lnotifyd.log`
2. Trigger notification targeting your user's session
3. Check logs for: `fork + setresuid to uid=<your_uid>`

**Quick smoke test (gdbus directly, no lnotify needed):**
```bash
gdbus call --session \
  --dest org.freedesktop.Notifications \
  --object-path /org/freedesktop/Notifications \
  --method org.freedesktop.Notifications.Notify \
  "lnotify-test" 0 "" "Test Title" "Test Body" "[]" "{}" 5000
```

### test_vt.sh — VT Switch Monitor

Tests the daemon's VT switch detection, session context rebuilds, engine dispatch, and queue drain.

**Requirements:**
- Root access
- Multiple VTs available
- A GUI session on one VT (typically tty3 for GNOME Wayland)

**Procedure:**
1. Start daemon in debug mode: `sudo ./build/lnotifyd --system --debug 2>&1 | tee /tmp/lnotifyd.log &`
2. Send a notification: `./build/lnotify -t "VT Test" "Hello"`
3. Switch VTs and observe logs:
   ```bash
   sudo chvt 2    # raw TTY
   sudo chvt 3    # GUI session
   sudo chvt 1    # GDM greeter
   ```

**Expected output per VT switch:**
- `VT switch: N -> M` log line
- `dismissing active engine` (if one was rendering)
- Context rebuild output showing session properties
- Queue drain attempt (if any queued notifications)

**Queue drain test:**
1. Switch to an empty VT: `sudo chvt 4`
2. Send notification: `./build/lnotify -t "Queued" "This should be queued"`
3. Verify log: `notification queued` or `no engine available`
4. Switch back to GUI: `sudo chvt 3`
5. Verify log: `draining queued notification` and engine selection

**Expired notification test:**
1. Switch to empty VT: `sudo chvt 4`
2. Send with short timeout: `./build/lnotify -t "Expiring" "Should expire" --timeout 2000`
3. Wait 3+ seconds
4. Switch back: `sudo chvt 3`
5. Verify: expired notification is silently dropped during drain (`queue_pop_live` skips it)

**Clean shutdown test:**
1. `sudo kill -INT $(pgrep lnotifyd)`
2. Verify log: `lnotifyd exiting`
3. Verify: socket file removed

### test_ssh.sh — SSH Terminal Engine

Tests SSH session discovery and 4-tier rendering fallback (OSC, tmux, cursor overlay, plain text).

**Requirements:**
- A running sshd
- Ability to ssh to localhost

**Test 1 — basic delivery:**
1. Terminal 1: `./build/lnotifyd --debug`
2. Terminal 2: `ssh localhost` (note your pts via `tty`)
3. Terminal 3: `./build/lnotify -t "SSH Test" "Hello from lnotify"`
4. Expected: notification appears in the SSH terminal
5. Check logs for tier used: `ssh: OSC 9 sent` / `ssh: tmux display-popup` / `ssh: cursor overlay` / `ssh: plain text`

**Test 2 — tmux tier:**
1. Start daemon, SSH in, then run `tmux` inside the SSH session
2. Send notification
3. Expected: tmux display-popup or display-message

**Test 3 — cursor overlay:**
1. SSH in, set `export LNOTIFY_SSH=overlay`
2. Send notification
3. Expected: ANSI-colored box at top-right, auto-dismisses after timeout

**Test 4 — plain text (forced):**
1. SSH in, set `export LNOTIFY_SSH=text`
2. Send notification
3. Expected: styled ANSI line (`[blue bg] lnotify [reset] Title: Body`)

**Test 5 — opt-out:**
1. SSH in, set `export LNOTIFY_SSH=none`
2. Send notification
3. Expected: no notification; log shows `ssh: session X opted out`

**Test 6 — fullscreen app detection:**
1. SSH in, run `vim`
2. Send notification
3. Expected: no notification interrupts vim; log shows `ssh: fullscreen app detected`

**Capture with `script` for inspection:**
```bash
# In the SSH session:
script /tmp/lnotify-ssh-capture.log
# ... send notifications from another terminal ...
exit
cat -v /tmp/lnotify-ssh-capture.log
# Look for escape sequences (^[)
```

**LNOTIFY_SSH environment variable setup:**

For `LNOTIFY_SSH` to be forwarded, configure sshd and client:
```
# /etc/ssh/sshd_config
AcceptEnv LNOTIFY_SSH

# ~/.ssh/config
SendEnv LNOTIFY_SSH
```

## Golden Files

Golden files are stored in `tests/golden/`. They are reference captures for visual comparison.

| Directory | Contents |
|-----------|----------|
| `tests/golden/fb/` | Framebuffer screenshot captures (PNG via `fbgrab`) |
| `tests/golden/terminal/` | Terminal output captures (via `script` command) |

To create a golden file:
1. Run the manual test and verify the output is correct
2. Copy the capture to the appropriate golden directory
3. Commit with a descriptive name (e.g., `toast_default.png`, `ssh_overlay_basic.log`)
