#!/bin/bash
# Headless smoke test: start mwc, connect a Wayland client (foot) and an X11
# client (xterm via XWayland), take a screenshot, quit.
# Output goes to build/smoke.log and build/smoke.png. Exits non-zero on failure.
LOG=build/smoke.log
PNG=build/smoke.png
. "$(dirname "$0")/lib.sh"

require_tools foot xterm Xwayland grim

marker='touch "$SMOKE_DIR/$0"; exec sleep 120'
start_mwc "foot sh -c '$marker' foot-up & exec xterm -e sh -c '$marker' xterm-up"

wait_for "foot connected (Wayland)" test -e "$SMOKE_DIR/foot-up"
wait_for "xterm connected (XWayland)" test -e "$SMOKE_DIR/xterm-up"

# Give the frames a moment to be drawn before the screenshot.
sleep 1
WAYLAND_DISPLAY=wayland-0 grim "$PNG" || fail "grim could not take a screenshot"
[ -s "$PNG" ] || fail "screenshot is empty"
kill -0 "$pid" 2>/dev/null || fail "mwc died"

echo "PASS: screenshot in $PNG"
