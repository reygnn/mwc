#!/bin/bash
# Headless test of interactive move and resize: the outline, the feedback box,
# keyboard control, Escape, and opaque dragging.
# Output goes to build/move.log. Exits non-zero on failure.
LOG=build/move.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland grim
for tool in build/vinput build/pngtool build/xclient build/xdgc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
MODE_MOVE=1
MODE_RESIZE=2

start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
wait_for "the startup command ran" test -s "$SMOKE_DIR/display"
X_DISPLAY=$(cat "$SMOKE_DIR/display")
start_vinput
refresh
B=$(global theme border)
T=$(global theme title)

x11() {
	local name=$1
	shift
	DISPLAY=$X_DISPLAY build/xclient class="$name" "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "PID_$name=$!"
	check "$name is mapped" "[ -n \"\$(get $name x)\" ]"
}
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

x11 mover pos=100,100 size=300x200
OW=$((300 + 2 * B)) OH=$((200 + 2 * B + T))
TX=250 TY=$((100 + B + T / 2))

settle
do_input move "$TX" "$TY"
do_input down left
do_input move $((TX + 2)) $((TY + 2))
check "a press and a little motion don't start a move" \
	"[ \"\$(global cursor mode)\" = $MODE_MOVE ] && [ \"\$(global outline shown)\" = 0 ]"
do_input move $((TX + 50)) $((TY + 30))
outline_is "the outline follows the pointer" 150 130 "$OW" "$OH"
geometry_is "the window stays where it is" mover 100 100 300 200
feedback_is "the box shows the position" "+150_+130"
pixel_is "the outline is drawn in the window's colour" 151 200 7387ad
pixel_is "the title bar line is drawn too" 300 $((130 + B + T - 1)) 7387ad
refresh
pixel_is "the box is drawn" $(($(global feedback x) + 2)) $(($(global feedback y) + 10)) aeb2c3
do_input up left
geometry_is "the window goes there when released" mover 150 130 300 200
no_grab_left "nothing is left of the grab"

settle
TX=300 TY=$((130 + B + T / 2))
do_input move "$TX" "$TY"
do_input down left
do_input move $((TX + 2)) "$TY"
do_input up left
geometry_is "a small slip doesn't move the window" mover 150 130 300 200

settle
do_input move "$TX" "$TY"
do_input down left
do_input move $((TX + 80)) $((TY + 40))
outline_is "dragging again" 230 170 "$OW" "$OH"
do_input key Escape
no_grab_left "Escape ends the drag"
geometry_is "and leaves the window where it was" mover 150 130 300 200
do_input up left
geometry_is "the release doesn't change that" mover 150 130 300 200

# ---- moving with the keyboard ---------------------------------------------------

do_input key alt+F7
check "Alt+F7 starts a move" "[ \"\$(global cursor mode)\" = $MODE_MOVE ]"
outline_is "and shows the outline at once" 150 130 "$OW" "$OH"
feedback_is "and the box" "+150_+130"
do_input key Right
outline_is "an arrow key moves 10 px" 160 130 "$OW" "$OH"
do_input key Down
outline_is "in every direction" 160 140 "$OW" "$OH"
do_input key shift+Left
outline_is "Shift+arrow moves 1 px" 159 140 "$OW" "$OH"
do_input key shift+Up
outline_is "in every direction" 159 139 "$OW" "$OH"
do_input key a
check "other keys do nothing" "[ \"\$(global cursor mode)\" = $MODE_MOVE ]"
geometry_is "the window hasn't moved yet" mover 150 130 300 200
do_input key Return
geometry_is "Return confirms" mover 159 139 300 200
no_grab_left "nothing is left of the grab"

do_input key alt+F7
do_input key Left
outline_is "a new move" 149 139 "$OW" "$OH"
do_input key Escape
geometry_is "Escape cancels" mover 159 139 300 200
no_grab_left "nothing is left of the grab"

do_input key alt+space
do_input key m
check "Move in the window menu starts a move" "[ \"\$(global cursor mode)\" = $MODE_MOVE ]"
do_input key Escape

# ---- resizing -------------------------------------------------------------------

RX=$((159 + OW - 2)) RY=$((139 + OH - 2))
do_input move "$RX" "$RY"
do_input down left
do_input move $((RX + 60)) $((RY + 40))
outline_is "resizing: the outline grows" 159 139 $((OW + 60)) $((OH + 40))
geometry_is "the window keeps its size meanwhile" mover 159 139 300 200
feedback_is "the box shows the size" "360_x_240"
do_input up left
geometry_is "it takes the new size when released" mover 159 139 360 240
no_grab_left "nothing is left of the grab"

do_input move $((159 + 1)) $((139 + 1))
do_input down left
do_input move $((159 + 1 + 40)) $((139 + 1 + 20))
outline_is "from the top left, the opposite corner stays" 199 159 $((OW + 60 - 40)) $((OH + 40 - 20))
do_input key Escape
geometry_is "Escape cancels a resize" mover 159 139 360 240
no_grab_left "nothing is left of the grab"
do_input up left

do_input key alt+F8
check "Alt+F8 starts a resize" "[ \"\$(global cursor mode)\" = $MODE_RESIZE ]"
outline_is "and shows the outline at once" 159 139 $((OW + 60)) $((OH + 40))
do_input key Right
do_input key shift+Down
outline_is "arrow keys resize as well" 159 139 $((OW + 70)) $((OH + 41))
do_input key Return
geometry_is "Return confirms" mover 159 139 370 241
no_grab_left "nothing is left of the grab"
stop mover

# Sizes in the client's own units, like 80x24 for a terminal.
x11 stepper pos=100,100 size=320x210 base=20x10 inc=30x20
do_input key alt+F8
feedback_is "the box counts steps above the base size" "10_x_10"
do_input key Right
do_input key Right
do_input key Right
feedback_is "and follows them" "11_x_10"
do_input key Escape
stop stepper

# The axis not being dragged must keep its size, steps or not.
x11 edgey pos=100,100 size=325x215 inc=30x20
refresh
do_input move $((100 + 325 + 2 * B - 2)) $((100 + 100))
do_input down left
do_input move $((100 + 325 + 2 * B + 60)) $((100 + 100))
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

x11 maxed pos=100,100 size=300x200
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
geometry_is "and it still remembers its place" maxed 100 100 300 200
stop maxed

# ---- the window goes away during a drag --------------------------------------------

x11 victim pos=100,100 size=300x200
settle
do_input move 250 $((100 + B + T / 2))
do_input down left
do_input move 300 200
outline_is "dragging a window" 150 $((100 + 200 - 100 - B - T / 2)) "$OW" "$OH"
stop victim
no_grab_left "when it goes away, so does the grab"
do_input up left
kill -0 "$pid" || fail "mwc died"

# ---- opaque dragging --------------------------------------------------------------

echo 'Mwm*moveOpaque: true' >"$RC"
kill -HUP "$pid"
sleep 0.2

x11 opaque pos=100,100 size=300x200
settle
TX=250 TY=$((100 + B + T / 2))
do_input move "$TX" "$TY"
do_input down left
do_input move $((TX + 50)) $((TY + 30))
geometry_is "moveOpaque: the window itself follows the pointer" opaque 150 130 300 200
check "moveOpaque: no outline" "[ \"\$(global outline shown)\" = 0 ]"
feedback_is "moveOpaque: the box stays" "+150_+130"
do_input key Escape
geometry_is "moveOpaque: Escape puts it back" opaque 100 100 300 200
do_input up left
no_grab_left "nothing is left of the grab"

RX=$((100 + OW - 2)) RY=$((100 + OH - 2))
do_input move "$RX" "$RY"
do_input down left
do_input move $((RX + 60)) $((RY + 40))
geometry_is "moveOpaque: the window itself grows" opaque 100 100 360 240
do_input key Escape
geometry_is "moveOpaque: Escape restores the size" opaque 100 100 300 200
do_input up left
stop opaque

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
start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display2"; exec sleep 300'
wait_for "the startup command ran" test -s "$SMOKE_DIR/display2"
X_DISPLAY=$(cat "$SMOKE_DIR/display2")
start_vinput
x11 envwins pos=100,100 size=300x200
settle
do_input move 250 $((100 + B + T / 2))
do_input down left
do_input move 300 200
check "MWC_MOVE_OPAQUE=0 beats the file" "[ \"\$(global outline shown)\" = 1 ]"
do_input key Escape
do_input up left
stop envwins

stop_vinput
stop_mwc

# Something that is no boolean is reported, and the file decides.
export MWC_MOVE_OPAQUE=maybe
rm -f "$SMOKE_DIR/display2"
start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display2"; exec sleep 300'
wait_for "the startup command ran" test -s "$SMOKE_DIR/display2"
X_DISPLAY=$(cat "$SMOKE_DIR/display2")
start_vinput
x11 nonsense pos=100,100 size=300x200
settle
do_input move 250 $((100 + B + T / 2))
do_input down left
do_input move 300 200
check "a value that is no boolean falls back to the file" "[ \"\$(get nonsense x)\" != 100 ]"
do_input up left
grep -q 'ignoring MWC_MOVE_OPAQUE value maybe' "$LOG" || fail "the bad value is not reported"
echo "ok: the bad value is reported"
stop nonsense
stop_vinput
stop_mwc

echo "PASS: move"
