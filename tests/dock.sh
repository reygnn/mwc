#!/bin/bash
# Headless test of the dock (Mwm*NAME*dock): Wayland and X11 tools side by side in
# mwmrc order from the corner, aligned at the bottom, the strip kept free, no
# focus on map and none by Alt+Tab, no moving, minimizing, maximizing or fullscreen,
# closing up when one grows or goes, clicks between them, and a reload that changes
# who is docked and where; dialogs stay outside. Then what clients and other features
# might do to a docked window: states asked for before mapping, a late app_id, an X11
# move, _NET_WM_STATE_BELOW, Lower, the window menu, tabs, tiling, desktops; a column,
# a centred row and a wrong dockPlacement.
# Output goes to build/dock.log. Exits non-zero on failure.
LOG=build/dock.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland grim
for tool in build/vinput build/xclient build/xdgc build/pngtool build/workspacec; do
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
# The middle of its client area.
mid() {
	echo $(($(get "$1" x) + $(get "$1" bw) + $(get "$1" w) / 2)) \
		$(($(get "$1" y) + $(get "$1" bw) + $(get "$1" th) + $(get "$1" h) / 2))
}
ws() { WAYLAND_DISPLAY=wayland-0 build/workspacec "$@"; }

check "no tools: no dock, the whole output usable" \
	"[ \"\$(dock w)\" = 0 ] && [ \"\$(global output uh)\" = 720 ]"

# ---- filling ----------------------------------------------------------------------

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

# ---- the strip --------------------------------------------------------------------

do_input key alt+F10
check "plain maximized: above the dock" \
	"[ \"\$(get plain max)\" = 1 ] && [ \$(bottom plain) = \$(dock y) ]"
do_input key alt+F10
check "plain back" "[ \"\$(get plain max)\" = 0 ]"

# Beside the dock the strip is there to use: the left half reaches the output's
# bottom, the right one, over the dock, stops above it, as do its quarters. Two has
# grown first, so the dock keeps still meanwhile.
G=8 # MWC_GAP
wait_for "two has grown" grep -q '^grown' "$SMOKE_DIR/two.log"
check "the dock as high as two" "[ \"\$(dock h)\" = \$((2 * B + \$(get two fh))) ]"
check "plain has the focus" "[ \"\$(get plain active)\" = 1 ]"
do_input key super+Left
check "plain snapped left: down to the output's edge, beside the dock" \
	"[ \"\$(get plain snap)\" != 0 ] && [ \$(bottom plain) = $((720 - G)) ]"
do_input key super+Down
check "its bottom quarter too" "[ \$(bottom plain) = $((720 - G)) ] && \
	[ \$(get plain y) -gt 360 ]"
do_input key super+Up
# A tool widens the dock under the left half: it refits above the dock, and goes
# back down when the dock shrinks. Only the dock's box changes, not the areas.
echo 'Mwm*wide*dock: true' >>"$RC"
reload_mwc
wayland wide size=600x40
check "the dock reaches under the left half" "[ \$(dock x) -lt 636 ]"
check "the left half refits above it" "[ \$(bottom plain) = \$((\$(dock y) - G)) ]"
stop wide
check "and back down when the dock shrinks" "[ \$(bottom plain) = $((720 - G)) ]"
do_input key super+Right
do_input key super+Right
check "snapped right, over the dock: above it" \
	"[ \"\$(get plain snap)\" != 0 ] && [ \$(bottom plain) = \$((\$(dock y) - G)) ]"
do_input key super+Up
check "its top quarter: half the usable height, in line with the quarter below" \
	"[ \$(bottom plain) = \$((G + (\$(dock y) - 3 * G) / 2)) ]"
do_input key super+Down
do_input key super+Left
check "plain free again" "[ \"\$(get plain snap)\" = 0 ]"
# Pushed down: beside the dock as far as the edge, over it only to the dock.
do_input key super+shift+Down
check "pushed down beside the dock: to the output's bottom" "[ \$(bottom plain) = 720 ]"
do_input key super+shift+Right
check "pushed right, now over the dock: above it" \
	"[ \$(right plain) = 1280 ] && [ \$(bottom plain) = \$(dock y) ]"
do_input key super+shift+Left
do_input key super+shift+Down
check "pushed down again" "[ \$(bottom plain) = 720 ]"
# The dock growing over a free window in its strip moves the window out.
wayland wide size=600x40
check "a free window the dock now covers moves out of its strip" \
	"[ \$(bottom plain) = \$(dock y) ]"
stop wide
do_input key super+shift+Up
# Placed by an entry: beside the dock it may reach into the strip, from either side.
printf 'Mwm*low*geometry: +20+500\nMwm*lower*geometry: +20-0\n' >>"$RC"
reload_mwc
wayland low size=200x150
check "a geometry beside the dock is kept, into its strip" \
	"[ \"\$(get low y)\" = 500 ] && [ \$(bottom low) -gt \$(dock y) ]"
stop low
wayland lower size=200x150
check "one from the bottom counts from the output's edge" "[ \$(bottom lower) = 720 ]"
stop lower

# ---- focus and functions ----------------------------------------------------------

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
check "a click on the dock's frame opens nothing" "[ \"\$(global menu open)\" = 0 ]"

# ---- changes ----------------------------------------------------------------------

wait_for "two has grown" grep -q '^grown' "$SMOKE_DIR/two.log"
check "two grew: still left of xone, the dock higher" \
	"[ \"\$(get two h)\" = 140 ] && [ \$(right two) = \$(get xone x) ] && \
	 [ \$(bottom two) = $((720 - B)) ] && [ \"\$(dock h)\" = \$((2 * B + \$(get two fh))) ] && \
	 [ \"\$(global output uh)\" = \$(dock y) ]"
stop xone
check "xone gone: two closes up to one" \
	"[ \$(right two) = \$(get one x) ] && \
	 [ \"\$(dock w)\" = \$((2 * B + \$(ow one) + \$(ow two))) ]"

# ---- reload -----------------------------------------------------------------------

TWO_X=$(get two x) TWO_Y=$(get two y)
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
	"[ \"\$(get two x)\" = $TWO_X ] && [ \"\$(get two y)\" = $TWO_Y ] && \
	 [ \"\$(get two everywhere)\" = 0 ]"
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
for app in plain one other two tool; do
	stop $app
done

# ---- what clients ask for ---------------------------------------------------------

DOCKED='Mwm*premax*dock: true
Mwm*prefull*dock: true
Mwm*late*dock: true
Mwm*xmove*dock: true
Mwm*xbelow*dock: true'
echo "$DOCKED" >"$RC"
reload_mwc
wayland premax size=200x100 pre=max
check "maximized before mapping: docked at its own size" \
	"[ \"\$(get premax max)\" = 0 ] && [ \"\$(get premax w)\" = 200 ] && \
	 [ \$(right premax) = $((1280 - B)) ] && [ \$(bottom premax) = $((720 - B)) ] && \
	 [ \"\$(global output uh)\" = \$(dock y) ]"
wayland prefull size=150x100 pre=full
check "fullscreen before mapping: docked with its frame" \
	"[ \"\$(get prefull full)\" = 0 ] && [ \"\$(get prefull w)\" = 150 ] && \
	 [ \"\$(get prefull frame)\" = 1 ] && [ \$(right prefull) = \$(get premax x) ]"
WAYLAND_DISPLAY=wayland-0 build/xdgc app=early size=100x60 late_app=late \
	>"$SMOKE_DIR/late.log" 2>&1 &
PID_late=$!
check "an app_id set after mapping: docked" \
	"[ -n \"\$(get late x)\" ] && [ \"\$(get late everywhere)\" = 1 ] && \
	 [ \$(right late) = \$(get prefull x) ]"

x11 xmove size=120x50 moveto=100,100
wait_for "xmove asked to move" grep -q '^moved' "$SMOKE_DIR/xmove.log"
# A normal window's map is handled after that request, and doesn't touch the dock.
x11 sync1 size=200x150
check "an X11 tool can't move itself out of the dock" \
	"[ \$(right xmove) = \$(get late x) ] && [ \$(bottom xmove) = $((720 - B)) ]"

x11 xbelow size=100x50 state=below color=00ff00 grow=100x150
check "xbelow docked" "[ \$(right xbelow) = \$(get xmove x) ]"
wait_for "xbelow has grown" grep -q '^grown' "$SMOKE_DIR/xbelow.log"
check "an X11 tool grew: the dock as high as it" \
	"[ \"\$(get xbelow h)\" = 150 ] && [ \$(bottom xbelow) = $((720 - B)) ] && \
	 [ \"\$(dock h)\" = \$((2 * B + \$(get xbelow fh))) ]"
pixel_is "asked to be below: still above the dock's frame" $(mid xbelow) 00ff00

# ---- what the user might do -------------------------------------------------------

click_at $(mid premax)
check "premax has the focus" "[ \"\$(get premax active)\" = 1 ]"
do_input key alt+F3
pixel_is "Lower keeps a docked window above the dock's frame" $(mid premax) 808080
click_at $(mid premax)
check "premax has the focus again" "[ \"\$(get premax active)\" = 1 ]"
do_input key alt+space
check "its window menu: only what a docked window can do" \
	"[ \"\$(global menu open)\" = 1 ] && [[ \"\$(global menu sensitive)\" = *C* ]] && \
	 [[ \"\$(global menu sensitive)\" != *[MSnxT1234O]* ]]"
do_input key Escape
check "the menu closed" "[ \"\$(global menu open)\" = 0 ]"
do_input key super+g
check "no tab group with the window before" \
	"[ \"\$(get premax tab)\" = -1 ] && [ \"\$(get sync1 tab)\" = -1 ]"
do_input key super+t
check "tiling: the normal window tiles, docked ones don't" \
	"[ \"\$(global tiling on)\" = 1 ] && [ \"\$(get sync1 tiled)\" = 1 ] && \
	 [ \"\$(get premax tiled)\" = 0 ] && [ \$(right premax) = $((1280 - B)) ]"
do_input key super+t
check "tiling off" "[ \"\$(global tiling on)\" = 0 ] && [ \"\$(get sync1 tiled)\" = 0 ]"
ws activate 2 >/dev/null || fail "activating desktop 2"
check "on desktop 2: the dock, not the normal window" \
	"[ \"\$(global desktop current)\" = 2 ] && [ \"\$(get premax visible)\" = 1 ] && \
	 [ \"\$(get xmove visible)\" = 1 ] && [ \"\$(get sync1 visible)\" = 0 ]"
ws activate 1 >/dev/null || fail "activating desktop 1"
check "back on desktop 1" "[ \"\$(global desktop current)\" = 1 ]"

# A window that is an icon when a reload docks it comes out of it, into the dock.
wayland later size=200x40
click_at $(($(get later x) + 60)) $(($(get later y) + $(get later bw) + 5))
check "later has the focus" "[ \"\$(get later active)\" = 1 ]"
do_input key alt+F9
check "later is an icon" "[ \"\$(get later min)\" = 1 ]"
printf '%s\nMwm*later*dock: true\n' "$DOCKED" >"$RC"
reload_mwc
check "a reload docks an icon: restored, into the dock" \
	"[ \"\$(get later min)\" = 0 ] && [ \$(right later) = \$(get xbelow x) ] && \
	 [ \$(bottom later) = $((720 - B)) ]"

# ---- other placements -------------------------------------------------------------

printf '%s\nMwm*dockPlacement: bottom right\n' "$DOCKED" >"$RC"
reload_mwc
check "a column from the bottom right corner up, aligned right" \
	"[ \$(right premax) = $((1280 - B)) ] && [ \$(bottom premax) = $((720 - B)) ] && \
	 [ \$(right prefull) = $((1280 - B)) ] && [ \$(bottom prefull) = \$(get premax y) ]"
check "its strip at the right kept free" \
	"[ \"\$(global output uw)\" = \$(dock x) ] && [ \"\$(global output uh)\" = 720 ]"
# Beside a column snapping keeps the usable area: a quarter in a row clear of the
# dock would overlap the half next to it. One tool keeps the column low.
printf 'Mwm*premax*dock: true\nMwm*dockPlacement: bottom right\n' >"$RC"
reload_mwc
wayland side size=300x200
check "the top row is clear of the dock" "[ \$(dock y) -gt 360 ]"
do_input key super+Right
do_input key super+Up
check "beside a column a quarter keeps the usable area, in line with the halves" \
	"[ \"\$(get side snap)\" != 0 ] && [ \$(right side) = \$((\$(dock x) - G)) ]"
do_input key super+Down
do_input key super+Down
check "the bottom right quarter, by the column: up to it" \
	"[ \$(right side) = \$((\$(dock x) - G)) ] && [ \$(get side y) -gt 360 ]"
stop side
printf '%s\nMwm*dockPlacement: bottom right\n' "$DOCKED" >"$RC"
reload_mwc
check "the column back" \
	"[ \$(right premax) = $((1280 - B)) ] && [ \$(bottom premax) = $((720 - B)) ]"
printf '%s\nMwm*dockPlacement: center top\n' "$DOCKED" >"$RC"
reload_mwc
check "a row in the middle of the top edge, from the left" \
	"[ \"\$(dock y)\" = 0 ] && [ \"\$(dock x)\" = \$(((1280 - \$(dock w)) / 2)) ] && \
	 [ \"\$(get premax x)\" = \$((\$(dock x) + B)) ] && [ \"\$(get premax y)\" = $B ] && \
	 [ \"\$(global output uy)\" = \$(dock h) ]"
printf '%s\nMwm*dockPlacement: left right\n' "$DOCKED" >"$RC"
reload_mwc
wait_for "a wrong dockPlacement is reported" \
	grep -q 'dockPlacement: expected a placement' "$LOG"
check "and the default is used" \
	"[ \$(right premax) = $((1280 - B)) ] && [ \$(bottom premax) = $((720 - B)) ]"

# ---- beside a dock along the top --------------------------------------------------

# Placed from the bottom, a window taller than the usable area reaches into the strip
# beside the dock, and keeps its bottom when it grows. Dragged to the left edge, the
# target and the snap reach the top.
printf 'Mwm*premax*dock: true\nMwm*dockPlacement: right top\nMwm*tall*geometry: +20-0\n' \
	>"$RC"
reload_mwc
check "a row along the top" \
	"[ \"\$(dock y)\" = 0 ] && [ \"\$(global output uy)\" = \$(dock h) ]"
wayland tall size=200x600 grow=200x640 grow_ms=1500
check "tall from the bottom, into the strip beside the dock" \
	"[ \$(bottom tall) = 720 ] && [ \$(get tall y) -lt \$(dock h) ]"
wait_for "tall has grown" grep -q '^grown' "$SMOKE_DIR/tall.log"
check "grown by itself, it keeps its bottom there" \
	"[ \"\$(get tall h)\" = 640 ] && [ \$(bottom tall) = 720 ]"
do_input move $(($(get tall x) + $(get tall bw) + 60)) $(($(get tall y) + $(get tall bw) + 5))
do_input down left
do_input move 0 360
check "dragging to the left edge: the target reaches the top" \
	"[ \"\$(global preview shown)\" = 1 ] && [ \"\$(global preview y)\" = $G ] && \
	 [ \"\$(global preview h)\" = $((720 - 2 * G)) ]"
do_input up left
check "dropped: snapped to it" \
	"[ \"\$(get tall snap)\" != 0 ] && [ \"\$(get tall y)\" = $G ] && \
	 [ \$(bottom tall) = $((720 - G)) ]"
stop tall

# The dock's frame costs no picture of the dock's size: a wide and a tall tool in a
# row make a dock of 12000x12000, 550 MB as one picture.
hwm() { awk '/^VmHWM/ { print int($2 / 1024) }' "/proc/$pid/status"; }
printf 'Mwm*wide*dock: true\nMwm*high*dock: true\n' >"$RC"
reload_mwc
before=$(hwm)
wayland wide size=12000x10 own_size=1
wayland high size=10x12000 own_size=1
check "a dock of both" "[ \"\$(dock w)\" -gt 12000 ] && [ \"\$(dock h)\" -gt 12000 ]"
after=$(hwm)
[ $((after - before)) -lt 200 ] || fail "the dock's frame took $((after - before)) MB"
echo "ok: the dock's frame takes $((after - before)) MB"
stop wide
stop high

stop_vinput
stop_mwc
echo "PASS"
