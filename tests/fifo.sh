#!/bin/bash
# Headless test of wp_fifo_v1: updates with the barrier wait for the refresh, one
# per refresh, and nothing is held after the wp_fifo_v1 goes; the protocol errors.
# Output goes to build/fifo.log. Exits non-zero on failure.
LOG=build/fifo.log
. "$(dirname "$0")/lib.sh"

for tool in build/fifoc build/powerc build/workspacec build/vinput build/xdgc; do
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
# All of it under an opaque window: wlroots 0.20 keeps the surface on its output
# then (no wl_surface.leave), but the scene buffer is on none.
rm -f "$SMOKE_DIR/go"
FIFOC_GO=$SMOKE_DIR/go WAYLAND_DISPLAY=wayland-0 timeout 15 build/fifoc cued 5 \
	>"$SMOKE_DIR/cued.out" 2>&1 &
cued=$!
wait_for "a window to cover" grep -q '^mapped' "$SMOKE_DIR/cued.out"
WAYLAND_DISPLAY=wayland-0 build/xdgc app=cover pre=full opaque=1 \
	>"$SMOKE_DIR/cover.events" 2>&1 &
cover=$!
check "an opaque window covers it" "[ \"\$(get cover full)\" = 1 ]"
touch "$SMOKE_DIR/go"
wait "$cued" || fail "fifoc cued: $(cat "$SMOKE_DIR/cued.out")"
t=$(sed -n 's/^seconds=\([0-9.]*\).*/\1/p' "$SMOKE_DIR/cued.out")
awk -v t="${t:-0}" 'BEGIN { exit !(t >= 2.5 && t <= 6) }' ||
	fail "under an opaque window: ${t:-no result} s, expected 2.5 to 6"
echo "ok: so does one under an opaque window ($t s)"
kill "$cover"
wait "$cover" 2>/dev/null
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

# Across two monitors it goes by the one most of it is on, as wlroots' frame callbacks
# do: a strip on a 60 Hz monitor doesn't slow a game on a 144 Hz one, nor does a strip
# on the 144 Hz one speed it up. Moved there last, the 60 Hz one is the first of its
# outputs in wlroots.
export WLR_HEADLESS_OUTPUTS=2
RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
printf '%s\n' 'Mwm*HEADLESS-1*outputMode: 1280x720@60' 'Mwm*HEADLESS-1*outputPosition: 0,0' \
	'Mwm*HEADLESS-2*outputMode: 1280x720@144' 'Mwm*HEADLESS-2*outputPosition: 1280,0' \
	'Mwm*moveOpaque: true' >"$RC"
start_mwc 'exec sleep 300'
start_vinput 2560 720
# across <description> <x> <min> <max>: a window mapped on the 144 Hz monitor, moved to
# x across the edge at 1280, 30 updates in min to max seconds.
across() {
	local what=$1 x=$2 min=$3 max=$4 t
	do_input move 1900 300
	rm -f "$SMOKE_DIR/go"
	FIFOC_GO=$SMOKE_DIR/go WAYLAND_DISPLAY=wayland-0 timeout 15 build/fifoc cued 30 \
		>"$SMOKE_DIR/across.out" 2>&1 &
	PID_across=$!
	wait_for "a window on the 144 Hz monitor" grep -q '^mapped' "$SMOKE_DIR/across.out"
	check "it is there" "[ -n \"\$(get fifoc x)\" ] && [ \"\$(get fifoc x)\" -ge 1280 ]"
	# Alt+drag on the client: the title of so small a window is mostly buttons. The
	# pointer ends away from the monitors' edges, where the window would snap.
	local tx=$(($(get fifoc x) + 20)) ty=$(($(get fifoc y) + $(get fifoc bw) + $(get fifoc th) + 30))
	do_input move "$tx" "$ty"
	do_input hold alt
	do_input down left
	do_input move $((tx - $(get fifoc x) + x)) "$ty"
	do_input up left
	do_input release alt
	check "$what: across the edge" "[ \"\$(get fifoc x)\" = $x ]"
	touch "$SMOKE_DIR/go"
	wait "$PID_across" || fail "fifoc across: $(cat "$SMOKE_DIR/across.out")"
	t=$(sed -n 's/^seconds=\([0-9.]*\).*/\1/p' "$SMOKE_DIR/across.out")
	awk -v t="${t:-9}" -v lo="$min" -v hi="$max" 'BEGIN { exit !(t >= lo && t <= hi) }' ||
		fail "$what: ${t:-no result} s for 30 updates, expected $min to $max"
	echo "ok: $what ($t s)"
}
# 28 refreshes: 0.19 s at 144 Hz, 0.47 s at 60. The frame is 76 wide.
across "mostly on the 144 Hz monitor, it goes at 144" 1250 0.1 0.33
across "mostly on the 60 Hz monitor, it goes at 60" 1220 0.36 0.9
stop_vinput
stop_mwc
unset WLR_HEADLESS_OUTPUTS
echo "PASS: fifo"
