#!/bin/bash
# Headless smoke test: start mwc, connect a Wayland client (foot), take a
# screenshot, quit.
# Output goes to build/smoke.log and build/smoke.png. Exits non-zero on failure.
LOG=build/smoke.log
PNG=build/smoke.png
. "$(dirname "$0")/lib.sh"

require_tools foot grim

marker='touch "$SMOKE_DIR/$0"; exec sleep 120'
start_mwc "exec foot sh -c '$marker' foot-up"

wait_for "foot connected (Wayland)" test -e "$SMOKE_DIR/foot-up"

# Give the frames a moment to be drawn before the screenshot.
sleep 1
WAYLAND_DISPLAY=wayland-0 grim "$PNG" || fail "grim could not take a screenshot"
[ -s "$PNG" ] || fail "screenshot is empty"
stop_mwc

echo "PASS: screenshot in $PNG"
