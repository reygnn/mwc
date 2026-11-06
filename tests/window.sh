#!/bin/bash
# Headless test of window behaviour: size hints, Motif hints, dialogs, window
# types, X11 panels, gravity and focus rules, with clients that can ask for
# each of them (tests/xclient.c, tests/xdgc.c).
# Output goes to build/window.log. Exits non-zero on failure.
LOG=build/window.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland grim
for tool in build/vinput build/pngtool build/xclient build/xdgc; do
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

sensitive_is() { # description window expected
	do_input key alt+space
	check "$1" "[ \"\$(global menu sensitive | tr -d TePD)\" = $3 ]"
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
EOF
kill -HUP "$pid"
settle

right() { echo $(($(get "$1" x) + $(outer_w "$1"))); }
bottom() { echo $(($(get "$1" y) + $(outer_h "$1"))); }

wayland gcorner
mapped gcorner
check "-0-0 puts a Wayland window into the corner" \
	"[ \"\$(right gcorner)\" = 1280 ] && [ \"\$(bottom gcorner)\" = 720 ]"
drag $(($(get gcorner x) + 100)) $(($(get gcorner y) + 12)) 500 300
check "it can be moved away" "[ \"\$(right gcorner)\" != 1280 ]"
moved_x=$(get gcorner x)
kill -HUP "$pid"
settle
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
# Loaded twice by now, and the window asking adds nothing.
check "a size in the value is reported once per load" \
	"[ \"\$(grep -c 'mwmrc:7: expected a position' \"$LOG\")\" = 2 ]"
check "half a position too" "grep -q 'mwmrc:8: expected a position' \"$LOG\""
check "and such a window cascades as usual" "[ \"\$(right gsized)\" != 1280 ]"
stop gsized
stop ghalf
rm "$RC"

stop_vinput
stop_mwc
echo "PASS: window"
