#!/bin/bash
# Headless test of wp_fifo_v1: updates with the barrier wait for the refresh, one
# per refresh, and nothing is held after the wp_fifo_v1 goes; the protocol errors.
# Output goes to build/fifo.log. Exits non-zero on failure.
LOG=build/fifo.log
. "$(dirname "$0")/lib.sh"

for tool in build/fifoc build/powerc build/workspacec; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done
fifoc() { WAYLAND_DISPLAY=wayland-0 timeout 10 build/fifoc "$@" 2>&1; }
# libwayland reports a protocol error on stderr itself: that goes to the log.
fifoc_quiet() { WAYLAND_DISPLAY=wayland-0 timeout 10 build/fifoc "$@" 2>>"$LOG"; }

# refreshes_between <description> <min> <max> <fifoc args...>
refreshes_between() {
	local what=$1 min=$2 max=$3 out r
	shift 3
	out=$(fifoc "$@") || fail "$what: fifoc $*: $out"
	echo "fifoc $*: $out" >>"$LOG"
	r=$(sed -n 's/^refreshes=\([0-9.]*\).*/\1/p' <<<"$out")
	[ -n "$r" ] || fail "$what: no result from fifoc: $out"
	awk -v r="$r" -v lo="$min" -v hi="$max" 'BEGIN { exit !(r >= lo && r <= hi) }' ||
		fail "$what: $r refreshes, expected $min to $max"
	echo "ok: $what ($r refreshes)"
}

# seconds_between <description> <min> <max> <fifoc args...>
seconds_between() {
	local what=$1 min=$2 max=$3 out t
	shift 3
	out=$(fifoc "$@") || fail "$what: fifoc $*: $out"
	echo "fifoc $*: $out" >>"$LOG"
	t=$(sed -n 's/^seconds=\([0-9.]*\).*/\1/p' <<<"$out")
	[ -n "$t" ] || fail "$what: no result from fifoc: $out"
	awk -v t="$t" -v lo="$min" -v hi="$max" 'BEGIN { exit !(t >= lo && t <= hi) }' ||
		fail "$what: $t s, expected $min to $max"
	echo "ok: $what ($t s)"
}

export MWC_TEST_STATE=$SMOKE_DIR/state # for the unplug hook
start_mwc 'exec sleep 300'

# 30 updates sent at once. Without the barrier the last replaces the others and all
# callbacks come with the next frame or the one after; with it each update gets a
# refresh of its own. The bounds leave room for a busy machine.
refreshes_between "updates without the barrier replace each other" 0 4 plain 30
refreshes_between "updates with the barrier come one per refresh" 25 45 fifo 30
refreshes_between "a second run in a new window too" 25 45 fifo 30
# Held, each update keeps a surface state in memory: past 64 the barrier clears early.
refreshes_between "more than 64 at once don't all wait" 0 120 fifo 300
# The wp_fifo_v1 goes after the first: the held ones must come at once.
refreshes_between "destroying the wp_fifo_v1 lets the held updates go" 0 6 release 30

# Nobody sees it: about one update a second, as Xwayland does, not every refresh (a
# hidden game at full speed) nor never (one stuck for good). Five updates: from the
# first until the fourth is through, three to four seconds. Hidden by mwc it keeps its
# output in wlroots, so mwc goes by its own state there.
seconds_between "a minimized window gets about one update a second" 2.5 6 minimized 5
# Its swapchain on a subsurface of its own: mwc goes by the window it belongs to.
export FIFOC_SUB=1
seconds_between "so does one with its updates on a subsurface" 2.5 6 minimized 5
unset FIFOC_SUB
# On a popup of a popup of the window (no grab, so it stays when the window goes).
export FIFOC_POPUP=1
seconds_between "so does one with its updates on a popup of a popup" 2.5 6 minimized 5
unset FIFOC_POPUP
# On another desktop: the window is mapped here, then the desktop goes.
rm -f "$SMOKE_DIR/go"
FIFOC_GO=$SMOKE_DIR/go WAYLAND_DISPLAY=wayland-0 timeout 15 build/fifoc cued 5 \
	>"$SMOKE_DIR/cued.out" 2>&1 &
cued=$!
wait_for "a window to hide" grep -q '^mapped' "$SMOKE_DIR/cued.out"
WAYLAND_DISPLAY=wayland-0 build/workspacec activate 2 >/dev/null || fail "desktop 2"
touch "$SMOKE_DIR/go"
wait "$cued" || fail "fifoc cued: $(cat "$SMOKE_DIR/cued.out")"
t=$(sed -n 's/^seconds=\([0-9.]*\).*/\1/p' "$SMOKE_DIR/cued.out")
awk -v t="${t:-0}" 'BEGIN { exit !(t >= 2.5 && t <= 6) }' ||
	fail "on another desktop: ${t:-no result} s, expected 2.5 to 6"
echo "ok: so does one on another desktop ($t s)"
WAYLAND_DISPLAY=wayland-0 build/workspacec activate 1 >/dev/null || fail "desktop 1"
WAYLAND_DISPLAY=wayland-0 build/powerc off || fail "powerc off"
seconds_between "so does one on a monitor switched off" 2.5 6 unseen 5
WAYLAND_DISPLAY=wayland-0 build/powerc on || fail "powerc on"
refreshes_between "on again, one per refresh" 25 45 fifo 30

# A client that goes while updates are held: wlroots drops them with the surface.
out=$(fifoc quit 30) || fail "fifoc quit: $out"
echo "fifoc quit 30: $out" >>"$LOG"
sleep 0.2
kill -0 "$pid" || fail "mwc died when a client left with held updates"
echo "ok: a client can leave with held updates"

out=$(fifoc_quiet twice) || fail "fifoc twice: $out"
[ "$out" = "error=wp_fifo_manager_v1:0" ] || fail "a second wp_fifo_v1: $out"
echo "ok: a second wp_fifo_v1 for a surface is already_exists"
out=$(fifoc_quiet gone) || fail "fifoc gone: $out"
[ "$out" = "error=wp_fifo_v1:0" ] || fail "set_barrier without a surface: $out"
echo "ok: set_barrier after the surface is gone is surface_destroyed"

# A monitor unplugged while a fullscreen window's updates wait for it: they go out
# during its removal and must not find the output mwc has already let go.
WAYLAND_DISPLAY=wayland-0 build/fifoc hold 60 >"$SMOKE_DIR/hold.out" 2>&1 &
hold=$!
wait_for "a fullscreen window holds updates" grep -q held "$SMOKE_DIR/hold.out"
kill -USR2 "$pid"
check "the monitor is gone" "! grep -q '^output_' \"\$MWC_TEST_STATE\""
kill -0 "$pid" || fail "mwc died when the monitor of held updates went"
echo "ok: a monitor can go while a fullscreen window's updates wait for it"
kill "$hold" 2>/dev/null
wait "$hold" 2>/dev/null

stop_mwc
echo "PASS: fifo"
