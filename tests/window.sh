#!/bin/bash
# Headless test of window behaviour: size hints, Motif hints, dialogs, window
# types, X11 panels, gravity and focus rules, with clients that can ask for
# each of them (tests/xclient.c, tests/xdgc.c).
# Output goes to build/window.log. Exits non-zero on failure.
LOG=build/window.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland grim
for tool in build/vinput build/pngtool build/xclient build/xdgc build/layerc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
wait_for "the startup command ran" test -s "$SMOKE_DIR/display"
X_DISPLAY=$(cat "$SMOKE_DIR/display")
start_vinput
refresh
B=$(global theme border)
T=$(global theme title)
THIN=3 # bevel 2 + 1, from frame_border()

PIDS=()
# x11 <name> key=value...: an X11 client, logging to $SMOKE_DIR/<name>.events.
x11() {
	local name=$1
	shift
	DISPLAY=$X_DISPLAY build/xclient class="$name" "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "PID_$name=$!"
}
wayland() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$name" "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "PID_$name=$!"
}
stop() {
	local var="PID_$1"
	kill "${!var}" 2>/dev/null
	wait "${!var}" 2>/dev/null
	check "$1 is gone" "[ -z \"\$(get $1 x)\" ]"
}
mapped() { check "$1 is mapped" "[ -n \"\$(get $1 x)\" ]"; }
settle() { sleep 0.45; }

# The left and top of the frame, and the size of its parts.
border() { get "$1" bw; }
title() { get "$1" th; }
outer_w() { echo $(($(get "$1" w) + 2 * $(border "$1"))); }
outer_h() { echo $(($(get "$1" h) + 2 * $(border "$1") + $(title "$1"))); }

drag() { # from_x from_y to_x to_y [button]
	local button=${5:-left}
	do_input move "$1" "$2"
	do_input down "$button"
	do_input move "$3" "$4"
	do_input up "$button"
}

# Drag the bottom-right corner by dx dy.
resize_by() { # app dx dy
	refresh
	local x=$(($(get "$1" x) + $(outer_w "$1") - 2))
	local y=$(($(get "$1" y) + $(outer_h "$1") - 2))
	drag "$x" "$y" $((x + $2)) $((y + $3))
}

sensitive_is() { # description window expected, leaving out the entries for tabs and desktops
	do_input key alt+space
	check "$1" "[ \"\$(global menu sensitive | tr -d TePD1234O)\" = $3 ]"
	do_input key Escape
}

# ---- size hints: X11 ----------------------------------------------------------

x11 hinted size=300x200 min=200x150 max=400x300
mapped hinted
resize_by hinted 500 500
check "X11: the maximum size holds" "[ \"\$(get hinted w)\" = 400 ] && [ \"\$(get hinted h)\" = 300 ]"
resize_by hinted -500 -500
check "X11: the minimum size holds" "[ \"\$(get hinted w)\" = 200 ] && [ \"\$(get hinted h)\" = 150 ]"
do_input key alt+F10
check "X11: maximizing stops at the maximum size" \
	"[ \"\$(get hinted max)\" = 1 ] && [ \"\$(get hinted w)\" = 400 ] && [ \"\$(get hinted h)\" = 300 ]"
do_input key alt+F10
check "X11: and goes back" "[ \"\$(get hinted max)\" = 0 ] && [ \"\$(get hinted w)\" = 200 ]"
stop hinted

x11 stepped size=300x200 base=20x10 inc=30x40
mapped stepped
resize_by stepped 47 53
check "X11: width and height go in steps above the base size" \
	"[ \$(((\$(get stepped w) - 20) % 30)) = 0 ] && [ \$(((\$(get stepped h) - 10) % 40)) = 0 ] && \
	[ \"\$(get stepped w)\" -gt 300 ]"
# From the top left, the steps must not move the bottom right corner.
refresh
right=$(($(get stepped x) + $(get stepped w))) bottom=$(($(get stepped y) + $(get stepped h)))
drag $(($(get stepped x) + 2)) $(($(get stepped y) + 2)) $(($(get stepped x) + 47)) $(($(get stepped y) + 53))
check "X11: steps from the top left keep the opposite corner" \
	"[ \$((\$(get stepped x) + \$(get stepped w))) = $right ] && \
	[ \$((\$(get stepped y) + \$(get stepped h))) = $bottom ]"
stop stepped

x11 square size=200x200 aspect=1/1
mapped square
resize_by square 150 30
check "X11: the aspect ratio holds" \
	"[ \"\$(get square w)\" = \"\$(get square h)\" ]"
stop square

# One edge at a time: the other side follows, and what is opposite stays.
right_edge() { # app dx
	refresh
	local x=$(($(get "$1" x) + $(outer_w "$1") - 2)) y=$(($(get "$1" y) + $(outer_h "$1") / 2))
	drag "$x" "$y" $((x + $2)) "$y"
}
bottom_edge() { # app dy
	refresh
	local x=$(($(get "$1" x) + $(outer_w "$1") / 2)) y=$(($(get "$1" y) + $(outer_h "$1") - 2))
	drag "$x" "$y" "$x" $((y + $2))
}
left_edge() { # app dx
	refresh
	local x=$(($(get "$1" x) + 1)) y=$(($(get "$1" y) + $(outer_h "$1") / 2))
	drag "$x" "$y" $((x + $2)) "$y"
}
size_is() { check "$1" "[ \"\$(get $2 w)\" = $3 ] && [ \"\$(get $2 h)\" = $4 ]"; }
x11 wide pos=100,100 size=400x200 aspect=2/1
mapped wide
refresh
WX=$(get wide x) WY=$(get wide y)
right_edge wide 200
size_is "X11: the right edge makes it wider, and the height follows" wide 600 300
check "the top left stays" "[ \"\$(get wide x)\" = $WX ] && [ \"\$(get wide y)\" = $WY ]"
right_edge wide -300
size_is "and narrower" wide 300 150
bottom_edge wide 80
size_is "the bottom edge: the width follows" wide 460 230
check "the top left stays" "[ \"\$(get wide x)\" = $WX ] && [ \"\$(get wide y)\" = $WY ]"
refresh
RIGHT=$(($(get wide x) + $(outer_w wide)))
left_edge wide 60
size_is "the left edge" wide 400 200
check "the right side stays" "[ \$((\$(get wide x) + \$(outer_w wide))) = $RIGHT ]"
stop wide

# Ratios the hints allow but no window can follow: the other side stops at what X11
# takes. 1:100 asked for 50000; 1:10^9 overflowed an int (UBSan in make check) and
# came out as height 1.
for ratio in 100 1000000000; do
	x11 extreme pos=100,100 size=300x300 aspect=1/$((2 * ratio)),1/$ratio sizes=1
	mapped extreme
	right_edge extreme 200
	check "X11: aspect ratio 1:$ratio, the height X11 can take at most" \
		"grep '^size' \"\$SMOKE_DIR/extreme.events\" | tail -1 | grep -qx 'size [0-9]*x32767'"
	stop extreme
done

# ---- size hints: Wayland ------------------------------------------------------

wayland wlimit size=300x200 min=200x150 max=400x300
mapped wlimit
resize_by wlimit 500 500
check "Wayland: the maximum size holds" "[ \"\$(get wlimit w)\" = 400 ] && [ \"\$(get wlimit h)\" = 300 ]"
resize_by wlimit -500 -500
check "Wayland: the minimum size holds" "[ \"\$(get wlimit w)\" = 200 ] && [ \"\$(get wlimit h)\" = 150 ]"
stop wlimit

wayland wfixed size=300x200 min=300x200 max=300x200
mapped wfixed
check "Wayland: a fixed size means a thin border" "[ \"\$(border wfixed)\" = $THIN ]"
sensitive_is "Wayland: and no Size or Maximize" wfixed MnLC
stop wfixed

# A minimum size of INT32_MAX, which wlroots lets through: what mwc asks for stays at
# its own limit, 2^20, and the frame around it adds up.
wayland whuge size=300x200 min=2147483647x2147483647 own_size=1 states=1
mapped whuge
do_input key alt+F10
wait_for "Wayland: maximized" grep -q '^configure maximized' "$SMOKE_DIR/whuge.events"
check "Wayland: a huge minimum size, capped" \
	"grep '^configure maximized' \"\$SMOKE_DIR/whuge.events\" | tail -1 | \
	grep -qx 'configure maximized 1048576x1048576'"
do_input key alt+F10
check "and back" "[ \"\$(get whuge max)\" = 0 ]"
refresh
X=$(get whuge x) Y=$(get whuge y)
do_input key super+t
check "tiling leaves it out, where it was" \
	"[ \"\$(global tiling on)\" = 1 ] && [ \"\$(get whuge tiled)\" = 0 ] && \
	[ \"\$(get whuge x)\" = $X ] && [ \"\$(get whuge y)\" = $Y ]"
do_input key super+t
stop whuge

# ---- a window that cannot be resized ------------------------------------------

x11 fixed size=300x200 min=300x200 max=300x200
mapped fixed
check "a fixed size means a thin border" "[ \"\$(border fixed)\" = $THIN ] && [ \"\$(title fixed)\" = $T ]"
sensitive_is "and no Size or Maximize in the menu" fixed MnLC
do_input key alt+F10
do_input key alt+F8
sleep 0.2
check "Alt+F10 and Alt+F8 do nothing" \
	"[ \"\$(get fixed max)\" = 0 ] && [ \"\$(global cursor mode)\" = 0 ]"
refresh
resize_by fixed 100 100
check "its border doesn't resize" "[ \"\$(get fixed w)\" = 300 ] && [ \"\$(get fixed h)\" = 200 ]"
stop fixed

# ---- _MOTIF_WM_HINTS ----------------------------------------------------------

# All functions except Close (32): 1 is the ALL bit, the rest is taken away.
x11 noclose motif=33,1
mapped noclose
sensitive_is "Motif: no Close in the menu" noclose MSnxL
do_input key alt+F4
sleep 0.2
grep -q close "$SMOKE_DIR/noclose.events" && fail "Motif: Alt+F4 must not close it"
echo "ok: Motif: Alt+F4 doesn't close it"
stop noclose

# Only moving (4).
x11 moveonly motif=4,1
mapped moveonly
check "Motif: no resize handles without the function" "[ \"\$(border moveonly)\" = $THIN ]"
sensitive_is "Motif: only Move and Lower remain" moveonly ML
do_input key alt+F9
sleep 0.2
check "Motif: Alt+F9 does nothing" "[ \"\$(get moveonly min)\" = 0 ]"
stop moveonly

# Not movable (all but 4).
x11 nomove motif=5,1
mapped nomove
settle
refresh
tx=$(($(get nomove x) + 100)) ty=$(($(get nomove y) + 12)) x0=$(get nomove x)
drag "$tx" "$ty" $((tx + 50)) $((ty + 50))
check "Motif: the title doesn't move it" "[ \"\$(get nomove x)\" = $x0 ]"
do_input hold alt
drag "$tx" $((ty + 40)) $((tx + 50)) $((ty + 90))
do_input release alt
check "Motif: nor does Alt+btn1" "[ \"\$(get nomove x)\" = $x0 ]"
stop nomove

# No decorations at all (0, as client-side decorated toolkits ask for).
x11 nodecor motif=1,0
mapped nodecor
check "Motif: no frame at all" "[ \"\$(border nodecor)\" = 0 ] && [ \"\$(title nodecor)\" = 0 ]"
stop nodecor

# Border and title (2 | 8), no buttons.
x11 nobuttons motif=1,10
mapped nobuttons
check "Motif: title without resize handles or buttons" \
	"[ \"\$(border nobuttons)\" = $THIN ] && [ \"\$(title nobuttons)\" = $T ]"
settle
refresh
# Where the menu button would be, the title bar starts.
do_input move $(($(get nobuttons x) + B + 2)) $(($(get nobuttons y) + B + 2))
do_input click left
sleep 0.1
check "Motif: where the menu button would be, nothing opens" "[ \"\$(global menu open)\" = 0 ]"
stop nobuttons

# Title only (8), no border.
x11 titleonly motif=1,8
mapped titleonly
check "Motif: title without a border" "[ \"\$(border titleonly)\" = 0 ] && [ \"\$(title titleonly)\" = $T ]"
stop titleonly

# ---- dialogs: X11 -------------------------------------------------------------

x11 parent size=600x400 dialog=child
check "dialog and main window are mapped" "[ -n \"\$(get parent x)\" ] && [ -n \"\$(get child x)\" ]"
check "the dialog gets the focus" "[ \"\$(get child kbfocus)\" = 1 ]"
check "the dialog is centred over its window" \
	"[ \$((\$(get child x) + \$(outer_w child) / 2)) = \$((\$(get parent x) + \$(outer_w parent) / 2)) ] && \
	[ \$((\$(get child y) + \$(outer_h child) / 2)) = \$((\$(get parent y) + \$(outer_h parent) / 2)) ]"
sensitive_is "a dialog has no Minimize or Maximize" child MSLC

do_input move $(($(get parent x) + 100)) $(($(get parent y) + 12))
settle
do_input click left
check "a click on the main window focuses it, the dialog stays above" \
	"[ \"\$(get parent kbfocus)\" = 1 ] && [ \"\$(stack_pos child)\" = 0 ] && [ \"\$(stack_pos parent)\" = 1 ]"

x11 other pos=800,300 size=300x200
mapped other
check "another window goes to the front" "[ \"\$(stack_pos other)\" = 0 ]"
do_input move $(($(get parent x) + 100)) $(($(get parent y) + 12))
settle
do_input click left
check "raising the main window brings its dialog along" \
	"[ \"\$(stack_pos child)\" = 0 ] && [ \"\$(stack_pos parent)\" = 1 ] && [ \"\$(stack_pos other)\" = 2 ]"

do_input key alt+F9
check "minimizing the main window takes the dialog along, without an icon" \
	"[ \"\$(get parent min)\" = 1 ] && [ \"\$(get child min)\" = 0 ] && [ \"\$(get child hidden)\" = 1 ]"
check "the focus goes to the other window" "[ \"\$(get other kbfocus)\" = 1 ]"
do_input key alt+Tab
sleep 0.1
check "Alt+Tab skips the hidden dialog" "[ \"\$(get other active)\" = 1 ] && [ \"\$(get child active)\" = 0 ]"
do_input move $(($(get parent icon_x) + 30)) $(($(get parent icon_y) + 30))
settle
do_input click left
do_input click left
check "restoring it brings the dialog back, above it" \
	"[ \"\$(get parent min)\" = 0 ] && [ \"\$(get child hidden)\" = 0 ] && \
	[ \"\$(stack_pos child)\" = 0 ] && [ \"\$(stack_pos parent)\" = 1 ]"
stop other

do_input move $(($(get child x) + 70)) $(($(get child y) + 12))
settle
do_input click left
check "the dialog has the focus" "[ \"\$(get child kbfocus)\" = 1 ]"
do_input key alt+F4
check "closing the dialog gives the focus to its window" \
	"[ -z \"\$(get child x)\" ] && [ \"\$(get parent kbfocus)\" = 1 ]"
stop parent

# Closing a minimized window must not leave its dialog hidden for good.
x11 orphanmain size=400x300 dialog=orphan
check "main window and dialog are mapped" "[ -n \"\$(get orphanmain x)\" ] && [ -n \"\$(get orphan x)\" ]"
bring_main() { for _ in 1 2 3; do refresh; [ "$(get orphanmain active)" = 1 ] && return; do_input key alt+Tab; done; }
bring_main
do_input key alt+F9
check "the dialog is hidden" "[ \"\$(get orphan hidden)\" = 1 ]"
do_input move $(($(get orphanmain icon_x) + 30)) $(($(get orphanmain icon_y) + 30))
settle
do_input click right
do_input key c
check "closing the minimized window brings the dialog back" \
	"[ -z \"\$(get orphanmain x)\" ] && [ \"\$(get orphan hidden)\" = 0 ] && [ \"\$(get orphan kbfocus)\" = 1 ]"
do_input key alt+F4
check "the dialog closes" "[ -z \"\$(get orphan x)\" ]"

# ---- dialogs: Wayland ---------------------------------------------------------

wayland wparent size=600x400 dialog=wchild
check "Wayland: dialog and main window are mapped" "[ -n \"\$(get wparent x)\" ] && [ -n \"\$(get wchild x)\" ]"
check "Wayland: the dialog is centred over its window" \
	"[ \$((\$(get wchild x) + \$(outer_w wchild) / 2)) = \$((\$(get wparent x) + \$(outer_w wparent) / 2)) ]"
sensitive_is "Wayland: a dialog has no Minimize or Maximize" wchild MSLC
do_input move $(($(get wparent x) + 100)) $(($(get wparent y) + 12))
settle
do_input key alt+Tab
check "Wayland: the main window has the focus" "[ \"\$(get wparent active)\" = 1 ]"
do_input key alt+F9
check "Wayland: minimizing takes the dialog along" \
	"[ \"\$(get wparent min)\" = 1 ] && [ \"\$(get wchild hidden)\" = 1 ]"
do_input move $(($(get wparent icon_x) + 30)) $(($(get wparent icon_y) + 30))
settle
do_input click left
do_input click left
check "Wayland: restoring brings it back" \
	"[ \"\$(get wparent min)\" = 0 ] && [ \"\$(get wchild hidden)\" = 0 ] && [ \"\$(stack_pos wchild)\" = 0 ]"
stop wparent

# ---- placement ----------------------------------------------------------------

x11 placed pos=100,120 size=300x200
mapped placed
check "USPosition: the frame goes where the client asked" \
	"[ \"\$(get placed x)\" = 100 ] && [ \"\$(get placed y)\" = 120 ]"
stop placed

x11 southeast pos=900,500 size=300x200 gravity=se
mapped southeast
check "SouthEast gravity: the frame's corner is the reference" \
	"[ \$((\$(get southeast x) + \$(outer_w southeast))) = 1200 ] && \
	[ \$((\$(get southeast y) + \$(outer_h southeast))) = 700 ]"
stop southeast

x11 staticg pos=300,200 size=300x200 gravity=static
mapped staticg
check "Static gravity: the client stays, the frame goes around it" \
	"[ \$((\$(get staticg x) + $B)) = 300 ] && [ \$((\$(get staticg y) + $B + $T)) = 200 ]"
stop staticg

x11 splash type=splash size=300x200
mapped splash
check "a splash screen has no frame and no focus, and is centred" \
	"[ \"\$(border splash)\" = 0 ] && [ \"\$(title splash)\" = 0 ] && [ \"\$(get splash active)\" = 0 ] && \
	[ \"\$(get splash x)\" = 490 ] && [ \"\$(get splash y)\" = 260 ]"
stop splash

# ---- states asked for before mapping --------------------------------------------

# More than once, because whether wlroots sees them on its own depends on timing.
for i in 1 2 3 4 5; do
	x11 startfull state=fullscreen
	check "starts fullscreen, try $i" "[ \"\$(get startfull full)\" = 1 ] && [ \"\$(get startfull w)\" = 1280 ]"
	stop startfull
	x11 startmax state=maximized
	check "starts maximized, try $i" "[ \"\$(get startmax max)\" = 1 ] && [ \"\$(outer_w startmax)\" = 1280 ]"
	stop startmax
	x11 startbelow state=below
	check "starts below, try $i" "[ \"\$(get startbelow below)\" = 1 ]"
	stop startbelow
	x11 startsticky state=sticky
	check "starts on all desktops, try $i" "[ \"\$(get startsticky everywhere)\" = 1 ]"
	stop startsticky
done

# ---- window types and stacking ------------------------------------------------

x11 normal pos=100,100 size=600x400 color=0000ff
mapped normal

x11 tool type=utility pos=800,100
mapped tool
sensitive_is "a utility window has no Minimize or Maximize" tool MSLC
stop tool

x11 note type=notification pos=200,150 size=100x60 color=ff0000
mapped note
check "a notification has no frame and no focus" \
	"[ \"\$(border note)\" = 0 ] && [ \"\$(get note active)\" = 0 ] && [ \"\$(get normal active)\" = 1 ]"
pixel_is "a notification lies above normal windows" 220 170 ff0000
do_input key alt+Tab
sleep 0.1
check "Alt+Tab skips a notification" "[ \"\$(get note active)\" = 0 ]"
stop note

x11 under state=below pos=150,150 size=100x60 color=00ff00
mapped under
check "the window has the focus" "[ \"\$(get under active)\" = 1 ]"
pixel_is "_NET_WM_STATE_BELOW stays below normal windows, even with the focus" 170 200 0000ff
stop under
check "the focus returns to the normal window" "[ \"\$(get normal active)\" = 1 ]"

x11 over state=above pos=150,150 size=100x60 color=ffff00
mapped over
do_input key alt+Tab
check "the normal window has the focus" "[ \"\$(get normal active)\" = 1 ]"
pixel_is "_NET_WM_STATE_ABOVE stays above normal windows" 170 200 ffff00
stop over

x11 wall type=desktop pos=0,0 size=1280x720 color=00ffff
mapped wall
check "a desktop window has no frame and no focus" \
	"[ \"\$(border wall)\" = 0 ] && [ \"\$(get wall x)\" = 0 ] && [ \"\$(get wall active)\" = 0 ]"
pixel_is "a desktop window lies under everything" 170 200 0000ff
pixel_is "and shows where nothing else is" 1200 600 00ffff
stop wall

# ---- X11 panels ---------------------------------------------------------------

x11 work workarea=1 pos=500,500 size=100x60
wait_for "_NET_WORKAREA is set" grep -q 'workarea 0 0 1280 720' "$SMOKE_DIR/work.events"
stop work

x11 panel type=dock pos=0,0 size=1280x40 strut=0,0,40,0 color=ff00ff
mapped panel
check "a dock has no frame and reserves its strut" \
	"[ \"\$(border panel)\" = 0 ] && [ \"\$(global output uy)\" = 40 ] && [ \"\$(global output uh)\" = 680 ]"
check "the normal window keeps the focus" "[ \"\$(get normal active)\" = 1 ] && [ \"\$(get panel active)\" = 0 ]"
pixel_is "the dock is drawn" 640 20 ff00ff

do_input key alt+F10
check "a maximized window stays out of the strut" \
	"[ \"\$(get normal max)\" = 1 ] && [ \"\$(get normal y)\" = 40 ] && [ \"\$(outer_h normal)\" = 680 ]"
pixel_is "the dock stays above it" 640 20 ff00ff
do_input key alt+Tab
sleep 0.1
check "Alt+Tab skips a dock" "[ \"\$(get panel active)\" = 0 ]"

x11 work workarea=1 pos=500,500 size=100x60
wait_for "_NET_WORKAREA leaves the strut out" grep -q 'workarea 0 40 1280 680' "$SMOKE_DIR/work.events"
stop work

stop panel
check "without the dock, the strut is gone and the window fills the room" \
	"[ \"\$(global output uy)\" = 0 ] && [ \"\$(get normal y)\" = 0 ] && [ \"\$(outer_h normal)\" = 720 ]"
do_input key alt+F10
check "the normal window is back" "[ \"\$(get normal max)\" = 0 ]"

# ---- input focus ---------------------------------------------------------------

x11 noinput input=0 pos=800,100 size=300x200
mapped noinput
check "a window without input focus doesn't get it on map" \
	"[ \"\$(get noinput active)\" = 0 ] && [ \"\$(get normal active)\" = 1 ]"
do_input move $(($(get noinput x) + 100)) $(($(get noinput y) + 12))
settle
do_input click left
check "nor when clicked, but it comes to the front" \
	"[ \"\$(get noinput active)\" = 0 ] && [ \"\$(get normal kbfocus)\" = 1 ] && [ \"\$(stack_pos noinput)\" = 0 ]"
stop noinput
stop normal

# ---- a place from mwmrc --------------------------------------------------------
RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
cat >"$RC" <<'EOF'
Mwm*gcorner*geometry:   -0-0
Mwm*gusers*geometry:    -10-20
Mwm*gleft*geometry:     +20+30
Mwm*gbig*geometry:      -0-0
Mwm*gdialog*geometry:   +0+0
Mwm*gpanel*geometry:    +0+0
Mwm*gsized*geometry:    100x100-0-0
Mwm*ghalf*geometry:     -0
Mwm*gkept*geometry:     -10-20
Mwm*gplain*geometry:    -0-0
Mwm*gutil*geometry:     +0+0
Mwm*gxdialog*geometry:  +0+0
Mwm*gtab*geometry:      -0-0
Mwm*gmaxed*geometry:    -0-0
Mwm*gpointer*geometry:  pointer
Mwm*gpedge*geometry:    Pointer
Mwm*gwgrow*geometry:    -0-0
Mwm*gxgrow*geometry:    -0-0
Mwm*gwleft*geometry:    +20+30
Mwm*gwmoved*geometry:   -0-0
Mwm*gwsized*geometry:   -0-0
Mwm*gwframe*geometry:   -0-0
Mwm*gwfleft*geometry:   +20+30
Mwm*gwfont*geometry:    -0-0
Mwm*gxfont*geometry:    -0-0
EOF
reload_mwc

right() { echo $(($(get "$1" x) + $(outer_w "$1"))); }
bottom() { echo $(($(get "$1" y) + $(outer_h "$1"))); }

wayland gcorner
mapped gcorner
check "-0-0 puts a Wayland window into the corner" \
	"[ \"\$(right gcorner)\" = 1280 ] && [ \"\$(bottom gcorner)\" = 720 ]"
drag $(($(get gcorner x) + 100)) $(($(get gcorner y) + 12)) 500 300
check "it can be moved away" "[ \"\$(right gcorner)\" != 1280 ]"
moved_x=$(get gcorner x)
reload_mwc
check "a reload leaves it there" "[ \"\$(get gcorner x)\" = $moved_x ]"
stop gcorner

x11 gusers pos=100,100
mapped gusers
check "distances from the edges, before the place an X11 program asks for" \
	"[ \"\$(right gusers)\" = 1270 ] && [ \"\$(bottom gusers)\" = 700 ]"
stop gusers

wayland gleft
mapped gleft
check "+20+30 counts from the left and top" \
	"[ \"\$(get gleft x)\" = 20 ] && [ \"\$(get gleft y)\" = 30 ]"
stop gleft

x11 gbig size=2000x100
mapped gbig
check "too wide for the screen: reachable, and still at the bottom" \
	"[ \"\$(get gbig x)\" = 0 ] && [ \"\$(bottom gbig)\" = 720 ]"
stop gbig

x11 gkept pos=100,100 rehide=1 rehide_ms=2000
mapped gkept
check "the entry goes first when it appears" "[ \"\$(right gkept)\" = 1270 ]"
settle
drag $(($(get gkept x) + 100)) $(($(get gkept y) + 12)) 500 300
check "it can be moved" "[ \"\$(right gkept)\" != 1270 ]"
moved_x=$(get gkept x)
moved_y=$(get gkept y)
wait_for "gkept was hidden and shown" grep -q "reshown 1" "$SMOKE_DIR/gkept.events"
settle
check "shown again, a program with a position of its own stays where it was" \
	"[ \"\$(get gkept x)\" = $moved_x ] && [ \"\$(get gkept y)\" = $moved_y ]"
stop gkept

x11 gplain rehide=1 rehide_ms=2000
mapped gplain
settle
drag $(($(get gplain x) + 100)) $(($(get gplain y) + 12)) 500 300
check "it can be moved" "[ \"\$(right gplain)\" != 1280 ]"
wait_for "gplain was hidden and shown" grep -q "reshown 1" "$SMOKE_DIR/gplain.events"
check "shown again, one without returns to its entry's place" \
	"[ \"\$(right gplain)\" = 1280 ] && [ \"\$(bottom gplain)\" = 720 ]"
stop gplain

do_input move 400 300
settle
wayland gpointer
mapped gpointer
cx=$(global cursor x) cy=$(global cursor y)
check "pointer puts the middle of the title bar under the pointer" \
	"[ \$cx -ge 399 ] && [ \$cx -le 401 ] && \
	 [ \$((\$(get gpointer x) + \$(outer_w gpointer) / 2 - cx)) -ge -\$(get gpointer th) ] && \
	 [ \$((\$(get gpointer x) + \$(outer_w gpointer) / 2 - cx)) -le \$(get gpointer th) ] && \
	 [ \$((\$(get gpointer y) + \$(get gpointer bw) + \$(get gpointer th) / 2)) = \$cy ]"
stop gpointer

do_input move 1270 710
settle
x11 gpedge pos=100,100
mapped gpedge
check "at the bottom right it stays on the screen, before the X11 program's place" \
	"[ \"\$(right gpedge)\" = 1280 ] && [ \"\$(bottom gpedge)\" = 720 ]"
stop gpedge

# Growing by itself, a window keeps the edges its entry counts from.
wayland gwgrow grow=400x300 states=1
mapped gwgrow
wait_for "gwgrow has grown" grep -q grown "$SMOKE_DIR/gwgrow.events"
check "a Wayland window placed at -0-0 keeps the corner when it grows" \
	"[ \"\$(outer_w gwgrow)\" -gt 400 ] && [ \"\$(right gwgrow)\" = 1280 ] && [ \"\$(bottom gwgrow)\" = 720 ]"
grown_w=$(outer_w gwgrow)
wayland gfocus pos=0,0
mapped gfocus
check "another window takes the focus" "[ \"\$(get gwgrow active)\" = 0 ]"
stop gfocus
check "gwgrow has the focus again" "[ \"\$(get gwgrow active)\" = 1 ]"
settle
check "and keeps the size it chose through the configures that come with that" \
	"[ \"\$(outer_w gwgrow)\" = $grown_w ] && [ \"\$(right gwgrow)\" = 1280 ]"
do_input key alt+F10
check "gwgrow is maximized" "[ \"\$(get gwgrow max)\" = 1 ]"
do_input key alt+F10
check "restored, mwc's own size change moves nothing: still in the corner" \
	"[ \"\$(get gwgrow max)\" = 0 ] && [ \"\$(right gwgrow)\" = 1280 ] && [ \"\$(bottom gwgrow)\" = 720 ]"
stop gwgrow

x11 gxgrow grow=400x300
mapped gxgrow
wait_for "gxgrow has grown" grep -q grown "$SMOKE_DIR/gxgrow.events"
check "an X11 window as well, whatever its own gravity" \
	"[ \"\$(outer_w gxgrow)\" -gt 400 ] && [ \"\$(right gxgrow)\" = 1280 ] && [ \"\$(bottom gxgrow)\" = 720 ]"
stop gxgrow

# Once mwc has asked for a size, its later configures carry one: the size it chose
# itself must be what they say, or an obedient client shrinks back.
wayland gwsized grow=400x300 grow_ms=3000 states=1
mapped gwsized
do_input key alt+F10
check "gwsized is maximized" "[ \"\$(get gwsized max)\" = 1 ]"
do_input key alt+F10
check "and restored" "[ \"\$(get gwsized max)\" = 0 ]"
wait_for "gwsized has grown" grep -q grown "$SMOKE_DIR/gwsized.events"
settle
check "it keeps the size it chose" "[ \"\$(outer_w gwsized)\" -gt 400 ] && [ \"\$(right gwsized)\" = 1280 ]"
grown_w=$(outer_w gwsized)
wayland gfocus
mapped gfocus
stop gfocus
check "gwsized has the focus again" "[ \"\$(get gwsized active)\" = 1 ]"
settle
check "it keeps the size it chose after one mwc asked for" \
	"[ \"\$(outer_w gwsized)\" = $grown_w ] && [ \"\$(right gwsized)\" = 1280 ]"
stop gwsized

wayland gwleft grow=400x300
mapped gwleft
wait_for "gwleft has grown" grep -q grown "$SMOKE_DIR/gwleft.events"
check "one placed at +20+30 keeps its top-left corner" \
	"[ \"\$(outer_w gwleft)\" -gt 400 ] && [ \"\$(get gwleft x)\" = 20 ] && [ \"\$(get gwleft y)\" = 30 ]"
stop gwleft

# A frame asked for only once the window is shown, as GTK3 does.
wayland gwframe late_frame=kde
mapped gwframe
check "it appears without one" "[ \"\$(title gwframe)\" = 0 ]"
check "gwframe has its frame" "[ \"\$(title gwframe)\" -gt 0 ]"
settle
check "a frame that comes late keeps a window placed at -0-0 in the corner" \
	"[ \"\$(right gwframe)\" = 1280 ] && [ \"\$(bottom gwframe)\" = 720 ]"
stop gwframe

wayland gwfleft late_frame=kde
mapped gwfleft
check "gwfleft has its frame" "[ \"\$(title gwfleft)\" -gt 0 ]"
settle
check "and one at +20+30 at its top-left corner" \
	"[ \"\$(get gwfleft x)\" = 20 ] && [ \"\$(get gwfleft y)\" = 30 ]"
stop gwfleft

# A reload with a larger font: the title bar grows, the corner stays.
wayland gwfont
x11 gxfont
mapped gwfont
mapped gxfont
check "both are in the corner" \
	"[ \"\$(right gwfont)\" = 1280 ] && [ \"\$(bottom gwfont)\" = 720 ] && \
	 [ \"\$(right gxfont)\" = 1280 ] && [ \"\$(bottom gxfont)\" = 720 ]"
old_title=$(title gwfont)
echo 'Mwm*fontList:  Sans Bold 30' >>"$RC"
kill -HUP "$pid"
check "the title bar has grown" "[ \"\$(title gwfont)\" -gt $old_title ]"
check "a reload with a taller title bar keeps a Wayland window in its corner" \
	"[ \"\$(right gwfont)\" = 1280 ] && [ \"\$(bottom gwfont)\" = 720 ]"
check "and an X11 window" \
	"[ \"\$(right gxfont)\" = 1280 ] && [ \"\$(bottom gxfont)\" = 720 ]"
sed -i '/^Mwm\*fontList:/d' "$RC"
kill -HUP "$pid"
check "the font is back" "[ \"\$(title gwfont)\" = $old_title ]"
check "and both are still in the corner" \
	"[ \"\$(bottom gwfont)\" = 720 ] && [ \"\$(bottom gxfont)\" = 720 ]"
stop gwfont
stop gxfont

wayland gwmoved grow=400x300 grow_ms=3000
mapped gwmoved
drag $(($(get gwmoved x) + 100)) $(($(get gwmoved y) + 12)) 500 300
check "it can be moved away" "[ \"\$(right gwmoved)\" != 1280 ]"
settle
moved_x=$(get gwmoved x)
moved_y=$(get gwmoved y)
wait_for "gwmoved has grown" grep -q grown "$SMOKE_DIR/gwmoved.events"
check "moved by hand, it grows from where it is now" \
	"[ \"\$(outer_w gwmoved)\" -gt 400 ] && [ \"\$(get gwmoved x)\" = $moved_x ] && [ \"\$(get gwmoved y)\" = $moved_y ]"
stop gwmoved

wayland gmain dialog=gdialog
mapped gmain
mapped gdialog
check "a dialog goes with its window, whatever its entry says" \
	"[ \"\$(get gdialog x)\" != 0 ] && [ \"\$(get gdialog y)\" != 0 ]"
stop gmain

x11 gxmain dialog=gxdialog
mapped gxmain
mapped gxdialog
check "an X11 dialog as well" \
	"[ \"\$(get gxdialog x)\" != 0 ] && [ \"\$(get gxdialog y)\" != 0 ]"
stop gxmain

x11 gutil type=utility
mapped gutil
check "and a utility window" "[ \"\$(get gutil x)\" != 0 ] && [ \"\$(get gutil y)\" != 0 ]"
stop gutil

wayland gcorner
mapped gcorner
do_input key super+t
check "tiling takes it over" "[ \"\$(get gcorner tiled)\" = 1 ]"
do_input key super+t
check "and gives it back to its place" \
	"[ \"\$(get gcorner tiled)\" = 0 ] && [ \"\$(right gcorner)\" = 1280 ] && [ \"\$(bottom gcorner)\" = 720 ]"
stop gcorner

wayland gother
mapped gother
wayland gtab
mapped gtab
do_input key super+g
check "gtab joins a group" "[ \"\$(get gtab tab)\" != -1 ]"
do_input key super+shift+g
check "taken out again" "[ \"\$(get gtab tab)\" = -1 ] && [ \"\$(get gtab tab_hidden)\" = 0 ]"
check "it cascades, the entry is for appearing" "[ \"\$(right gtab)\" != 1280 ]"
stop gtab
stop gother

wayland gmaxed pre=max
mapped gmaxed
check "gmaxed starts maximized" "[ \"\$(get gmaxed max)\" = 1 ]"
settle
do_input key alt+F10
check "and goes back to the top left, as documented" \
	"[ \"\$(get gmaxed max)\" = 0 ] && [ \"\$(get gmaxed x)\" = 32 ] && [ \"\$(get gmaxed y)\" = 32 ]"
stop gmaxed

wayland glate late_app=gcorner
check "its app_id arrives" "[ -n \"\$(get gcorner x)\" ]"
check "too late for the entry, as documented" "[ \"\$(right gcorner)\" != 1280 ]"
kill "$PID_glate"
wait "$PID_glate" 2>/dev/null
check "glate is gone" "[ -z \"\$(get gcorner x)\" ]"

x11 gpanel type=dock pos=0,500 size=1280x40 strut=0,0,0,40
mapped gpanel
check "a panel stays where it wants to be" "[ \"\$(get gpanel y)\" = 500 ]"
wayland gcorner
mapped gcorner
check "the corner is the usable area's" \
	"[ \"\$(right gcorner)\" = 1280 ] && [ \"\$(bottom gcorner)\" = 680 ]"
stop gcorner
stop gpanel

x11 gsized
wayland ghalf
mapped gsized
mapped ghalf
# Loaded four times by now (twice for the font), and the window asking adds nothing.
check "a size in the value is reported once per load" \
	"[ \"\$(grep -c 'mwmrc:7: expected a position' \"$LOG\")\" = 4 ]"
check "half a position too" "grep -q 'mwmrc:8: expected a position' \"$LOG\""
check "and such a window cascades as usual" "[ \"\$(right gsized)\" != 1280 ]"
stop gsized
stop ghalf
rm "$RC"

# A dialog that is being moved goes away with its window: the move ends with it.
x11 dlgmain size=400x300 dialog=dlgkid iconify_ms=2500
check "a window with a dialog" "[ -n \"\$(get dlgkid x)\" ] && [ -n \"\$(get dlgmain x)\" ]"
kx=$(($(get dlgkid x) + $(get dlgkid bw) + 40)) ky=$(($(get dlgkid y) + $(get dlgkid bw) + $(get dlgkid th) + 30))
do_input hold alt
do_input move $kx $ky
do_input down left
do_input move $((kx + 60)) $((ky + 40))
check "the dialog is being moved" "[ \"\$(global cursor mode)\" = 1 ]"
wait_for "the window minimizes itself" grep -q '^iconified' "$SMOKE_DIR/dlgmain.events"
check "and the dialog goes with it" "[ \"\$(get dlgmain min)\" = 1 ] && [ \"\$(get dlgkid hidden)\" = 1 ]"
check "the move ends" "[ \"\$(global cursor mode)\" = 0 ] && [ \"\$(global outline shown)\" = 0 ]"
do_input move $((kx + 200)) $((ky + 100))
do_input up left
do_input release alt
kx2=$(get dlgkid x) ky2=$(get dlgkid y)
do_input move 900 600
sleep 0.2
refresh
[ "$(get dlgkid x)" = "$kx2" ] && [ "$(get dlgkid y)" = "$ky2" ] ||
	fail "the hidden dialog still follows the pointer"
echo "ok: the hidden dialog stays where it is"
stop dlgmain

# The corner grooves go on from the lines where the buttons meet, as in mwm: dark on
# B + T - 1, light on B + T, along the top and down the sides.
wayland grooves size=300x200
check "it has the focus" "[ \"\$(get grooves active)\" = 1 ]"
sleep 0.5
refresh
GX=$(get grooves x) GY=$(get grooves y) GB=$(get grooves bw) GT=$(get grooves th)
GW=$(($(get grooves w) + 2 * GB)) GH=$(($(get grooves h) + 2 * GB + GT))
K=$((GB + GT))
WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/grooves.png" || fail "a screenshot"
px() { build/pngtool pixel "$SMOKE_DIR/grooves.png" "$1" "$2"; }
BORDER_Y=$((GY + GB / 2)) TITLE_Y=$((GY + GB + GT / 2))
line_at() { # description x: a groove in the border and a button edge in the title there
	local x=$2
	[ "$(px $((x - 1)) $BORDER_Y)" != "$(px $x $BORDER_Y)" ] ||
		fail "$1: no groove in the border at $x"
	[ "$(px $((x - 1)) $BORDER_Y)" = "$(px $((x - 1)) $TITLE_Y)" ] &&
		[ "$(px $x $BORDER_Y)" = "$(px $x $TITLE_Y)" ] ||
		fail "$1: the groove at $x isn't on the button edge: border $(px $((x - 1)) $BORDER_Y) $(px $x $BORDER_Y), title $(px $((x - 1)) $TITLE_Y) $(px $x $TITLE_Y)"
	echo "ok: $1"
}
line_at "top left: the groove goes on from the menu button's edge" $((GX + K))
line_at "top right: and from the maximize button's" $((GX + GW - K))
SIDE_X=$((GX + GW - GB / 2)) CLIENT_X=$((GX + GW - GB - 10))
[ "$(px $SIDE_X $((GY + K - 1)))" = "$(px $CLIENT_X $((GY + K - 1)))" ] &&
	[ "$(px $SIDE_X $((GY + K - 1)))" != "$(px $SIDE_X $((GY + K)))" ] ||
	fail "the side groove isn't on the title bar's lower edge"
echo "ok: down the side it goes on from the title bar's lower edge"
stop grooves

# A narrow window: the corners take at most a third of the frame, as in mwm, so a
# piece of the side is left between them.
wayland narrow size=40x150
check "narrow is mapped" "[ -n \"\$(get narrow x)\" ]"
sleep 0.5
refresh
NX=$(get narrow x) NY=$(get narrow y) NB=$(get narrow bw)
NW=$(($(get narrow w) + 2 * NB))
NK=$((NW / 3))
WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/narrow.png" || fail "a screenshot"
npx() { build/pngtool pixel "$SMOKE_DIR/narrow.png" "$1" "$2"; }
NY0=$((NY + NB / 2))
[ "$(npx $((NX + NK - 1)) $NY0)" != "$(npx $((NX + NK)) $NY0)" ] &&
	[ "$(npx $((NX + NW - NK - 1)) $NY0)" != "$(npx $((NX + NW - NK)) $NY0)" ] ||
	fail "the corners of a frame $NW wide don't end at a third, $NK"
[ "$(npx $((NX + NW / 2)) $NY0)" = "$(npx $((NX + NW / 2 + 1)) $NY0)" ] ||
	fail "no side left between the corners"
echo "ok: a narrow window's corners take a third each, with side between them"
stop narrow

# Behind the client lies the frame's colour, as the frame window under an X11 client:
# it shows through what the client leaves transparent and takes the clicks outside its
# input region.
wayland clear size=300x200 clear=1
check "clear has the focus" "[ \"\$(get clear active)\" = 1 ]"
wayland other size=100x80
check "other takes it" "[ \"\$(get other active)\" = 1 ] && [ \"\$(get clear active)\" = 0 ]"
drag $(($(get other x) + 50)) $(($(get other y) + $(get other bw) + 4)) 1000 500
sleep 0.5
refresh
QX=$(get clear x) QY=$(get clear y) QB=$(get clear bw) QT=$(get clear th)
MX=$((QX + QB + $(get clear w) / 2)) MY=$((QY + QB + QT + $(get clear h) / 2))
WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/clear.png" || fail "a screenshot"
qpx() { build/pngtool pixel "$SMOKE_DIR/clear.png" "$1" "$2"; }
[ "$(qpx $MX $MY)" = "$(qpx $((QX + QB / 2)) $MY)" ] ||
	fail "a transparent client shows $(qpx $MX $MY), not its frame's $(qpx $((QX + QB / 2)) $MY)"
echo "ok: a transparent client shows its frame's colour"
do_input move $MX $MY
do_input click left
check "a click where it takes none brings it to the front" \
	"[ \"\$(get clear active)\" = 1 ] && [ \"\$(global menu open)\" = 0 ]"
stop other
stop clear

# ---- X11 requests for the focus ---------------------------------------------------------

saw() { wait_for "$1" grep -q "$3" "$SMOKE_DIR/$2.events"; }
# A program asks for its window to be activated (_NET_ACTIVE_WINDOW): not from a
# Wayland window that has the focus.
x11 asker size=300x200 activate_after="$SMOKE_DIR/ask1"
mapped asker
wayland wfront size=300x200
mapped wfront
check "the Wayland window has the focus" "[ \"\$(get wfront kbfocus)\" = 1 ]"
touch "$SMOKE_DIR/ask1"
# Retitled after the request: by then mwc has handled it (xclient's usage).
check "mwc has had the request" "[ \"\$(get asker tlen)\" = 12 ]"
check "the Wayland window keeps it" \
	"[ \"\$(get wfront kbfocus)\" = 1 ] && [ \"\$(get asker active)\" = 0 ]"
stop asker
# From another X11 window it may: that is how X taskbars and emacsclient work.
x11 asker2 size=300x200 activate_after="$SMOKE_DIR/ask2"
mapped asker2
x11 xfront size=300x200
mapped xfront
check "an X11 window has the focus" "[ \"\$(get xfront active)\" = 1 ]"
touch "$SMOKE_DIR/ask2"
check "it gets the focus" "[ \"\$(get asker2 active)\" = 1 ]"
stop asker2
stop xfront
# A menu-like window that wants the keyboard doesn't take it from a Wayland window.
x11 orwin pos=900,400 size=300x200 popup=600,300 popup_type=none popup_ms=1500 popup_for=1500
mapped orwin
refresh
do_input move $(($(get wfront x) + 100)) $(($(get wfront y) + 100))
do_input click left
check "the Wayland window has the focus again" "[ \"\$(get wfront kbfocus)\" = 1 ]"
saw "the X11 popup opens" orwin '^popup shown'
check "the keyboard stays with the Wayland window" "[ \"\$(get wfront kbfocus)\" = 1 ]"
stop orwin
stop wfront
# Back from an icon: X taskbars ask with _NET_ACTIVE_WINDOW, and wlroots also takes
# WM_CHANGE_STATE NormalState, which ICCCM leaves to mapping. Either one from another X11
# window, as for activation, not from a Wayland one. After each request the window is
# retitled (xclient's usage): once mwc has the title, it has had the request.
had() { check "mwc has had the request of $1" "[ \"\$(get $1 tlen)\" = 12 ]"; }
for how in normalize activate; do
	x11 xhas size=300x200
	mapped xhas
	x11 xicon size=300x200 iconic=1 ${how}_after="$SMOKE_DIR/$how.1"
	check "an X11 window starts as an icon" "[ \"\$(get xicon min)\" = 1 ]"
	check "the other X11 window has the focus" "[ \"\$(get xhas active)\" = 1 ]"
	touch "$SMOKE_DIR/$how.1"
	check "$how: it comes back, with the focus" \
		"[ \"\$(get xicon min)\" = 0 ] && [ \"\$(get xicon active)\" = 1 ]"
	stop xicon
	stop xhas
	wayland wfront2 size=300x200
	mapped wfront2
	x11 xicon2 size=300x200 iconic=1 ${how}_after="$SMOKE_DIR/$how.2"
	check "another X11 window starts as an icon" "[ \"\$(get xicon2 min)\" = 1 ]"
	check "a Wayland window has the focus" "[ \"\$(get wfront2 kbfocus)\" = 1 ]"
	touch "$SMOKE_DIR/$how.2"
	had xicon2
	check "$how: it stays an icon, the Wayland window keeps the focus" \
		"[ \"\$(get xicon2 min)\" = 1 ] && [ \"\$(get wfront2 kbfocus)\" = 1 ]"
	stop xicon2
	stop wfront2
done
# Nor while a panel or launcher has the keyboard, with an X11 window focused below: not
# by activation, a restore from an icon, or the program moving the X focus between its
# own windows (XSetInputFocus, which wlroots allows within one program).
x11 xbelow pos=100,100 size=300x200 dialog=xbelowdlg focus_main_after="$SMOKE_DIR/fm"
mapped xbelow
check "its dialog is mapped" "[ -n \"\$(get xbelowdlg x)\" ]"
x11 xasks pos=800,100 size=300x200 activate_after="$SMOKE_DIR/act"
mapped xasks
x11 xicon3 pos=800,400 size=300x200 iconic=1 normalize_after="$SMOKE_DIR/norm3"
check "an X11 window starts as an icon" "[ \"\$(get xicon3 min)\" = 1 ]"
refresh
do_input move $(($(get xbelowdlg x) + 40)) $(($(get xbelowdlg y) + 40))
do_input click left
check "the X11 dialog below has the focus" "[ \"\$(get xbelowdlg active)\" = 1 ]"
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top size=200x100 keyboard=on-demand \
	color=00ff00 >"$SMOKE_DIR/launcher.events" 2>&1 &
PID_launcher=$!
wait_for "a launcher has the keyboard" grep -q keyboard-enter "$SMOKE_DIR/launcher.events"
touch "$SMOKE_DIR/act" "$SMOKE_DIR/norm3" "$SMOKE_DIR/fm"
had xasks
had xicon3
had xbelow
check "none of them gets the keyboard" \
	"[ \"\$(get xasks active)\" = 0 ] && [ \"\$(get xicon3 min)\" = 1 ] && \
	 [ \"\$(get xbelow kbfocus)\" = 0 ] && [ \"\$(get xbelowdlg kbfocus)\" = 0 ]"
grep -q keyboard-leave "$SMOKE_DIR/launcher.events" &&
	fail "the launcher lost the keyboard to an X11 program"
echo "ok: the launcher keeps the keyboard"
# The X focus the program took goes back to its dialog, which has it when the launcher goes.
wait_for "the X focus is back on the dialog" \
	eval "grep '^focus-in' \"\$SMOKE_DIR/xbelow.events\" | tail -1 | grep -qx 'focus-in dialog'"
check "mwc still has the dialog as the window with the focus" \
	"[ \"\$(get xbelowdlg active)\" = 1 ] && [ \"\$(get xbelow active)\" = 0 ]"
kill "$PID_launcher"
wait "$PID_launcher" 2>/dev/null
check "the launcher gone, the dialog has the keyboard" "[ \"\$(get xbelowdlg kbfocus)\" = 1 ]"
stop xicon3
stop xasks
stop xbelow

# ---- fullscreen that doesn't cover the output ---------------------------------------------

# A program keeping its own size, as a game with a fixed resolution: in the middle of
# the output, with black around it, not the desktop (xdg-shell).
wayland fixedres size=400x300 own_size=1 pre=full
mapped fixedres
check "it is fullscreen" "[ \"\$(get fixedres full)\" = 1 ] && [ \"\$(get fixedres w)\" = 400 ]"
pixel_is "black around it" 5 5 000000
pixel_is "black beside it" 300 360 000000
pixel_is "the window itself in the middle" 640 360 808080
stop fixedres
# Its popups are placed by where it is drawn: one at its right edge slides back onto the
# output. 400 wide in the middle of 1280 is drawn from 440; a popup 600 wide at 350
# would end at 1390, so it slides 110 to the left (Review 2026-11-18 #23).
wayland fixedpop size=400x300 own_size=1 pre=full popup=350,100,600,100 \
	popup_after="$SMOKE_DIR/popup"
mapped fixedpop
check "it is fullscreen" "[ \"\$(get fixedpop full)\" = 1 ] && [ \"\$(get fixedpop w)\" = 400 ]"
touch "$SMOKE_DIR/popup"
saw "its popup slides back onto the output" fixedpop '^popup at 240 100$'
stop fixedpop

# ---- what mwc refuses is written back -----------------------------------------------------

# wlroots writes _NET_WM_STATE_HIDDEN as asked before mwc decides: when mwc refuses,
# the property must say what is (Review 2026-11-18 #39). An icon asking to be shown
# while a Wayland window has the focus stays an icon, and says so.
wayland wfront3 size=300x200
mapped wfront3
x11 xhidden size=300x200 iconic=1 netstate=1 script=-i
check "an X11 window starts as an icon" "[ \"\$(get xhidden min)\" = 1 ]"
check "a Wayland window has the focus" "[ \"\$(get wfront3 kbfocus)\" = 1 ]"
saw "the icon asks to be shown" xhidden '^sent 1'
check "it stays an icon" "[ \"\$(get xhidden min)\" = 1 ]"
wait_for "and _NET_WM_STATE says hidden again" \
	eval "grep '^net-more' \"\$SMOKE_DIR/xhidden.events\" | tail -1 | grep -q 'hidden=1'"
stop xhidden
stop wfront3

# ---- a burst of titles ---------------------------------------------------------------------

# A program that sets its title as fast as it can (a progress counter, a terminal
# forwarding title escapes): the title bar is drawn at most once a refresh, with the last
# one, not once per title (Review 2026-11-18 #18).
wayland titled size=600x200 titles=2000
mapped titled
saw "it sends 2000 titles" titled '^titles sent'
check "the last one counts" "[ \"\$(get titled tlen)\" = 4 ]"
check "and is drawn" "[ \"\$(get titled tdrawn)\" = 4 ]"
n=$(get titled tdraws)
[ "$n" -le 60 ] || fail "2000 titles drew the title bar $n times"
echo "ok: 2000 titles drew the title bar $n times"
stop titled

# ---- the window menu a client asks for -------------------------------------------------

# show_window_menu says where in surface coordinates, which include a shadow drawn
# around the window geometry (GTK): the menu opens there, not 20 px off.
wayland shadowed size=300x200 shadow=20 menu_at=50,40
mapped shadowed
refresh
SX=$(get shadowed x) SY=$(get shadowed y)
do_input move $((SX + 150)) $((SY + $(border shadowed) + $(title shadowed) + 100))
do_input click left
saw "it asks for its window menu" shadowed '^menu asked'
check "the menu opens at that point of its surface" \
	"[ \"\$(global menu open)\" = 1 ] && \
	 [ \"\$(global menu x)\" = \$((SX + \$(border shadowed) + 30)) ] && \
	 [ \"\$(global menu y)\" = \$((SY + \$(border shadowed) + \$(title shadowed) + 20)) ]"
do_input key Escape
stop shadowed

# ---- what a window is offered ----------------------------------------------------------------

# wm_capabilities as mwc means them: a dialog gets no maximize or minimize to show.
wayland capable size=300x200 caps=1 dialog=capdlg
mapped capable
wait_for "the main window is offered both" grep -q 'caps capable max=1 min=1' "$SMOKE_DIR/capable.events"
wait_for "its dialog neither" grep -q 'caps capdlg max=0 min=0' "$SMOKE_DIR/capable.events"
stop capable

# ---- X11 states that wlroots offers ----------------------------------------------------------

more_is() { [ "$(grep '^net-more' "$SMOKE_DIR/$1.events" | tail -1)" = "net-more $2" ]; }
x11 stick size=300x200 netstate=1 script=+s
mapped stick
saw "it asks to be sticky" stick '^sent 1'
check "it is on all desktops" "[ \"\$(get stick everywhere)\" = 1 ] && more_is stick 'sticky=1 shaded=0 hidden=0'"
stop stick
# Rolled up is the tiling layout's to decide: the request is written back at once, so
# the client sees no change. Sticky afterwards shows what the state says by then.
x11 roll size=300x200 netstate=1 script=+r,+s
mapped roll
saw "it asks to be shaded, then sticky" roll '^sent 2'
wait_for "shaded is taken back, sticky kept" more_is roll 'sticky=1 shaded=0 hidden=0'
check "and it isn't rolled up" "[ \"\$(get roll shaded)\" = 0 ]"
stop roll

stop_vinput
stop_mwc
echo "PASS: window"
