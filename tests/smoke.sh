#!/bin/bash
# Headless smoke test: start mwc, connect a Wayland client (foot) and an X11
# client (xterm via XWayland), take a screenshot, quit.
# Output goes to build/smoke.log and build/smoke.png. Exits non-zero on failure.
set -u

MWC=./mwc
LOG=build/smoke.log
PNG=build/smoke.png
TIMEOUT=15

for tool in foot xterm Xwayland grim; do
	command -v "$tool" >/dev/null || { echo "FAIL: $tool not installed"; exit 1; }
done
[ -x "$MWC" ] || { echo "FAIL: $MWC not built"; exit 1; }

# Keep this path short: Wayland socket paths are limited to 108 bytes.
base=${XDG_RUNTIME_DIR:-/tmp}
SMOKE_DIR=$(mktemp -d "$base/mwc.XXXXXX") || exit 1
export SMOKE_DIR

pid=
cleanup() {
	if [ -n "$pid" ]; then
		kill "$pid" 2>/dev/null
		wait "$pid" 2>/dev/null
	fi
	rm -rf "$SMOKE_DIR"
}
trap cleanup EXIT

fail() {
	echo "FAIL: $1 (see $LOG)"
	exit 1
}

# wait_for <description> <test expression...>: poll until true or mwc died.
wait_for() {
	local what=$1 i
	shift
	for ((i = 0; i < TIMEOUT * 5; i++)); do
		"$@" && { echo "ok: $what"; return 0; }
		kill -0 "$pid" 2>/dev/null || fail "mwc died while waiting for $what"
		sleep 0.2
	done
	fail "timed out waiting for $what"
}

mkdir -p build
export XDG_RUNTIME_DIR=$SMOKE_DIR
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
unset WAYLAND_DISPLAY DISPLAY

marker='touch "$SMOKE_DIR/$0"; exec sleep 120'
"$MWC" -s "foot sh -c '$marker' foot-up & exec xterm -e sh -c '$marker' xterm-up" \
	>"$LOG" 2>&1 &
pid=$!

wait_for "Wayland socket" test -S "$SMOKE_DIR/wayland-0"
wait_for "foot connected (Wayland)" test -e "$SMOKE_DIR/foot-up"
wait_for "xterm connected (XWayland)" test -e "$SMOKE_DIR/xterm-up"

# Give the frames a moment to be drawn before the screenshot.
sleep 1
WAYLAND_DISPLAY=wayland-0 grim "$PNG" || fail "grim could not take a screenshot"
[ -s "$PNG" ] || fail "screenshot is empty"
kill -0 "$pid" 2>/dev/null || fail "mwc died"

echo "PASS: screenshot in $PNG"
