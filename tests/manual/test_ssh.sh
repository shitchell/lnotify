#!/bin/bash
# SSH terminal engine manual test
# Prerequisites: A running sshd, ability to ssh to localhost
#
# ============================================================================
# Test 1: Localhost SSH — full fallback chain
# ============================================================================
#
# Setup:
#   Terminal 1:
#     ./build/lnotifyd --debug
#
#   Terminal 2 (ssh into localhost):
#     ssh localhost
#     # Note your pts device: tty
#
#   Terminal 3 (send notification):
#     ./build/lnotify -t "SSH Test" "Hello from lnotify"
#
# Expected:
#   - lnotifyd logs: "ssh: found N qualifying SSH session(s)"
#   - One of the rendering tiers succeeds (depends on your terminal)
#   - Check the tier in logs: "ssh: OSC 9 sent" / "ssh: tmux display-popup"
#     / "ssh: cursor overlay" / "ssh: plain text"
#
# ============================================================================
# Test 2: tmux tier
# ============================================================================
#
# Setup:
#   Terminal 1:
#     ./build/lnotifyd --debug
#
#   Terminal 2:
#     ssh localhost
#     tmux
#     # You're now in an SSH session inside tmux
#
#   Terminal 3:
#     ./build/lnotify -t "tmux Test" "Should appear in tmux"
#
# Expected:
#   - tmux display-popup (tmux >= 3.2) or display-message appears
#   - Logs show: "ssh: tmux display-popup sent" or "ssh: tmux display-message sent"
#
# ============================================================================
# Test 3: Cursor overlay tier
# ============================================================================
#
# Setup:
#   Terminal 1:
#     ./build/lnotifyd --debug
#
#   Terminal 2:
#     ssh localhost
#     export LNOTIFY_SSH=overlay
#     # Force overlay-only mode
#
#   Terminal 3:
#     ./build/lnotify -t "Overlay" "Top-right box"
#
# Expected:
#   - An ANSI-colored box appears at the top-right of the SSH terminal
#   - After timeout (default 5s), the box clears
#   - Logs show: "ssh: cursor overlay written"
#
# Note: LNOTIFY_SSH must be forwarded by sshd. Add to /etc/ssh/sshd_config:
#   AcceptEnv LNOTIFY_SSH
# And in your ssh client config (~/.ssh/config):
#   SendEnv LNOTIFY_SSH
#
# ============================================================================
# Test 4: Plain text tier (forced)
# ============================================================================
#
# Setup:
#   Terminal 1:
#     ./build/lnotifyd --debug
#
#   Terminal 2:
#     ssh localhost
#     export LNOTIFY_SSH=text
#
#   Terminal 3:
#     ./build/lnotify -t "Plain" "Text only notification"
#
# Expected:
#   - A styled line appears in the SSH terminal:
#     [blue bg] lnotify [reset] Plain: Text only notification
#   - Logs show: "ssh: plain text notification written"
#
# ============================================================================
# Test 5: Opt-out (LNOTIFY_SSH=none)
# ============================================================================
#
# Setup:
#   Terminal 1:
#     ./build/lnotifyd --debug
#
#   Terminal 2:
#     ssh localhost
#     export LNOTIFY_SSH=none
#
#   Terminal 3:
#     ./build/lnotify -t "Opt-out" "Should not appear"
#
# Expected:
#   - No notification in the SSH terminal
#   - Logs show: "ssh: session X opted out (LNOTIFY_SSH=none)"
#
# ============================================================================
# Test 6: Fullscreen app detection
# ============================================================================
#
# Setup:
#   Terminal 1:
#     ./build/lnotifyd --debug
#
#   Terminal 2:
#     ssh localhost
#     vim     # or any app in ssh_fullscreen_apps
#
#   Terminal 3:
#     ./build/lnotify -t "Fullscreen" "Should be skipped"
#
# Expected:
#   - No notification interrupts vim
#   - Logs show: "ssh: fullscreen app detected: vim"
#   - Logs show: "ssh: skipping ... (fullscreen app)"
#
# ============================================================================
# Test 7: Capture with `script` command
# ============================================================================
#
# This captures the raw terminal output for inspection.
#
# Setup:
#   Terminal 2:
#     ssh localhost
#     script /tmp/lnotify-ssh-capture.log
#     # Now send notifications from Terminal 3
#     # When done:
#     exit    # exits script recording
#     cat /tmp/lnotify-ssh-capture.log | cat -v
#     # Look for escape sequences (^[)
#
# ============================================================================
# Test 8: User filtering (system mode)
# ============================================================================
#
# Setup:
#   Create config: /etc/lnotify.conf with:
#     ssh_users = yourusername
#
#   Terminal 1 (as root):
#     sudo ./build/lnotifyd --system --debug
#
#   Terminal 2:
#     ssh localhost     # as your user
#
#   Terminal 3:
#     ./build/lnotify --system -t "Filtered" "Only for allowed users"
#
# Expected:
#   - Notification appears in qualifying SSH session
#   - SSH sessions from non-listed users are skipped
#

echo "This is a manual test. Follow the instructions above."
echo ""
echo "Quick smoke test (requires ssh to localhost):"
echo ""
echo "  # Terminal 1: Start daemon"
echo "  ./build/lnotifyd --debug"
echo ""
echo "  # Terminal 2: SSH in"
echo "  ssh localhost"
echo ""
echo "  # Terminal 3: Send notification"
echo "  ./build/lnotify -t 'SSH Test' 'Hello from lnotify'"
echo ""
echo "Check daemon logs for SSH delivery details."
