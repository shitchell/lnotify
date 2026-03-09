#!/bin/bash
# Framebuffer engine manual test
# Prerequisites: root access, a raw TTY (not under a compositor)
#
# Steps:
# 1. Build: make daemon
# 2. Switch to a raw TTY: sudo chvt 2
# 3. On tty2, run: sudo ./build/lnotifyd --debug &
# 4. Trigger a notification (once client exists, or call engine directly via test binary)
# 5. Capture: sudo fbgrab /tmp/fb_toast.png
# 6. Switch back: sudo chvt 3 (or your GUI VT)
# 7. Inspect /tmp/fb_toast.png
#
# Expected:
# - Toast appears in configured position (default: top-right)
# - Rounded corners, border, colors match config defaults
# - Text is readable (8x8 bitmap font)
#
# To save as golden:
# cp /tmp/fb_toast.png tests/golden/fb/toast_default.png

echo "This is a manual test. Follow the instructions in the comments above."
echo "Cannot be run automatically — requires a raw TTY with framebuffer access."
