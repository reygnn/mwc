#!/bin/bash
# Headless test of interactive move and resize: the outline, the feedback box,
# keyboard control, Escape, and opaque dragging.
# Output goes to build/move.log. Exits non-zero on failure.
LOG=build/move.log
. "$(dirname "$0")/lib.sh"

require_tools grim
for tool in build/vinput build/pngtool build/xdgc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
MODE_MOVE=1
MODE_RESIZE=2

start_mwc 'exec sleep 300'
start_vinput
refresh
B=$(global theme border)
T=$(global theme title)

wayland() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$name" "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "PID_$name=$!"
	check "$name is mapped" "[ -n \"\$(get $name x)\" ]"
}
stop() {
	local var="PID_$1"
	kill "${!var}" 2>/dev/null
	wait "${!var}" 2>/dev/null
	check "$1 is gone" "[ -z \"\$(get $1 x)\" ]"
}
settle() { sleep 0.45; }

geometry_is() { # description app x y w h
	check "$1" "[ \"\$(get $2 x)\" = $3 ] && [ \"\$(get $2 y)\" = $4 ] && \
		[ \"\$(get $2 w)\" = $5 ] && [ \"\$(get $2 h)\" = $6 ]"
}
outline_is() { # description x y w h
	check "$1" "[ \"\$(global outline shown)\" = 1 ] && [ \"\$(global outline x)\" = $2 ] && \
		[ \"\$(global outline y)\" = $3 ] && [ \"\$(global outline w)\" = $4 ] && \
		[ \"\$(global outline h)\" = $5 ]"
}
no_grab_left() { # description
	check "$1" "[ \"\$(global cursor mode)\" = 0 ] && [ \"\$(global outline shown)\" = 0 ] && \
		[ -z \"\$(global feedback text)\" ]"
}
feedback_is() { # description text, with _ for spaces
	check "$1" "[ \"\$(global feedback text)\" = '$2' ]"
}

# ---- a window that has never been anything but maximized ------------------------

WAYLAND_DISPLAY=wayland-0 build/xdgc app=premaxed pre=max size=300x200 >/dev/null 2>&1 &
PID_premaxed=$!
check "it maps maximized" "[ \"\$(get premaxed max)\" = 1 ]"
settle
do_input move 640 $((B + T / 2))
do_input down left
do_input move 700 80
check "dragged off, the outline has a size, though the window has none of its own yet" \
	"[ \"\$(global outline shown)\" = 1 ] && [ \"\$(global outline w)\" -ge 300 ] && \
	[ \"\$(global outline h)\" -ge 200 ]"
do_input up left
check "dropped, it chooses its own" \
	"[ \"\$(get premaxed max)\" = 0 ] && [ \"\$(get premaxed w)\" = 300 ] && [ \"\$(get premaxed h)\" = 200 ]"
kill "$PID_premaxed"
wait "$PID_premaxed" 2>/dev/null
check "the window is gone" "[ -z \"\$(get premaxed x)\" ]"

# ---- moving with the outline --------------------------------------------------

wayland mover size=300x200
refresh
X=$(get mover x) Y=$(get mover y)
OW=$((300 + 2 * B)) OH=$((200 + 2 * B + T))
TX=$((X + 150)) TY=$((Y + B + T / 2))

settle
do_input move "$TX" "$TY"
do_input down left
do_input move $((TX + 2)) $((TY + 2))
check "a press and a little motion don't start a move" \
	"[ \"\$(global cursor mode)\" = $MODE_MOVE ] && [ \"\$(global outline shown)\" = 0 ]"
do_input move $((TX + 50)) $((TY + 30))
outline_is "the outline follows the pointer" $((X + 50)) $((Y + 30)) "$OW" "$OH"
geometry_is "the window stays where it is" mover "$X" "$Y" 300 200
feedback_is "the box shows the position" "+$((X + 50))_+$((Y + 30))"
pixel_is "the outline is drawn in the window's colour" $((X + 51)) $((Y + 100)) c6c1aa
pixel_is "the title bar line is drawn too" $((X + 200)) $((Y + 30 + B + T - 1)) c6c1aa
refresh
pixel_is "the box is drawn" $(($(global feedback x) + 2)) $(($(global feedback y) + 10)) aaaaaa
# A fast mouse: the box is drawn about once a refresh, not for every motion event,
# and shows where the window ends up.
draws=$(global feedback draws)
do_input glide $((TX + 250)) $((TY + 130)) 200
do_input glide $((TX + 50)) $((TY + 30)) 200
feedback_is "after a fast move the box shows the position" "+$((X + 50))_+$((Y + 30))"
refresh
[ $(($(global feedback draws) - draws)) -le 40 ] ||
	fail "400 motion events drew the box $(($(global feedback draws) - draws)) times"
echo "ok: 400 motion events drew the box $(($(global feedback draws) - draws)) times"
do_input up left
X=$((X + 50)) Y=$((Y + 30))
geometry_is "the window goes there when released" mover "$X" "$Y" 300 200
no_grab_left "nothing is left of the grab"

settle
TX=$((X + 150)) TY=$((Y + B + T / 2))
do_input move "$TX" "$TY"
do_input down left
do_input move $((TX + 2)) "$TY"
do_input up left
geometry_is "a small slip doesn't move the window" mover "$X" "$Y" 300 200

settle
do_input move "$TX" "$TY"
do_input down left
do_input move $((TX + 80)) $((TY + 40))
outline_is "dragging again" $((X + 80)) $((Y + 40)) "$OW" "$OH"
do_input key Escape
no_grab_left "Escape ends the drag"
geometry_is "and leaves the window where it was" mover "$X" "$Y" 300 200
do_input up left
geometry_is "the release doesn't change that" mover "$X" "$Y" 300 200

# ---- moving with the keyboard ---------------------------------------------------

do_input key alt+F7
check "Alt+F7 starts a move" "[ \"\$(global cursor mode)\" = $MODE_MOVE ]"
outline_is "and shows the outline at once" "$X" "$Y" "$OW" "$OH"
feedback_is "and the box" "+${X}_+${Y}"
do_input key Right
outline_is "an arrow key moves 10 px" $((X + 10)) "$Y" "$OW" "$OH"
do_input key Down
outline_is "in every direction" $((X + 10)) $((Y + 10)) "$OW" "$OH"
do_input key shift+Left
outline_is "Shift+arrow moves 1 px" $((X + 9)) $((Y + 10)) "$OW" "$OH"
do_input key shift+Up
outline_is "in every direction" $((X + 9)) $((Y + 9)) "$OW" "$OH"
do_input key a
check "other keys do nothing" "[ \"\$(global cursor mode)\" = $MODE_MOVE ]"
geometry_is "the window hasn't moved yet" mover "$X" "$Y" 300 200
do_input key Return
X=$((X + 9)) Y=$((Y + 9))
geometry_is "Return confirms" mover "$X" "$Y" 300 200
no_grab_left "nothing is left of the grab"

# Held down, an arrow repeats, as keys do for clients (600 ms, then 25 a second).
do_input key alt+F7
do_input press Right
check "a held arrow repeats" "[ \"\$(global outline x)\" -ge $((X + 50)) ]"
do_input lift Right
refresh
held_x=$(global outline x)
sleep 0.3
refresh
[ "$(global outline x)" = "$held_x" ] || fail "the repeat goes on after the key went up"
echo "ok: and stops when it goes up"
do_input key Escape
geometry_is "Escape: the window stays where it was" mover "$X" "$Y" 300 200

# Its own repeat follows mwmrc as the clients' does; with rate 0 a held arrow is one
# step. (A reload ends the move, so it can't change the rate in the middle of one.)
# Timed, not polled: the defaults would get anywhere in the end. Each reading counts
# from when its state was written, which a loaded machine may delay, so the times are
# measured around them rather than taken from the sleeps.
printf 'Mwm*keyboardRepeatDelay: 100\nMwm*keyboardRepeatRate: 50\n' >"$RC"
reload_mwc
do_input key alt+F7
ms() { echo $(($(date +%s%N) / 1000000)); }
t0=$(ms)
do_input press Right
sleep 0.3
t1a=$(ms)
refresh
t1=$(ms)
early=$(global outline x)
sleep 0.5
t2a=$(ms)
refresh
t2=$(ms)
late=$(global outline x)
do_input lift Right
# The default delay, 600 ms, has no repeat yet: a second step shows mwmrc's 100 ms, if the
# reading was taken in time to tell.
[ $((t1 - t0)) -lt 600 ] || fail "too slow to tell the delay: read $((t1 - t0)) ms after the press"
[ "$early" -gt $((X + 10)) ] ||
	fail "no repeat $((t1 - t0)) ms after the press: the delay isn't mwmrc's 100 ms"
echo "ok: a shorter delay ($((early - X)) px within $((t1 - t0)) ms)"
# Steps between the readings over the longest time they can have been apart: 50 a
# second gives about 50, the default 25 about 25.
# Late enough, the outline stops at the edge of the output and the steps run out.
[ $((t2 - t0)) -lt 1800 ] || fail "too slow to tell the rate: read $((t2 - t0)) ms after the press"
rate=$(((late - early) / 10 * 1000 / (t2 - t1a)))
[ "$rate" -ge 37 ] || fail "$rate steps a second between the readings: the rate isn't mwmrc's 50"
echo "ok: a faster repeat (at least $rate a second)"
do_input key Escape
printf 'Mwm*keyboardRepeatDelay: 100\nMwm*keyboardRepeatRate: 0\n' >"$RC"
reload_mwc
do_input key alt+F7
do_input press Right
outline_is "rate 0: one step" $((X + 10)) "$Y" "$OW" "$OH"
sleep 0.8 # past the default delay too, which would have shown a second step
outline_is "and no more" $((X + 10)) "$Y" "$OW" "$OH"
do_input lift Right
do_input key Escape
rm "$RC"
reload_mwc
geometry_is "the window stays where it was" mover "$X" "$Y" 300 200

do_input key alt+F7
do_input key Left
outline_is "a new move" $((X - 10)) "$Y" "$OW" "$OH"
do_input key Escape
geometry_is "Escape cancels" mover "$X" "$Y" 300 200
no_grab_left "nothing is left of the grab"

do_input key alt+space
do_input key m
check "Move in the window menu starts a move" "[ \"\$(global cursor mode)\" = $MODE_MOVE ]"
do_input key Escape

# ---- resizing -------------------------------------------------------------------

RX=$((X + OW - 2)) RY=$((Y + OH - 2))
do_input move "$RX" "$RY"
do_input down left
do_input move $((RX + 60)) $((RY + 40))
outline_is "resizing: the outline grows" "$X" "$Y" $((OW + 60)) $((OH + 40))
geometry_is "the window keeps its size meanwhile" mover "$X" "$Y" 300 200
feedback_is "the box shows the size" "360_x_240"
do_input up left
geometry_is "it takes the new size when released" mover "$X" "$Y" 360 240
no_grab_left "nothing is left of the grab"

do_input move $((X + 1)) $((Y + 1))
do_input down left
do_input move $((X + 1 + 40)) $((Y + 1 + 20))
outline_is "from the top left, the opposite corner stays" $((X + 40)) $((Y + 20)) \
	$((OW + 60 - 40)) $((OH + 40 - 20))
do_input key Escape
geometry_is "Escape cancels a resize" mover "$X" "$Y" 360 240
no_grab_left "nothing is left of the grab"
do_input up left

do_input key alt+F8
check "Alt+F8 starts a resize" "[ \"\$(global cursor mode)\" = $MODE_RESIZE ]"
outline_is "and shows the outline at once" "$X" "$Y" $((OW + 60)) $((OH + 40))
do_input key Right
do_input key shift+Down
outline_is "arrow keys resize as well" "$X" "$Y" $((OW + 70)) $((OH + 41))
do_input key Return
geometry_is "Return confirms" mover "$X" "$Y" 370 241
no_grab_left "nothing is left of the grab"
stop mover

# The axis not being dragged must keep its size.
wayland edgey size=325x215
refresh
X=$(get edgey x) Y=$(get edgey y)
do_input move $((X + 325 + 2 * B - 2)) $((Y + 100))
do_input down left
do_input move $((X + 325 + 2 * B + 60)) $((Y + 100))
do_input up left
check "the right edge only changes the width" \
	"[ \"\$(get edgey h)\" = 215 ] && [ \"\$(get edgey w)\" -gt 325 ]"
stop edgey

# ---- Wayland clients ------------------------------------------------------------

wayland wmover size=300x200
refresh
WX=$(get wmover x) WY=$(get wmover y)
settle
do_input move $((WX + 150)) $((WY + B + T / 2))
do_input down left
do_input move $((WX + 200)) $((WY + B + T / 2 + 30))
outline_is "Wayland: the outline follows the pointer" $((WX + 50)) $((WY + 30)) "$OW" "$OH"
geometry_is "Wayland: the window stays where it is" wmover "$WX" "$WY" 300 200
do_input up left
geometry_is "Wayland: it goes there when released" wmover $((WX + 50)) $((WY + 30)) 300 200
WX=$((WX + 50)) WY=$((WY + 30))

do_input move $((WX + 1)) $((WY + 1))
do_input down left
do_input move $((WX + 1 + 40)) $((WY + 1 + 20))
geometry_is "Wayland: no new size while dragging" wmover "$WX" "$WY" 300 200
do_input up left
geometry_is "Wayland: from the top left, the opposite corner stays" wmover \
	$((WX + 40)) $((WY + 20)) 260 180
WX=$((WX + 40)) WY=$((WY + 20))

do_input key alt+F8
do_input key Right
do_input key Escape
geometry_is "Wayland: Escape cancels a resize" wmover "$WX" "$WY" 260 180
stop wmover

# ---- a maximized window -----------------------------------------------------------

wayland maxed size=300x200
refresh
X=$(get maxed x) Y=$(get maxed y)
do_input key alt+F10
check "the window is maximized" "[ \"\$(get maxed max)\" = 1 ]"
settle
do_input move 640 $((B + T / 2))
do_input down left
do_input move 640 200
check "dragging it away restores it, the outline has the old size" \
	"[ \"\$(get maxed max)\" = 0 ] && [ \"\$(global outline w)\" = $OW ] && [ \"\$(global outline h)\" = $OH ]"
do_input key Escape
check "Escape maximizes it again" \
	"[ \"\$(get maxed max)\" = 1 ] && [ \"\$(get maxed x)\" = 0 ] && [ \"\$(get maxed w)\" = $((1280 - 2 * B)) ]"
no_grab_left "nothing is left of the grab"
do_input up left
do_input key alt+F10
geometry_is "and it still remembers its place" maxed "$X" "$Y" 300 200
stop maxed

# ---- the window goes away during a drag --------------------------------------------

wayland victim size=300x200
refresh
X=$(get victim x) Y=$(get victim y)
settle
do_input move $((X + 150)) $((Y + B + T / 2))
do_input down left
do_input move $((X + 200)) $((Y + 100))
outline_is "dragging a window" $((X + 50)) $((Y + 100 - B - T / 2)) "$OW" "$OH"
stop victim
no_grab_left "when it goes away, so does the grab"
do_input up left
kill -0 "$pid" || fail "mwc died"

# ---- opaque dragging --------------------------------------------------------------

echo 'Mwm*moveOpaque: true' >"$RC"
reload_mwc

wayland opaque size=300x200
refresh
X=$(get opaque x) Y=$(get opaque y)
settle
TX=$((X + 150)) TY=$((Y + B + T / 2))
do_input move "$TX" "$TY"
do_input down left
do_input move $((TX + 50)) $((TY + 30))
geometry_is "moveOpaque: the window itself follows the pointer" opaque \
	$((X + 50)) $((Y + 30)) 300 200
check "moveOpaque: no outline" "[ \"\$(global outline shown)\" = 0 ]"
feedback_is "moveOpaque: the box stays" "+$((X + 50))_+$((Y + 30))"
do_input key Escape
geometry_is "moveOpaque: Escape puts it back" opaque "$X" "$Y" 300 200
do_input up left
no_grab_left "nothing is left of the grab"

RX=$((X + OW - 2)) RY=$((Y + OH - 2))
do_input move "$RX" "$RY"
do_input down left
do_input move $((RX + 60)) $((RY + 40))
geometry_is "moveOpaque: the window itself grows" opaque "$X" "$Y" 360 240
do_input key Escape
geometry_is "moveOpaque: Escape restores the size" opaque "$X" "$Y" 300 200
do_input up left
stop opaque

# A reload in the middle of a resize: the outputs are as they were, so the grab goes on,
# with the size it may be waiting to send.
wayland wreload size=300x200
refresh
WX=$(get wreload x) WY=$(get wreload y)
do_input move $((WX + 1)) $((WY + 1))
do_input down left
do_input move $((WX + 11)) $((WY + 11))
geometry_is "moveOpaque, Wayland: resizing" wreload $((WX + 10)) $((WY + 10)) 290 190
reload_mwc
check "after a reload the grab goes on" "[ \"\$(global cursor mode)\" = $MODE_RESIZE ]"
do_input move $((WX + 41)) $((WY + 21))
geometry_is "moveOpaque, Wayland: it still follows the pointer" wreload \
	$((WX + 40)) $((WY + 20)) 260 180
do_input up left
stop wreload

wayland wopaque size=300x200
refresh
WX=$(get wopaque x) WY=$(get wopaque y)
do_input move $((WX + 1)) $((WY + 1))
do_input down left
do_input move $((WX + 41)) $((WY + 21))
geometry_is "moveOpaque, Wayland: the window resizes with the pointer" wopaque \
	$((WX + 40)) $((WY + 20)) 260 180
do_input key Escape
geometry_is "moveOpaque, Wayland: Escape restores it, corner and all" wopaque "$WX" "$WY" 300 200
do_input up left

# Faster than the client draws: sizes it hasn't caught up with wait, the last one counts.
do_input move $((WX + 1)) $((WY + 1))
do_input down left
for i in $(seq 1 30); do
	do_input move $((WX + 1 + 3 * i)) $((WY + 1 + 2 * i))
done
do_input up left
geometry_is "moveOpaque, Wayland: many steps end at the last one" wopaque \
	$((WX + 90)) $((WY + 60)) 210 140

# Mid-resize, a client that goes away is not to be told about the resize ending.
refresh
do_input move $(($(get wopaque x) + 1)) $(($(get wopaque y) + 1))
do_input down left
do_input move $(($(get wopaque x) + 41)) $(($(get wopaque y) + 21))
check "moveOpaque, Wayland: resizing again" "[ \"\$(global cursor mode)\" = $MODE_RESIZE ]"
stop wopaque
no_grab_left "moveOpaque, Wayland: when the client dies, so does the grab"
do_input up left
kill -0 "$pid" || fail "mwc died"
stop_vinput
stop_mwc

# ---- the setting ------------------------------------------------------------------

echo 'Mwm*moveOpaque: true' >"$RC"
export MWC_MOVE_OPAQUE=0
start_mwc 'exec sleep 300'
start_vinput
wayland envwins size=300x200
refresh
X=$(get envwins x) Y=$(get envwins y)
settle
do_input move $((X + 150)) $((Y + B + T / 2))
do_input down left
do_input move $((X + 200)) $((Y + 100))
check "MWC_MOVE_OPAQUE=0 beats the file" "[ \"\$(global outline shown)\" = 1 ]"
do_input key Escape
do_input up left
stop envwins

stop_vinput
stop_mwc

# Something that is no boolean is reported, and the file decides.
export MWC_MOVE_OPAQUE=maybe
start_mwc 'exec sleep 300'
start_vinput
wayland nonsense size=300x200
refresh
X=$(get nonsense x) Y=$(get nonsense y)
settle
do_input move $((X + 150)) $((Y + B + T / 2))
do_input down left
do_input move $((X + 200)) $((Y + 100))
check "a value that is no boolean falls back to the file" "[ \"\$(get nonsense x)\" != $X ]"
do_input up left
grep -q 'ignoring MWC_MOVE_OPAQUE value maybe' "$LOG" || fail "the bad value is not reported"
echo "ok: the bad value is reported"
stop nonsense
stop_vinput
stop_mwc

echo "PASS: move"
