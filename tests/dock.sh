#!/bin/bash
# Headless test of the dock (Mwm*NAME*dock): Wayland and X11 tools side by side in
# mwmrc order from the corner, aligned at the bottom, the strip kept free, no
# focus on map and none by Alt+Tab, no moving, minimizing, maximizing or fullscreen,
# closing up when one grows or goes, clicks between them, and a reload that changes
# who is docked and where; dialogs stay outside.
# Output goes to build/dock.log. Exits non-zero on failure.
LOG=build/dock.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland
for tool in build/vinput build/xclient build/xdgc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
cat >"$RC" <<'EOF'
Mwm*one*dock: true
Mwm*xone*dock: true
Mwm*two*dock: true
EOF

export MWC_TEST_STATE=$SMOKE_DIR/state
start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
wait_for "the startup command ran" test -s "$SMOKE_DIR/display"
X_DISPLAY=$(cat "$SMOKE_DIR/display")
start_vinput
refresh
B=3 # bevel 2 + 1

x11() {
	local name=$1
	shift
	DISPLAY=$X_DISPLAY build/xclient class="$name" "$@" >"$SMOKE_DIR/$name.log" 2>&1 &
	eval "PID_$name=$!"
	check "$name is mapped" "[ -n \"\$(get $name x)\" ]"
}
wayland() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$name" "$@" >"$SMOKE_DIR/$name.log" 2>&1 &
	eval "PID_$name=$!"
	check "$name is mapped" "[ -n \"\$(get $name x)\" ]"
}
stop() {
	local var="PID_$1"
	kill "${!var}" 2>/dev/null
	wait "${!var}" 2>/dev/null
	check "$1 is gone" "[ -z \"\$(get $1 x)\" ]"
}
dock() { global dock "$1"; }
# The frame's outer width; its right and bottom edges.
ow() { echo $(($(get "$1" w) + 2 * $(get "$1" bw))); }
right() { echo $(($(get "$1" x) + $(ow "$1"))); }
bottom() { echo $(($(get "$1" y) + $(get "$1" fh))); }
click_at() { do_input move "$1" "$2"; do_input click left; }

check "no tools: no dock, the whole output usable" \
	"[ \"\$(dock w)\" = 0 ] && [ \"\$(global output uh)\" = 720 ]"

# ---- filling ---------------------------------------------------------------------

wayland plain size=300x200
check "a normal window has the focus" "[ \"\$(get plain active)\" = 1 ]"
wayland one size=200x100
check "one: in the bottom right corner, inside the dock's frame" \
	"[ \$(right one) = $((1280 - B)) ] && [ \$(bottom one) = $((720 - B)) ]"
check "the dock: around it, at the output's edge" \
	"[ \$((\$(dock x) + \$(dock w))) = 1280 ] && [ \$((\$(dock y) + \$(dock h))) = 720 ] && \
	 [ \"\$(dock w)\" = \$((2 * B + \$(ow one))) ] && [ \"\$(dock h)\" = \$((2 * B + \$(get one fh))) ]"
check "mapping a docked window takes no focus" \
	"[ \"\$(get one active)\" = 0 ] && [ \"\$(get plain active)\" = 1 ]"
check "the dock's strip is kept free" "[ \"\$(global output uh)\" = \$(dock y) ]"
check "on every desktop" "[ \"\$(get one everywhere)\" = 1 ]"

x11 xone size=150x60
check "xone, the second line: left of one, aligned at the bottom" \
	"[ \$(right xone) = \$(get one x) ] && [ \$(bottom xone) = $((720 - B)) ]"
check "the dock as high as the highest" \
	"[ \"\$(dock h)\" = \$((2 * B + \$(get one fh))) ] && \
	 [ \"\$(dock w)\" = \$((2 * B + \$(ow one) + \$(ow xone))) ]"
wayland two size=100x40 grow=120x140 grow_ms=1500
check "two, the third line: left of xone" "[ \$(right two) = \$(get xone x) ]"

# ---- the strip ----------------------------------------------------------------------

do_input key alt+F10
check "plain maximized: above the dock" \
	"[ \"\$(get plain max)\" = 1 ] && [ \$(bottom plain) = \$(dock y) ]"
do_input key alt+F10
check "plain back" "[ \"\$(get plain max)\" = 0 ]"

# ---- focus and functions ----------------------------------------------------------------

wayland other size=300x200
check "other has the focus" "[ \"\$(get other active)\" = 1 ]"
do_input key alt+Tab
check "Alt+Tab goes to plain, past the dock" "[ \"\$(get plain active)\" = 1 ]"
do_input key alt+Tab
check "and back to other" "[ \"\$(get other active)\" = 1 ]"
click_at $(($(get one x) + $(get one bw) + 50)) $(($(get one y) + $(get one fh) - 20))
check "a click focuses a docked window" "[ \"\$(get one active)\" = 1 ]"
X=$(get one x) Y=$(get one y)
do_input key alt+F9
do_input key alt+F10
do_input key alt+F11
refresh
check "no minimizing, maximizing or fullscreen" \
	"[ \"\$(get one min)\" = 0 ] && [ \"\$(get one max)\" = 0 ] && [ \"\$(get one full)\" = 0 ]"
do_input key alt+Tab
check "Alt+Tab from a docked window goes on among the others" \
	"[ \"\$(get one active)\" = 0 ] && [ \"\$(get plain active)\" = 1 ]"
click_at $(($(get one x) + $(get one bw) + 50)) $(($(get one y) + $(get one fh) - 20))
check "one has the focus again" "[ \"\$(get one active)\" = 1 ]"
do_input key alt+F7
do_input move $(($(get other x) + 100)) $(($(get other y) + 100))
do_input click left
check "no moving" "[ \"\$(get one x)\" = $X ] && [ \"\$(get one y)\" = $Y ]"

# A click on the frame between the tools: not the desktop's root menu.
check "no menu yet" "[ \"\$(global menu open)\" = 0 ]"
click_at $(($(dock x) + 1)) $(($(dock y) + 1))
refresh
check "a click on the dock's frame opens nothing" "[ \"\$(global menu open)\" = 0 ]"

# ---- changes -------------------------------------------------------------------------

wait_for "two has grown" grep -q '^grown' "$SMOKE_DIR/two.log"
check "two grew: still left of xone, the dock higher" \
	"[ \"\$(get two h)\" = 140 ] && [ \$(right two) = \$(get xone x) ] && \
	 [ \$(bottom two) = $((720 - B)) ] && [ \"\$(dock h)\" = \$((2 * B + \$(get two fh))) ] && \
	 [ \"\$(global output uh)\" = \$(dock y) ]"
stop xone
check "xone gone: two closes up to one" \
	"[ \$(right two) = \$(get one x) ] && \
	 [ \"\$(dock w)\" = \$((2 * B + \$(ow one) + \$(ow two))) ]"

# ---- reload ----------------------------------------------------------------------------

TWO_X=$(get two x)
cat >"$RC" <<'EOF'
Mwm*dockPlacement: left top
Mwm*plain*dock: true
Mwm*one*dock: true
Mwm*tool*dock: true
Mwm*tooldlg*dock: true
EOF
reload_mwc
check "a new line pulls plain in: the first, top left" \
	"[ \"\$(get plain x)\" = $B ] && [ \"\$(get plain y)\" = $B ] && \
	 [ \"\$(get plain everywhere)\" = 1 ]"
check "one follows to its right, aligned at the top" \
	"[ \"\$(get one x)\" = \$(right plain) ] && [ \"\$(get one y)\" = $B ]"
check "the dock top left, the strip at the top kept free" \
	"[ \"\$(dock x)\" = 0 ] && [ \"\$(dock y)\" = 0 ] && \
	 [ \"\$(global output uy)\" = \$(dock h) ] && \
	 [ \"\$(global output uh)\" = \$((720 - \$(dock h))) ]"
check "two, no longer docked, stays where it was, on this desktop only" \
	"[ \"\$(get two x)\" = $TWO_X ] && [ \"\$(get two everywhere)\" = 0 ]"
click_at $(($(get two x) + 60)) $(($(get two y) + $(get two bw) + 5))
check "two has the focus" "[ \"\$(get two active)\" = 1 ]"
do_input key alt+F9
check "and can be minimized again" "[ \"\$(get two min)\" = 1 ]"

# A dialog stays outside, even with an entry of its own.
wayland tool size=80x50 dialog=tooldlg
check "tooldlg is mapped" "[ -n \"\$(get tooldlg x)\" ]"
check "tool docked right of one, its dialog not" \
	"[ \"\$(get tool x)\" = \$(right one) ] && [ \"\$(get tool y)\" = $B ] && \
	 [ \"\$(dock w)\" = \$((2 * B + \$(ow plain) + \$(ow one) + \$(ow tool))) ]"

cat >"$RC" <<'EOF'
EOF
reload_mwc
check "no dock lines: no dock, the whole output usable" \
	"[ \"\$(dock w)\" = 0 ] && [ \"\$(global output uh)\" = 720 ] && \
	 [ \"\$(global output uy)\" = 0 ]"

stop_vinput
stop_mwc
echo "PASS"
