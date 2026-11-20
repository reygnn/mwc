#!/bin/bash
# Headless test of orderings: a window's state changing under a grab, focus after a
# window goes, coming back to the right output, unplugging one, and the rest of what
# the review of mwc's states turned up. tests/snap.sh and tests/tile.sh cover the
# plain transitions.
# Output goes to build/state.log. Exits non-zero on failure.
LOG=build/state.log
. "$(dirname "$0")/lib.sh"

require_tools grim
for tool in build/vinput build/pngtool build/xdgc build/badclient build/layerc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
begin() { # [vinput width height]
	start_mwc 'exec sleep 300'
	start_vinput "$@"
	refresh
	B=$(global theme border)
	T=$(global theme title)
}
finish() {
	stop_vinput
	stop_mwc
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
settle() { sleep 0.45; }
focus_on() {
	local i
	for i in 1 2 3 4 5; do
		refresh
		[ "$(get "$1" active)" = 1 ] && return
		do_input key alt+Tab
	done
	fail "$1 never got the focus"
}
frame_w() { echo $(($(get "$1" w) + 2 * $(get "$1" bw))); }
title_x() { refresh; echo $(($(get "$1" x) + $(frame_w "$1") / 2)); }
title_y() { refresh; echo $(($(get "$1" y) + $(get "$1" bw) + $(get "$1" th) / 2)); }
grab_title() {
	settle
	do_input move "$(title_x "$1")" "$(title_y "$1")"
	do_input down left
}
drag_to() {
	grab_title "$1"
	do_input move "$2" "$3"
	do_input up left
}
no_grab_left() {
	check "$1" "[ \"\$(global cursor mode)\" = 0 ] && [ \"\$(global outline shown)\" = 0 ] && \
		[ -z \"\$(global feedback text)\" ]"
}

begin

# ---- fullscreen: nothing of the frame's offset stays ------------------------------

wayland fs size=300x200
do_input key alt+F11
check "fullscreen" "[ \"\$(get fs full)\" = 1 ] && [ \"\$(get fs w)\" = 1280 ]"
pixel_is "the client starts at the output's corner" 0 0 808080
pixel_is "and reaches its far edge" 1279 719 808080
do_input key alt+F11
check "back" "[ \"\$(get fs full)\" = 0 ]"
refresh
pixel_is "and the client is inside the frame again" \
	$(($(get fs x) + $(get fs bw) + 1)) $(($(get fs y) + $(get fs bw) + $(get fs th) + 1)) 808080
stop fs

# ---- a state change under a grab ends the grab ------------------------------------

# Tiling takes a floating window while it is being dragged.
do_input key super+t
wayland wide size=900x200 min=900x100
wayland mainw size=200x200
check "too wide, it floats beside the layout" "[ \"\$(get wide tiled)\" = 0 ] && [ \"\$(get mainw tiled)\" = 1 ]"
focus_on wide
grab_title wide
do_input move 700 400
check "the wide window is being moved" "[ \"\$(global cursor mode)\" = 1 ]"
stop mainw
check "the other one closes, the layout takes it" "[ \"\$(get wide tiled)\" = 1 ]"
no_grab_left "the grab gives way"
do_input up left
check "the release doesn't throw it out of its cell" \
	"[ \"\$(get wide x)\" = 8 ] && [ \"\$(get wide y)\" = 8 ]"
stop wide
do_input key super+t

# ---- who gets the focus ------------------------------------------------------------

wayland lonely size=300x200
do_input key alt+F3
check "Alt+F3 on the only window: it keeps the focus" "[ \"\$(get lonely kbfocus)\" = 1 ]"
do_input key alt+shift+Tab
check "Alt+Shift+Tab too" "[ \"\$(get lonely kbfocus)\" = 1 ]"
stop lonely

# Alt+Shift+Tab takes the dialog along to the back, and the focused window it is.
wayland gp size=300x200 dialog=gd
wayland gq size=300x200
drag_to gq 1000 500
settle
do_input move "$(title_x gp)" "$(title_y gp)"
do_input click left
check "the main window has the focus, its dialog above it" \
	"[ \"\$(get gp active)\" = 1 ] && [ \"\$(stack_pos gd)\" = 0 ]"
do_input key alt+shift+Tab
check "Alt+Shift+Tab: both go back, the dialog stays above its window" \
	"[ \"\$(get gq active)\" = 1 ] && [ \"\$(stack_pos gq)\" = 0 ] && [ \"\$(stack_pos gd)\" = 1 ] && \
	[ \"\$(stack_pos gp)\" = 2 ]"
stop gp
stop gq

# The main window closes and only a shaded one is left: it becomes the main window,
# and needs the focus too.
do_input key super+t
wayland sa size=300x200
wayland sb size=300x200
focus_on sa
do_input key alt+F9
check "shaded" "[ \"\$(get sa shaded)\" = 1 ]"
focus_on sb
do_input key alt+F4
check "the main window closes: the shaded one takes over, with the focus" \
	"[ -z \"\$(get sb x)\" ] && [ \"\$(get sa tile_main)\" = 1 ] && [ \"\$(get sa shaded)\" = 0 ] && \
	[ \"\$(get sa active)\" = 1 ]"
stop sa

# ---- rolled up without need, or for good ---------------------------------------------

wayland fine size=300x200
wayland huge size=900x600 min=900x600
wayland newer size=300x200
check "the one too large floats" "[ \"\$(get huge tiled)\" = 0 ]"
check "and nothing was rolled up on its account" "[ \"\$(get fine shaded)\" = 0 ] && [ \"\$(get newer shaded)\" = 0 ]"
stop huge
stop newer
stop fine

wayland keeper size=300x200
wayland tall size=100x100 min=100x690
wayland newest size=300x200
check "too tall for the column: it floats instead of being rolled up for good" \
	"[ \"\$(get tall tiled)\" = 0 ] && [ \"\$(get tall shaded)\" = 0 ]"
stop tall
stop newest
stop keeper

do_input key super+t

# ---- what Wayland clients are told ----------------------------------------------------

wayland told states=1 size=300x200
do_input key alt+F10
check "maximized" "[ \"\$(get told max)\" = 1 ]"
wait_for "the client hears of it" grep -q 'configure maximized' "$SMOKE_DIR/told.log"
: >"$SMOKE_DIR/told.log"
do_input key super+Left
check "from maximized straight to snapped" "[ \"\$(get told snap)\" != 0 ] && [ \"\$(get told max)\" = 0 ]"
wait_for "the client hears it isn't maximized any more" sh -c "tail -1 '$SMOKE_DIR/told.log' | grep -q 'configure free'"
do_input key alt+F5
stop told

# ---- Alt+F7 and Alt+F8 only warp for a grab ---------------------------------------------

wayland nowarp size=300x200
do_input move 500 500
do_input key alt+F10
do_input key alt+F8
check "Alt+F8 on a maximized window: the pointer stays" \
	"[ \"\$(global cursor x)\" = 500 ] && [ \"\$(global cursor mode)\" = 0 ]"
do_input key alt+F10
do_input key alt+F11
do_input key alt+F7
check "Alt+F7 on a fullscreen window: the pointer stays" \
	"[ \"\$(global cursor x)\" = 500 ] && [ \"\$(global cursor mode)\" = 0 ]"
do_input key alt+F11
stop nowarp

# ---- the menu button ---------------------------------------------------------------------

# The middle of the menu button.
menu_button() { refresh; do_input move $(($(get "$1" x) + B + T / 2)) $(($(get "$1" y) + B + T / 2)); }
wayland menubtn size=300x200
settle
menu_button menubtn
do_input click left
check "a click on the menu button opens the menu" "[ \"\$(global menu open)\" = 1 ]"
do_input click left
check "a double click closes the window" "[ \"\$(global menu open)\" = 0 ]"
check "and it is gone" "[ -z \"\$(get menubtn x)\" ]"
wait "$PID_menubtn" 2>/dev/null

wayland menubtn2 size=300x200
settle
menu_button menubtn2
do_input click left
sleep 0.5
do_input click left
check "two slow clicks only open and close the menu" \
	"[ \"\$(global menu open)\" = 0 ] && [ -n \"\$(get menubtn2 x)\" ]"
stop menubtn2

# ---- keys that have to work whatever else is going on -----------------------------------

# Super+Return with an exclusive surface: off, like the other bindings.
export MWC_TERMINAL="touch $SMOKE_DIR/terminal-started"
stop_vinput
stop_mwc
begin
WAYLAND_DISPLAY=wayland-0 build/layerc layer=overlay anchor=right size=50x50 keyboard=exclusive color=555555 \
	>"$SMOKE_DIR/lock.log" 2>&1 &
PID_lock=$!
wait_for "the surface holds the keyboard" grep -q keyboard-enter "$SMOKE_DIR/lock.log"
do_input key super+Return
sleep 0.3
[ -e "$SMOKE_DIR/terminal-started" ] && fail "the terminal binding should be off"
echo "ok: the terminal binding is off under an exclusive surface"

: >"$SMOKE_DIR/lock.log"
do_input key a
wait_for "keys still go to the exclusive surface" grep -q 'key 30' "$SMOKE_DIR/lock.log"
kill "$PID_lock"
wait "$PID_lock" 2>/dev/null
do_input key super+Return
wait_for "with it gone, the binding works" test -e "$SMOKE_DIR/terminal-started"
unset MWC_TERMINAL

# ---- an on-demand surface doesn't take the keyboard back -----------------------------------

wayland typer size=300x200
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top anchor=right size=50x50 keyboard=on-demand \
	repaint=200 color=666666 >"$SMOKE_DIR/demand.log" 2>&1 &
PID_demand=$!
wait_for "it has the keyboard on map" grep -q keyboard-enter "$SMOKE_DIR/demand.log"
settle
do_input move "$(title_x typer)" "$(title_y typer)"
do_input click left
check "a click takes it to the window" "[ \"\$(get typer kbfocus)\" = 1 ]"
sleep 0.7
check "and it stays there, however often the surface commits" "[ \"\$(get typer kbfocus)\" = 1 ]"
kill "$PID_demand"
wait "$PID_demand" 2>/dev/null
stop typer

# ---- popups repositioned, classes changed -----------------------------------------------------

WAYLAND_DISPLAY=wayland-0 build/badclient popup-far far >"$SMOKE_DIR/far.log" 2>&1 &
PID_far=$!
check "the client is mapped" "[ -n \"\$(get far x)\" ]"
refresh
do_input move $(($(get far x) + 50)) $(($(get far y) + 50))
do_input click left
wait_for "it repositions its popup far off-screen" grep -q '^popup-far' "$SMOKE_DIR/far.log"
FX=$(get far x)
check "the popup is kept on the output" \
	"[ \"\$(grep -c '^popup-at' '$SMOKE_DIR/far.log')\" -ge 2 ] && \
	[ \"\$(grep '^popup-at' '$SMOKE_DIR/far.log' | tail -1 | cut -d' ' -f2)\" -lt $((1280 - FX)) ]"
stop far

echo 'Mwm*renamed*activeBackground: #00aa00' >"$SMOKE_DIR/rc"
mkdir -p "$XDG_CONFIG_HOME/mwc"
cp "$SMOKE_DIR/rc" "$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
wayland changer late_app=renamed size=300x200
check "the new app_id is known" "[ -n \"\$(get renamed x)\" ]"
refresh
pixel_is "a late app_id gets its colours" $(($(get renamed x) + 3)) $(($(get renamed y) + 100)) 00aa00
kill "$PID_changer"
wait "$PID_changer" 2>/dev/null
check "the window is gone" "[ -z \"\$(get renamed x)\" ]"
rm "$XDG_CONFIG_HOME/mwc/mwmrc"
kill -HUP "$pid"
finish

# Who gets the focus after a dialog, in a session of its own: two more windows in the
# first one would move the cascade the menu button cases depend on.
begin
wayland parent size=300x200 dialog=dlg
check "main window and dialog" "[ -n \"\$(get parent x)\" ] && [ -n \"\$(get dlg x)\" ]"
wayland other size=300x200
drag_to other 1000 500
settle
do_input move "$(title_x dlg)" "$(title_y dlg)"
do_input click left
check "the dialog has the focus, the other window is above its own" \
	"[ \"\$(get dlg active)\" = 1 ] && [ \"\$(stack_pos other)\" -lt \"\$(stack_pos parent)\" ]"
do_input key alt+F4
check "closing a dialog gives the focus to its window, not the one above it" \
	"[ -z \"\$(get dlg x)\" ] && [ \"\$(get parent active)\" = 1 ]"
stop parent
stop other
finish

# ---- two outputs: back to the right one, and unplugging ----------------------------------------

export WLR_HEADLESS_OUTPUTS=2
begin 2560 720
wayland roam size=300x200
drag_to roam 2559 360
check "snapped on the second output" "[ \"\$(get roam x)\" = 1924 ]"
do_input key alt+F11
check "fullscreen on it" "[ \"\$(get roam x)\" = 1280 ] && [ \"\$(get roam full)\" = 1 ]"
do_input key alt+F11
check "and back snapped on the same output, not where it was before" \
	"[ \"\$(get roam x)\" = 1924 ] && [ \"\$(get roam full)\" = 0 ]"
do_input key super+t
check "tiling takes it to the first output" "[ \"\$(get roam x)\" -lt 1280 ]"
do_input key super+t
check "and it comes back on the second" "[ \"\$(get roam x)\" = 1924 ]"
do_input key alt+F5
check "restored" "[ \"\$(get roam snap)\" = 0 ]"

drag_to roam 1600 300
check "floating on the second output" "[ \"\$(get roam x)\" -gt 1280 ]"
wayland maxed2 size=300x200
drag_to maxed2 1600 300
do_input key alt+F10
check "another one maximized there" "[ \"\$(get maxed2 max)\" = 1 ] && [ \"\$(get maxed2 x)\" = 1280 ]"

kill -USR2 "$pid"
check "unplugged: the maximized window fills what is left" \
	"[ \"\$(get maxed2 x)\" = 0 ] && [ \$((\$(get maxed2 w) + 2 * $B)) = 1280 ]"
check "the floating one is in reach again" "[ \"\$(get roam x)\" -lt 1280 ]"
do_input key alt+F10
check "and un-maximized it comes back in reach too" \
	"[ \"\$(get maxed2 max)\" = 0 ] && [ \"\$(get maxed2 x)\" -lt 1280 ]"
stop roam
stop maxed2
finish

# ---- what windows change later ---------------------------------------------------------

begin
said() { grep -q "^$2" "$SMOKE_DIR/$1.log"; }

do_input key super+t
wayland hintl size=300x200 fixed_after="$SMOKE_DIR/hintl"
check "it tiles" "[ \"\$(get hintl tiled)\" = 1 ]"
touch "$SMOKE_DIR/hintl"
wait_for "fixed at its size after mapping" said hintl 'fixed'
check "and floats" "[ \"\$(get hintl tiled)\" = 0 ]"
stop hintl
do_input key super+t

# A dialog hidden with a window it stops belonging to.
wayland unp dialog=unpkid unparent=1
focus_on unp
do_input key alt+F9
check "the parent is an icon, its dialog hidden" "[ \"\$(get unp min)\" = 1 ] && [ \"\$(get unpkid hidden)\" = 1 ]"
check "no longer its dialog, it is there again" "[ \"\$(get unpkid hidden)\" = 0 ]"
stop unp
finish

# States asked for before a window has been anywhere belong to the output the pointer is on.
export WLR_HEADLESS_OUTPUTS=2
begin 2560 720
do_input move 1800 300
wayland premax2 pre=max size=300x200
check "maximized before the first commit, on the second output" \
	"[ \"\$(get premax2 max)\" = 1 ] && [ \"\$(get premax2 x)\" = 1280 ]"
do_input key alt+F11
check "fullscreen there" "[ \"\$(get premax2 full)\" = 1 ] && [ \"\$(get premax2 x)\" = 1280 ]"
do_input key alt+F11
check "and back to maximized there, not on the first" \
	"[ \"\$(get premax2 max)\" = 1 ] && [ \"\$(get premax2 x)\" = 1280 ]"
stop premax2
for state in max full; do
	wayland pl$state pre_late=$state size=300x200
	check "$state between the first configure and the first buffer, on the second output" \
		"[ \"\$(get pl$state x)\" = 1280 ] && [ \"\$(get pl$state y)\" = 0 ]"
	stop pl$state
done
# What a tiled window goes back to may go with an output it has already left.
wayland back size=300x200
drag_to back 1600 300
do_input key super+t
check "tiling takes it to the first output" "[ \"\$(get back x)\" -lt 1280 ]"
kill -USR2 "$pid"
do_input key super+t
check "the second output is gone, and it floats in reach" \
	"[ \"\$(get back tiled)\" = 0 ] && [ \"\$(get back x)\" -lt 1280 ]"
stop back
finish
unset WLR_HEADLESS_OUTPUTS

# A menu or a grab must not go on where there is no longer anything.
export WLR_HEADLESS_OUTPUTS=3
begin 3840 720
do_input move 3000 400
do_input click left
check "a root menu on the third output" "[ \"\$(global menu open)\" = 1 ]"
kill -USR2 "$pid"
check "unplugged, the menu is gone" "[ \"\$(global menu open)\" = 0 ]"
wayland strand size=300x200
drag_to strand 1600 300
do_input key alt+F7
check "a move from the keyboard on the second output" "[ \"\$(global cursor mode)\" != 0 ]"
do_input key Right
kill -USR2 "$pid"
check "unplugged, the grab is over" "[ \"\$(global cursor mode)\" = 0 ]"
do_input key Return
check "and the window is in reach" "[ \"\$(get strand x)\" -lt 1280 ]"
stop strand
finish
unset WLR_HEADLESS_OUTPUTS

echo "PASS: state"
