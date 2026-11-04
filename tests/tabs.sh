#!/bin/bash
# Headless test of tab groups: forming them, switching, what only the shown window takes
# part in, windows leaving, and how groups get along with the window states.
# Output goes to build/tabs.log. Exits non-zero on failure.
LOG=build/tabs.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland grim
for tool in build/vinput build/xclient build/xdgc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state

begin() {
	rm -f "$SMOKE_DIR/display"
	start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
	wait_for "the startup command ran" test -s "$SMOKE_DIR/display"
	X_DISPLAY=$(cat "$SMOKE_DIR/display")
	start_vinput
	refresh
	B=$(global theme border)
	T=$(global theme title)
}
finish() {
	stop_vinput
	stop_mwc
}
x11() {
	local name=$1
	shift
	DISPLAY=$X_DISPLAY build/xclient class="$name" "$@" >"$SMOKE_DIR/$name.log" 2>&1 &
	eval "PID_$name=$!"
	check "$name is mapped" "[ -n \"\$(get $name x)\" ] && [ \"\$(get $name active)\" = 1 ]"
}
wayland() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$name" "$@" >"$SMOKE_DIR/$name.log" 2>&1 &
	eval "PID_$name=$!"
	check "$name is mapped" "[ -n \"\$(get $name x)\" ] && [ \"\$(get $name active)\" = 1 ]"
}
stop() {
	local var="PID_$1"
	kill "${!var}" 2>/dev/null
	wait "${!var}" 2>/dev/null
	check "$1 is gone" "[ -z \"\$(get $1 x)\" ]"
}
shown() { # description app, and it has the focus
	check "$1" "[ \"\$(get $2 tab_hidden)\" = 0 ] && [ \"\$(get $2 active)\" = 1 ] && \
		[ \"\$(get $2 kbfocus)\" = 1 ]"
}
hidden() {
	check "$1" "[ \"\$(get $2 tab_hidden)\" = 1 ] && [ \"\$(get $2 active)\" = 0 ] && \
		[ \"\$(get $2 visible)\" = 0 ]"
}
alone() { check "$1" "[ \"\$(get $2 tab)\" = -1 ] && [ \"\$(get $2 tab_hidden)\" = 0 ]"; }
said() { grep -q "^$2" "$SMOKE_DIR/$1.log"; }

begin

# ---- forming a group ---------------------------------------------------------------------

wayland a size=400x300
refresh
AX=$(get a x) AY=$(get a y)
x11 b size=300x200
do_input key super+g
shown "Super+G: the window stays shown" b
hidden "and the other is behind it" a
check "in the place of the other, at its size" \
	"[ \"\$(get b x)\" = $AX ] && [ \"\$(get b y)\" = $AY ] && \
	[ \"\$(get b w)\" = 400 ] && [ \"\$(get b h)\" = 300 ]"
check "in tab order" "[ \"\$(get a tab)\" = 0 ] && [ \"\$(get b tab)\" = 1 ]"

do_input key super+Tab
shown "Super+Tab: the next tab" a
hidden "and the other goes" b
check "nothing moved" "[ \"\$(get a x)\" = $AX ] && [ \"\$(get a y)\" = $AY ] && [ \"\$(get a w)\" = 400 ]"
do_input key super+shift+Tab
shown "Super+Shift+Tab: back again" b

x11 c size=200x100
refresh
[ "$(get c tab)" = -1 ] || fail "a new window joins by itself"
do_input key alt+Tab
shown "Alt+Tab counts the group once" b
hidden "and doesn't reach the hidden tab" a
do_input key alt+Tab
shown "and on to the other window" c
do_input key super+g
shown "a third one" c
check "at the end" "[ \"\$(get c tab)\" = 2 ]"
do_input key super+1
shown "Super+1: the first tab" a
do_input key super+3
shown "Super+3: the third" c
do_input key super+9
shown "Super+9: nothing there, nothing happens" c

# ---- sizes, moving, states -----------------------------------------------------------------

do_input key alt+F8
do_input key Right
do_input key Right
do_input key Return
check "the shown one has a new size" "[ \"\$(get c w)\" != 400 ]"
refresh
W=$(get c w) H=$(get c h)
check "and the hidden ones have it too, before they are shown" \
	"[ \"\$(get a w)\" = $W ] && [ \"\$(get b w)\" = $W ] && [ \"\$(get a h)\" = $H ]"

do_input key alt+F10
check "maximized" "[ \"\$(get c max)\" = 1 ] && [ \$((\$(get c w) + 2 * B)) = 1280 ]"
do_input key super+1
shown "another tab" a
check "takes it over" "[ \"\$(get a max)\" = 1 ] && [ \$((\$(get a w) + 2 * B)) = 1280 ] && \
	[ \"\$(get c max)\" = 0 ]"
do_input key alt+F10
check "and un-maximized, it goes where the group was" \
	"[ \"\$(get a max)\" = 0 ] && [ \"\$(get a w)\" = $W ] && [ \"\$(get a x)\" = $AX ]"
do_input key super+Left
check "snapped" "[ \"\$(get a snap)\" != 0 ] && [ \"\$(get a x)\" = 0 ]"
do_input key super+3
check "the state moves over too" "[ \"\$(get c snap)\" != 0 ] && [ \"\$(get c x)\" = 0 ] && \
	[ \"\$(get a snap)\" = 0 ]"
do_input key alt+F5
check "restored" "[ \"\$(get c snap)\" = 0 ] && [ \"\$(get c x)\" = $AX ]"

do_input key alt+F11
check "fullscreen" "[ \"\$(get c full)\" = 1 ]"
do_input key super+Tab
check "no switching while it fills the screen" "[ \"\$(get c tab_hidden)\" = 0 ] && [ \"\$(get c full)\" = 1 ]"
do_input key alt+F11
check "and back" "[ \"\$(get c full)\" = 0 ]"

do_input key alt+F9
check "minimized: one icon" "[ \"\$(get c min)\" = 1 ] && [ \"\$(get a min)\" = 0 ] && [ \"\$(get a tab_hidden)\" = 1 ]"
refresh
do_input move $(($(get c icon_x) + 30)) $(($(get c icon_y) + 30))
do_input click left
do_input click left
shown "restored, as it was" c
hidden "the others still behind it" a

# ---- leaving -------------------------------------------------------------------------------

do_input key super+shift+g
alone "Super+Shift+G: on its own" c
shown "with the focus" c
check "somewhere else" "[ \"\$(get c tab_hidden)\" = 0 ]"
check "the group goes on" "[ \"\$(get b tab)\" != -1 ]"
stop c
do_input key alt+Tab
stop b
alone "the last one left is alone" a
shown "and has the focus" a
stop a
finish

begin
wayland a size=400x300
x11 b size=300x200
do_input key super+g
x11 c size=200x100
do_input key super+g
shown "three, the last one shown" c
x11 d size=200x100
do_input key super+g
do_input key super+2
shown "the second of four" b
do_input key alt+F4
shown "the shown one closes, and the one to its right takes over" c
refresh
[ -z "$(get b x)" ] || fail "the closed window is still there"
do_input key super+1
shown "the first one" a
do_input key alt+F4
shown "the first closes: the one now first" c
do_input key alt+F4
shown "one more" d
alone "and it is alone" d
stop d
x11 b size=300x200
x11 c size=200x100
do_input key super+g
do_input key super+2
shown "the last of two" c
do_input key alt+F4
shown "at the end: the one to its left" b
alone "and it is alone" b
stop b

wayland a size=400x300
x11 b size=300x200
do_input key super+g
stop a
shown "a hidden one dies: nothing shows of it" b
alone "and the group is over" b
stop b

wayland nb size=300x200
x11 b size=300x200
do_input key super+g
do_input key super+Tab
shown "the client on top" nb
stop nb
shown "a shown one dies" b
alone "and the other carries on" b
stop b

# ---- who can't be in a group ---------------------------------------------------------------

wayland a size=400x300
x11 fixed size=200x100 min=200x100 max=200x100
do_input key super+g
alone "a window of a fixed size can't join" fixed
alone "and nothing happens to the other" a
stop fixed
wayland nof csd=1 size=200x100
do_input key super+g
alone "a window drawing its own frame can't join" nof
stop nof
DISPLAY=$X_DISPLAY build/xclient class=dlg dialog=kid >"$SMOKE_DIR/dlg.log" 2>&1 &
PID_dlg=$!
check "a dialog with the focus" "[ \"\$(get kid active)\" = 1 ]"
do_input key super+g
alone "a dialog can't join" kid
stop dlg
stop a

# ---- requests from behind ------------------------------------------------------------------

x11 asks ask_later=fullscreen size=300x200
wayland front size=400x300
do_input key super+g
shown "the other tab in front" front
wait_for "the hidden tab asks for fullscreen" said asks 'asked'
sleep 0.3
check "it stays behind" "[ \"\$(get asks tab_hidden)\" = 1 ] && [ \"\$(get asks full)\" = 0 ] && [ \"\$(get front full)\" = 0 ]"
stop asks
stop front

x11 pa dialog=dk dialog_ms=2500 size=300x200
wayland qa size=400x300
do_input key super+g
shown "a tab in front of the other" qa
check "a dialog of the hidden one brings it to the front" "[ \"\$(get pa tab_hidden)\" = 0 ] && [ \"\$(get qa tab_hidden)\" = 1 ]"
check "and has the focus" "[ \"\$(get dk active)\" = 1 ] && [ \"\$(get dk hidden)\" = 0 ]"
do_input key alt+Tab
do_input key super+Tab
check "the dialog goes with its tab" "[ \"\$(get dk hidden)\" = 1 ] && [ \"\$(get qa tab_hidden)\" = 0 ]"
do_input key super+Tab
check "and comes back with it" "[ \"\$(get dk hidden)\" = 0 ]"
stop pa
stop qa

# ---- clicks only reach what is shown -------------------------------------------------------

x11 under move_press=1 pos=100,100 size=400x300
x11 over move_press=1 size=300x200
do_input key super+g
shown "one X tab over another" over
: >"$SMOKE_DIR/under.log"
do_input move 300 250
do_input click left
wait_for "the shown one gets the click" said over 'press main'
sleep 0.3
said under 'press' && fail "a hidden X window caught a click"
echo "ok: the hidden X window didn't"
stop under
stop over

# ---- the layout ----------------------------------------------------------------------------

do_input key super+t
check "tiling" "[ \"\$(global tiling on)\" = 1 ]"
wayland s1 size=300x200
x11 s2 size=300x200
x11 s3 size=300x200
check "three cells" "[ \"\$(get s1 tiled)\" = 1 ] && [ \"\$(get s2 tiled)\" = 1 ] && [ \"\$(get s3 tile_main)\" = 1 ]"
refresh
do_input key super+g
shown "joined in the layout" s3
check "two cells now" "[ \"\$(get s2 tab_hidden)\" = 1 ] && [ \"\$(get s2 tiled)\" = 0 ] && [ \"\$(get s3 tiled)\" = 1 ]"
check "the other one takes all of the stack" "[ \"\$(get s1 tiled)\" = 1 ] && \
	[ \$((\$(get s1 y) + \$(get s1 h) + 2 * B + T + 8)) = 720 ]"
refresh
X=$(get s3 x) Y=$(get s3 y) W=$(get s3 w) M=$(get s3 tile_main)
do_input key super+Tab
shown "the other tab" s2
check "in the same cell" "[ \"\$(get s2 x)\" = $X ] && [ \"\$(get s2 y)\" = $Y ] && [ \"\$(get s2 w)\" = $W ] && \
	[ \"\$(get s2 tile_main)\" = $M ] && [ \"\$(get s3 tiled)\" = 0 ]"
do_input key super+shift+g
alone "out of it" s2
check "a new window leads the layout" "[ \"\$(get s2 tile_main)\" = 1 ] && [ \"\$(get s2 tiled)\" = 1 ]"
alone "the group is over" s3
check "and each has a cell" "[ \"\$(get s3 tiled)\" = 1 ] && [ \"\$(get s1 tiled)\" = 1 ]"
do_input key super+t
stop s1
stop s2
stop s3

# Its place in the layout goes with the shown tab: the group took the place of the older
# window, below the other one in the stack, though the newer window is what shows.
x11 o1 size=300x200
x11 o0 size=300x200
do_input key alt+Tab
shown "the older one" o1
x11 o2 size=300x200
do_input key super+g
x11 o3 size=250x150
do_input key super+t
do_input key alt+Tab
do_input key alt+Tab
shown "the group" o2
check "in the stack below the other" "[ \"\$(get o3 tile_main)\" = 1 ] && [ \"\$(get o2 y)\" -gt \"\$(get o0 y)\" ]"
do_input key super+Tab
shown "its other tab" o1
check "stays below it" "[ \"\$(get o1 tiled)\" = 1 ] && [ \"\$(get o1 y)\" -gt \"\$(get o0 y)\" ]"
do_input key super+t
stop o1
stop o2
stop o0
stop o3

# What a group goes back to, and its place in the layout, go with the shown tab.
x11 g1 pos=200,150 size=300x200
x11 g2 size=300x200
do_input key super+g
refresh
GX=$(get g2 x) GY=$(get g2 y)
x11 g3 size=250x150
do_input key super+t
check "two in the layout" "[ \"\$(get g2 tiled)\" = 1 ] && [ \"\$(get g3 tile_main)\" = 1 ]"
do_input key alt+Tab
shown "the group in the stack" g2
do_input key super+Tab
shown "its other tab" g1
check "in the stack as well, not leading" "[ \"\$(get g1 tile_main)\" = 0 ] && [ \"\$(get g3 tile_main)\" = 1 ]"
do_input key super+t
check "out of the layout it goes back to where the group was" \
	"[ \"\$(get g1 tiled)\" = 0 ] && [ \"\$(get g1 x)\" = $GX ] && [ \"\$(get g1 y)\" = $GY ]"
stop g1
stop g2
stop g3

# ---- a reload ------------------------------------------------------------------------------

wayland a size=400x300
x11 b size=300x200
do_input key super+g
kill -HUP "$pid"
sleep 0.5
shown "a group stays through a reload" b
hidden "both of it" a
stop a
stop b
finish

echo "PASS: tabs"
