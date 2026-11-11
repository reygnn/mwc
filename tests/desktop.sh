#!/bin/bash
# Headless test of the virtual desktops: switching through ext-workspace-v1 as a
# tool does, moving windows through the window menu, what goes along (dialogs, tab
# groups) and what stays everywhere (panels), icons and tiling per desktop, and a
# window on another desktop asking for attention.
# Output goes to build/desktop.log. Exits non-zero on failure.
LOG=build/desktop.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland
for tool in build/vinput build/xclient build/xdgc build/workspacec; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
wait_for "the startup command ran" test -s "$SMOKE_DIR/display"
X_DISPLAY=$(cat "$SMOKE_DIR/display")
start_vinput
refresh

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
ws() { WAYLAND_DISPLAY=wayland-0 build/workspacec "$@"; }
switch_to() {
	ws activate "$1" >/dev/null || fail "activating desktop $1"
	check "desktop $1 is current" "[ \"\$(global desktop current)\" = $1 ]"
}
on() { # description app desktop shown(0/1)
	check "$1" "[ \"\$(get $2 desktop)\" = $3 ] && [ \"\$(get $2 visible)\" = $4 ]"
}
to_desktop() { # app n: through its window menu
	do_input key alt+space
	check "the window menu of $1 is open" "[ \"\$(global menu open)\" = 1 ]"
	do_input key "$2"
	check "$1 went to desktop $2" "[ \"\$(get $1 desktop)\" = $2 ]"
}

# ---- what a tool sees ---------------------------------------------------------------

out=$(ws list) || fail "workspacec list: $out"
echo "$out" >>"$LOG"
[ "$(grep -c '^workspace ' <<<"$out")" = 4 ] || fail "not four workspaces: $out"
grep -q '^workspace id=1 name=1 x=0 state=active caps=activate group=1$' <<<"$out" ||
	fail "desktop 1 isn't the active one: $out"
grep -q '^workspace id=4 name=4 x=3 state=none caps=activate group=1$' <<<"$out" ||
	fail "desktop 4: $out"
grep -q '^group outputs=1 caps=0$' <<<"$out" || fail "the group: $out"
echo "ok: four desktops in one group for the output, the first one active"

# ---- switching -------------------------------------------------------------------------

wayland a size=300x200
on "a window opens on the current desktop" a 1 1
switch_to 2
on "switched away: it is out of sight" a 1 0
check "and nothing has the focus" "[ \"\$(global focus any)\" = 0 ]"
wayland b size=300x200
on "a window opened now is on desktop 2" b 2 1
check "and has the focus" "[ \"\$(get b active)\" = 1 ]"
do_input key alt+Tab
check "Alt+Tab doesn't reach the other desktop" "[ \"\$(get b active)\" = 1 ] && [ \"\$(get a visible)\" = 0 ]"
switch_to 1
on "back on desktop 1: it is there again" a 1 1
on "and the other one isn't" b 2 0
check "the window there gets the focus" "[ \"\$(get a active)\" = 1 ] && [ \"\$(get a kbfocus)\" = 1 ]"
out=$(ws list) || fail "workspacec list: $out"
grep -q '^workspace id=1 name=1 x=0 state=active' <<<"$out" &&
	grep -q '^workspace id=2 name=2 x=1 state=none' <<<"$out" ||
	fail "the tool doesn't see the switch: $out"
echo "ok: the tool sees which one is current"

# ---- moving a window -----------------------------------------------------------------

to_desktop a 3
on "moved: it is out of sight" a 3 0
check "and has lost the focus" "[ \"\$(global focus any)\" = 0 ]"
switch_to 3
on "it is on desktop 3" a 3 1
stop a
stop b

# A dialog goes along with its window, and a tab group as a whole.
x11 parent size=300x200 dialog=kid
check "the dialog is there" "[ -n \"\$(get kid x)\" ]"
do_input move $(($(get parent x) + 30)) $(($(get parent y) + $(get parent th) + 30))
do_input click left
check "the window has the focus" "[ \"\$(get parent active)\" = 1 ]"
to_desktop parent 4
on "its dialog went along" kid 4 0
stop parent
x11 g1 size=300x200
x11 g2 size=300x200
do_input key super+g
check "a tab group" "[ \"\$(get g1 tab)\" != -1 ] && [ \"\$(get g2 tab)\" != -1 ]"
to_desktop g2 1
check "the whole group went" "[ \"\$(get g1 desktop)\" = 1 ] && [ \"\$(get g2 desktop)\" = 1 ]"
switch_to 1
check "and is shown there, one tab of it" \
	"[ \$((\$(get g1 visible) + \$(get g2 visible))) = 1 ]"
stop g1
stop g2

# A panel is on every desktop and can't be moved.
x11 dock type=dock pos=0,0 size=1280x30 strut=0,0,30,0
switch_to 2
on "a panel stays on every desktop" dock 1 1
switch_to 1
stop dock

# ---- icons and tiling per desktop ------------------------------------------------------

wayland i1 size=300x200
do_input key alt+F9
check "minimized on desktop 1, its icon shows" "[ \"\$(get i1 min)\" = 1 ] && [ \"\$(get i1 icon_shown)\" = 1 ]"
switch_to 2
check "not on desktop 2" "[ \"\$(get i1 icon_shown)\" = 0 ]"
wayland i2 size=300x200
do_input key alt+F9
check "an icon on desktop 2 takes the first slot there too" \
	"[ \"\$(get i2 icon_shown)\" = 1 ] && [ \"\$(get i2 icon_x)\" = \"\$(get i1 icon_x)\" ] && \
	[ \"\$(get i2 icon_y)\" = \"\$(get i1 icon_y)\" ]"
switch_to 1
check "back: the first one's icon shows, the other's doesn't" \
	"[ \"\$(get i1 icon_shown)\" = 1 ] && [ \"\$(get i2 icon_shown)\" = 0 ]"
stop i1
stop i2

do_input key super+t
check "tiling on" "[ \"\$(global tiling on)\" = 1 ]"
wayland t1 size=300x200
wayland t2 size=300x200
check "two tiled on desktop 1" "[ \"\$(get t1 tiled)\" = 1 ] && [ \"\$(get t2 tiled)\" = 1 ]"
T1="$(get t1 x) $(get t1 y) $(get t1 w) $(get t1 h)"
switch_to 2
wayland t3 size=300x200
check "alone on desktop 2, it has the whole layout" \
	"[ \"\$(get t3 tiled)\" = 1 ] && [ \"\$(get t3 tile_main)\" = 1 ] && [ \"\$(get t3 w)\" -gt 1000 ]"
check "the others stay in their layout" "[ \"\$(get t1 tiled)\" = 1 ] && [ \"\$(get t2 tiled)\" = 1 ]"
switch_to 1
check "back: they are where they were" \
	"[ \"\$(get t1 x) \$(get t1 y) \$(get t1 w) \$(get t1 h)\" = \"$T1\" ]"
do_input key super+t
stop t1
stop t2
stop t3

# ---- attention from another desktop ----------------------------------------------------

# Its dialog comes while the window is on another desktop: that desktop is marked,
# and the user stays where they are.
x11 late size=300x200 dialog=latekid dialog_ms=2500
to_desktop late 2
ws urgent 2 >"$SMOKE_DIR/urgent.out" 2>&1 &
PID_urgent=$!
check "its dialog appears, on desktop 2" "[ \"\$(get latekid desktop)\" = 2 ] && [ \"\$(get latekid visible)\" = 0 ]"
wait "$PID_urgent" || fail "desktop 2 isn't marked urgent: $(cat "$SMOKE_DIR/urgent.out")"
echo "ok: desktop 2 is marked urgent"
check "the current desktop stays" "[ \"\$(global desktop current)\" = 1 ]"
switch_to 2
on "there it is" latekid 2 1
out=$(ws list) || fail "workspacec list: $out"
grep -q '^workspace id=2 name=2 x=1 state=active caps' <<<"$out" ||
	fail "going there doesn't clear urgent: $out"
echo "ok: going there clears it"
stop late

kill -0 "$pid" || fail "mwc died"
stop_vinput
stop_mwc
echo "PASS: desktop"
