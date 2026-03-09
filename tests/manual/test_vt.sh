#!/bin/bash
# VT switch monitor manual test
# Tests the daemon's poll()-based event loop: VT switch detection,
# session context rebuilds, engine dispatch, and queue drain.
#
# Prerequisites:
#   - Root access (for chvt and sysfs access)
#   - Multiple VTs available
#   - Build: make
#
# Test procedure:
#
# 1. Start daemon in debug mode (root for system mode VT monitoring):
#      sudo ./build/lnotifyd --system --debug 2>&1 | tee /tmp/lnotifyd.log &
#
# 2. From another terminal, send a notification:
#      ./build/lnotify -t "Test 1" "Hello from VT test"
#
#    Expected: daemon logs engine selection and renders the notification.
#    If on a GUI VT, D-Bus engine should accept. If on a raw TTY,
#    framebuffer engine should accept.
#
# 3. Switch VTs and observe daemon logs:
#      sudo chvt 2    # switch to tty2
#      sudo chvt 3    # switch to tty3 (often GNOME Wayland)
#      sudo chvt 1    # switch to tty1 (often GDM greeter)
#
#    Expected for each switch:
#      - "VT switch: N -> M" log line
#      - "dismissing active engine" (if one was rendering)
#      - context_init_from_logind output showing session properties
#      - Queue drain attempt (if any queued notifications)
#
# 4. Queue drain test:
#    a. Switch to a raw TTY without a session (e.g., tty4):
#         sudo chvt 4
#    b. Send a notification:
#         ./build/lnotify -t "Queued" "This should be queued"
#    c. Check logs — should show "notification queued" (no engine accepts
#       if no session is active on that VT)
#    d. Switch back to your session:
#         sudo chvt 3
#    e. Check logs — should show "draining queued notification" and
#       engine selection for the queued notification
#
# 5. Expired notification test:
#    a. Switch to an empty VT:
#         sudo chvt 4
#    b. Send a notification with a short timeout:
#         ./build/lnotify -t "Expiring" "Should expire" --timeout 2000
#    c. Wait 3+ seconds
#    d. Switch back:
#         sudo chvt 3
#    e. Check logs — the expired notification should be silently dropped
#       during queue drain (queue_pop_live skips expired entries)
#
# 6. Clean shutdown:
#    Send SIGINT or SIGTERM to the daemon:
#      sudo kill -INT $(pgrep lnotifyd)
#
#    Expected: logs show "lnotifyd exiting", socket file removed
#
# Monitoring tip:
#   Watch the daemon log in real-time:
#     tail -f /tmp/lnotifyd.log
