#!/bin/bash
# D-Bus engine manual test
# Prerequisites: A Wayland or X11 session with a notification daemon
#                (GNOME, KDE, etc.)
#
# Test 1: Same-user delivery
# 1. On your GUI session: ./build/lnotifyd --debug
# 2. Send notification (once client exists): ./build/lnotify -t "Test" "Hello from D-Bus"
# 3. Observe: desktop notification appears
# 4. Check logs: "D-Bus notify succeeded"
#
# Test 2: Cross-user delivery (requires root)
# 1. As root: sudo ./build/lnotifyd --system --debug
# 2. Trigger notification targeting your user's session
# 3. Observe: notification appears on your desktop
# 4. Check logs: "fork + setresuid to uid=1000"
#
# Test 3: Quick smoke test (gdbus directly)
# Run this on a GUI session to verify gdbus works:
#   gdbus call --session \
#     --dest org.freedesktop.Notifications \
#     --object-path /org/freedesktop/Notifications \
#     --method org.freedesktop.Notifications.Notify \
#     "lnotify-test" 0 "" "Test Title" "Test Body" "[]" "{}" 5000
#
# Expected: a desktop notification appears with "Test Title" / "Test Body"
#
# Test 4: Retry behavior
# 1. Start lnotifyd before the compositor is fully loaded (e.g., in .xinitrc)
# 2. Send a notification immediately
# 3. Check logs: retry messages with exponential backoff
# 4. Notification should eventually appear once the notification server starts

echo "This is a manual test. Follow the instructions above."
echo ""
echo "Quick smoke test (run on a GUI session):"
echo "  gdbus call --session \\"
echo "    --dest org.freedesktop.Notifications \\"
echo "    --object-path /org/freedesktop/Notifications \\"
echo "    --method org.freedesktop.Notifications.Notify \\"
echo "    'lnotify-test' 0 '' 'Test Title' 'Test Body' '[]' '{}' 5000"
