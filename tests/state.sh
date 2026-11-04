#!/bin/bash
# Headless test of orderings: a window's state changing under a grab, focus after a
# window goes, coming back to the right output, unplugging one, and the rest of what
# the review of mwc's states turned up. tests/snap.sh and tests/tile.sh cover the
# plain transitions.
# Output goes to build/state.log. Exits non-zero on failure.
LOG=build/state.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland grim
for tool in build/vinput build/pngtool build/xclient build/xdgc build/badclient build/layerc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
begin() { # [vinput width height]
	rm -f "$SMOKE_DIR/display"
	start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
	wait_for "the startup command ran" test -s "$SMOKE_DIR/display"
	X_DISPLAY=$(cat "$SMOKE_DIR/display")
	start_vinput "$@"
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

# ---- X11 fullscreen: nothing of the frame's offset stays --------------------------

x11 fs pos=100,100 size=300x200 color=ff0000
do_input key alt+F11
check "fullscreen" "[ \"\$(get fs full)\" = 1 ] && [ \"\$(get fs w)\" = 1280 ]"
pixel_is "the client starts at the output's corner" 0 0 ff0000
pixel_is "and reaches its far edge" 1279 719 ff0000
do_input key alt+F11
check "back" "[ \"\$(get fs full)\" = 0 ]"
pixel_is "and the client is inside the frame again" $((100 + B + 1)) $((100 + B + T + 1)) ff0000
stop fs

# ---- a state change under a grab ends the grab ------------------------------------

# Tiling takes a floating window while it is being dragged.
do_input key super+t
x11 wide pos=63,63 size=900x200 min=900x100
x11 mainw size=200x200
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

x11 parent pos=50,50 size=300x200 dialog=dlg
check "main window and dialog" "[ -n \"\$(get parent x)\" ] && [ -n \"\$(get dlg x)\" ]"
x11 other pos=600,300 size=300x200
settle
do_input move "$(title_x dlg)" "$(title_y dlg)"
do_input click left
check "the dialog has the focus" "[ \"\$(get dlg active)\" = 1 ]"
do_input key alt+F4
check "closing a dialog gives the focus to its window, not the one above it" \
	"[ -z \"\$(get dlg x)\" ] && [ \"\$(get parent active)\" = 1 ]"
stop parent
stop other

x11 lonely pos=100,100 size=300x200
do_input key alt+F3
check "Alt+F3 on the only window: it keeps the focus" "[ \"\$(get lonely kbfocus)\" = 1 ]"
do_input key alt+shift+Tab
check "Alt+Shift+Tab too" "[ \"\$(get lonely kbfocus)\" = 1 ]"
stop lonely

# Alt+Shift+Tab takes the dialog along to the back, and the focused window it is.
x11 gp pos=50,50 size=300x200 dialog=gd
x11 gq pos=600,300 size=300x200
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
x11 sa size=300x200
x11 sb size=300x200
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

x11 fine size=300x200
x11 huge size=900x600 min=900x600
x11 newer size=300x200
check "the one too large floats" "[ \"\$(get huge tiled)\" = 0 ]"
check "and nothing was rolled up on its account" "[ \"\$(get fine shaded)\" = 0 ] && [ \"\$(get newer shaded)\" = 0 ]"
stop huge
stop newer
stop fine

x11 keeper size=300x200
x11 tall size=100x100 min=100x690
x11 newest size=300x200
check "too tall for the column: it floats instead of being rolled up for good" \
	"[ \"\$(get tall tiled)\" = 0 ] && [ \"\$(get tall shaded)\" = 0 ]"
stop tall
stop newest
stop keeper

# A window taking no focus can still be unrolled.
x11 inopen size=300x200 input=0
x11 inmain size=300x200
refresh
RIGHT=$(($(get inopen x) + $(frame_w inopen) - $(get inopen bw) - $(get inopen th) - $(get inopen th) / 2))
settle
do_input move "$RIGHT" "$(title_y inopen)"
do_input click left
check "minimize shades it" "[ \"\$(get inopen shaded)\" = 1 ]"
settle
do_input move "$(title_x inopen)" "$(title_y inopen)"
do_input click left
check "a click unrolls it, focus or not" "[ \"\$(get inopen shaded)\" = 0 ]"
stop inmain
stop inopen
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

x11 nowarp pos=100,100 size=300x200
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

x11 menubtn pos=200,200 size=300x200
settle
do_input move $((200 + B + T / 2)) $((200 + B + T / 2))
do_input click left
check "a click on the menu button opens the menu" "[ \"\$(global menu open)\" = 1 ]"
do_input click left
check "a double click closes the window" "[ \"\$(global menu open)\" = 0 ]"
wait_for "the client is told" grep -q '^close main' "$SMOKE_DIR/menubtn.log"
check "and it is gone" "[ -z \"\$(get menubtn x)\" ]"

x11 menubtn2 pos=200,200 size=300x200
settle
do_input move $((200 + B + T / 2)) $((200 + B + T / 2))
do_input click left
sleep 0.5
do_input click left
check "two slow clicks only open and close the menu" \
	"[ \"\$(global menu open)\" = 0 ] && [ -n \"\$(get menubtn2 x)\" ]"
stop menubtn2

# Motif took Close away: no double click either.
x11 menubtn3 pos=200,200 size=300x200 motif=33,1
settle
do_input move $((200 + B + T / 2)) $((200 + B + T / 2))
do_input click left
do_input click left
sleep 0.2
check "without the Close function, a double click closes nothing" "[ -n \"\$(get menubtn3 x)\" ]"
stop menubtn3

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

# An X11 popup can't take the keyboard from it.
DISPLAY=$X_DISPLAY build/xclient class=orpop override=late pos=100,100 size=120x80 >/dev/null 2>&1 &
PID_orpop=$!
check "the X11 popup is there" "[ \"\$(global unmanaged count)\" = 1 ]"
: >"$SMOKE_DIR/lock.log"
do_input key a
wait_for "keys still go to the exclusive surface" grep -q 'key 30' "$SMOKE_DIR/lock.log"
kill "$PID_orpop"
wait "$PID_orpop" 2>/dev/null
kill "$PID_lock"
wait "$PID_lock" 2>/dev/null
do_input key super+Return
wait_for "with it gone, the binding works" test -e "$SMOKE_DIR/terminal-started"
unset MWC_TERMINAL

# ---- an on-demand surface doesn't take the keyboard back -----------------------------------

x11 typer pos=100,100 size=300x200
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

# ---- X11 requests ---------------------------------------------------------------------------

x11 mover pos=100,100 size=300x200 moveto=0,0
wait_for "the client asked to go to 0,0" grep -q '^moved' "$SMOKE_DIR/mover.log"
check "the frame goes there, the title stays on the screen" \
	"[ \"\$(get mover x)\" = 0 ] && [ \"\$(get mover y)\" = 0 ]"
stop mover

x11 semover pos=900,500 size=300x200 gravity=se moveto=800,400
wait_for "the client asked to move" grep -q '^moved' "$SMOKE_DIR/semover.log"
check "by its gravity: the corner goes there" \
	"[ \$((\$(get semover x) + \$(frame_w semover))) = 1100 ] && \
	[ \$((\$(get semover y) + \$(get semover fh))) = 600 ]"
stop semover

x11 normal pos=100,100 size=300x200 normalize=1
do_input key alt+F10
check "maximized" "[ \"\$(get normal max)\" = 1 ]"
wait_for "the client says NormalState" grep -q '^normalized' "$SMOKE_DIR/normal.log"
sleep 0.2
check "that doesn't un-maximize it" "[ \"\$(get normal max)\" = 1 ]"
do_input key alt+F10
stop normal

x11 iconic pos=100,100 size=300x200 iconic=1
check "WM_HINTS iconic: it starts as an icon" "[ \"\$(get iconic min)\" = 1 ]"
stop iconic

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
kill -HUP "$pid"
sleep 0.1
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

# ---- two outputs: back to the right one, and unplugging ----------------------------------------

export WLR_HEADLESS_OUTPUTS=2
begin 2560 720
x11 roam pos=100,100 size=300x200
drag_to roam 2559 360
check "snapped on the second output" "[ \"\$(get roam x)\" = 1920 ]"
do_input key alt+F11
check "fullscreen on it" "[ \"\$(get roam x)\" = 1280 ] && [ \"\$(get roam full)\" = 1 ]"
do_input key alt+F11
check "and back snapped on the same output, not where it was before" \
	"[ \"\$(get roam x)\" = 1920 ] && [ \"\$(get roam full)\" = 0 ]"
do_input key super+t
check "tiling takes it to the first output" "[ \"\$(get roam x)\" -lt 1280 ]"
do_input key super+t
check "and it comes back on the second" "[ \"\$(get roam x)\" = 1920 ]"
do_input key alt+F5
check "restored" "[ \"\$(get roam snap)\" = 0 ]"

drag_to roam 1600 300
check "floating on the second output" "[ \"\$(get roam x)\" -gt 1280 ]"
x11 maxed2 pos=1400,100 size=300x200
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

# ---- X windows withdrawn and shown again, and what they change later --------------------

begin
said() { grep -q "^$2" "$SMOKE_DIR/$1.log"; }

x11 drift pos=100,100 rehide=3
wait_for "it has been hidden and shown three times" said drift 'reshown 3'
check "it hasn't moved" "[ \"\$(get drift x)\" = 100 ] && [ \"\$(get drift y)\" = 100 ]"
stop drift

x11 fsdrop state=fullscreen rehide=1 rehide_drop=1 size=300x200
wait_for "shown again without the state" said fsdrop 'reshown 1'
check "it isn't fullscreen any longer" "[ \"\$(get fsdrop full)\" = 0 ] && [ \"\$(get fsdrop frame)\" = 1 ] && \
	[ \"\$(get fsdrop w)\" = 300 ] && [ \"\$(get fsdrop h)\" = 200 ]"
stop fsdrop

# The window manager takes the states away from a withdrawn window itself, as EWMH says.
x11 fsplain state=fullscreen rehide=1 size=300x200
wait_for "shown again" said fsplain 'reshown 1'
check "so it comes back without them" "[ \"\$(get fsplain full)\" = 0 ] && [ \"\$(get fsplain w)\" = 300 ]"
stop fsplain

# wlroots only forgets fullscreen that way. Maximized stays, for the window as well.
x11 mxkeep state=maximized rehide=1 size=300x200
wait_for "shown again" said mxkeep 'reshown 1'
check "still maximized" "[ \"\$(get mxkeep max)\" = 1 ] && [ \$((\$(get mxkeep w) + 2 * B)) = 1280 ]"
do_input key alt+F10
check "and it can leave, for its own size" "[ \"\$(get mxkeep max)\" = 0 ] && [ \"\$(get mxkeep w)\" = 300 ]"
stop mxkeep

x11 minre rehide=1 rehide_ms=1500
do_input key alt+F9
check "minimized" "[ \"\$(get minre min)\" = 1 ]"
wait_for "withdrawn as an icon and shown again" said minre 'reshown 1'
check "it is a window again" "[ \"\$(get minre min)\" = 0 ] && [ \"\$(get minre active)\" = 1 ]"
said minre 'wm_state 1' || fail "it still says it is an icon"
echo "ok: and says so"
stop minre

x11 axisoff state=maximized script=-v,-h size=300x200
wait_for "one axis taken back, then the other" said axisoff 'sent 1'
check "the first ends it" "[ \"\$(get axisoff max)\" = 0 ] && [ \"\$(get axisoff w)\" = 300 ]"
wait_for "the second" said axisoff 'sent 2'
sleep 0.3
check "and the second changes nothing" "[ \"\$(get axisoff max)\" = 0 ] && [ \"\$(get axisoff w)\" = 300 ]"
stop axisoff
x11 axison script=+v size=300x200
check "one axis asked for is enough" "[ \"\$(get axison max)\" = 1 ] && [ \$((\$(get axison w) + 2 * B)) = 1280 ]"
stop axison

x11 selfocus dialog=selfd focus_main=1
wait_for "a program moved the focus to its own other window" said selfocus 'focused main'
check "and that one shows it" "[ \"\$(get selfocus active)\" = 1 ] && [ \"\$(get selfd active)\" = 0 ] && \
	[ \"\$(get selfocus kbfocus)\" = 1 ]"
stop selfocus

x11 motifl motif_later=0
wait_for "functions taken away after mapping" said motifl 'motif set'
do_input key alt+F7
check "no move any more" "[ \"\$(global cursor mode)\" = 0 ]"
stop motifl

do_input key super+t
x11 hintl hints_later=300x200 size=300x200
check "it tiles" "[ \"\$(get hintl tiled)\" = 1 ]"
wait_for "fixed at a size after mapping" said hintl 'hints set'
check "and floats" "[ \"\$(get hintl tiled)\" = 0 ]"
stop hintl
do_input key super+t

x11 kidp dialog=kid dialog_ms=1500
do_input key alt+F9
check "the parent is an icon" "[ \"\$(get kidp min)\" = 1 ]"
check "a dialog appearing now stays hidden" "[ \"\$(get kid hidden)\" = 1 ] && [ \"\$(get kid active)\" = 0 ]"
refresh
do_input move $(($(get kidp icon_x) + 30)) $(($(get kidp icon_y) + 30))
do_input click left
do_input click left
check "and comes back with it" "[ \"\$(get kidp min)\" = 0 ] && [ \"\$(get kid hidden)\" = 0 ]"
stop kidp

x11 latin latin1_title=1
sleep 0.5
grep -aq 'Invalid UTF-8' "$LOG" && fail "a Latin-1 title reaches Pango as it is"
echo "ok: a Latin-1 title is converted"
stop latin

x11 extents report=1
wait_for "a program asks what is supported" said extents 'supported frame extents'
said extents 'supported frame extents 1' || fail "_NET_FRAME_EXTENTS isn't listed"
echo "ok: _NET_FRAME_EXTENTS is listed"
stop extents

x11 area workarea=1
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top anchor=top,left,right size=0x30 exclusive=30 \
	>"$SMOKE_DIR/panel.log" 2>&1 &
PID_panel=$!
wait_for "an idle X client hears of a panel" grep -q 'workarea 0 30 1280 690' "$SMOKE_DIR/area.log"
kill "$PID_panel"
wait "$PID_panel" 2>/dev/null
stop area
finish

# A menu or a grab must not go on where there is no longer anything.
export WLR_HEADLESS_OUTPUTS=3
begin 3840 720
do_input move 3000 400
do_input click left
check "a root menu on the third output" "[ \"\$(global menu open)\" = 1 ]"
kill -USR2 "$pid"
check "unplugged, the menu is gone" "[ \"\$(global menu open)\" = 0 ]"
x11 strand pos=1400,100 size=300x200
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
