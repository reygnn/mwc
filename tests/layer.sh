#!/bin/bash
# Headless layer-shell test: panels reserving space, stacking against windows,
# keyboard rules and clicks, using tests/layerc.c as the client.
# Output goes to build/layer.log. Exits non-zero on failure.
LOG=build/layer.log
. "$(dirname "$0")/lib.sh"

require_tools foot xterm Xwayland grim
for tool in build/vinput build/pngtool build/layerc build/xdgc build/xclient; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
ROOT=5b7792
PANEL=ff00ff

# layerc_start <name> key=value...: remembers the pid in LAYERC_<name>, logs to
# $SMOKE_DIR/<name>.events.
layerc_start() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/layerc "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "LAYERC_$name=$!"
	wait_for "$name is configured" grep -q configured "$SMOKE_DIR/$name.events"
}

layerc_stop() {
	local var="LAYERC_$1"
	kill "${!var}" 2>/dev/null
	wait "${!var}" 2>/dev/null
}

has_event() { grep -q "$2" "$SMOKE_DIR/$1.events"; }

output_is() { # <description> <x> <y> <w> <h>: the usable area
	check "$1" "[ \"\$(global output ux)\" = $2 ] && [ \"\$(global output uy)\" = $3 ] && \
		[ \"\$(global output uw)\" = $4 ] && [ \"\$(global output uh)\" = $5 ]"
}

marker='touch "$SMOKE_DIR/$0"; exec sleep 120'
start_mwc "echo \"\$DISPLAY\" > \"\$SMOKE_DIR/display\"; foot sh -c '$marker' foot-up & \
	while [ ! -e \"\$SMOKE_DIR/foot-up\" ]; do sleep 0.1; done; \
	exec xterm -e sh -c '$marker' xterm-up"
wait_for "foot connected (Wayland)" test -e "$SMOKE_DIR/foot-up"
wait_for "xterm connected (XWayland)" test -e "$SMOKE_DIR/xterm-up"
X_DISPLAY=$(cat "$SMOKE_DIR/display")
start_vinput
check "both windows are mapped" "[ -n \"\$(get foot x)\" ] && [ -n \"\$(get XTerm x)\" ]"
B=$(global theme border)
T=$(global theme title)

# ---- reserving space ---------------------------------------------------------

output_is "without panels, all of the output is usable" 0 0 1280 720

do_input key alt+F10
check "xterm is maximized over everything" \
	"[ \"\$(get XTerm max)\" = 1 ] && [ \"\$(get XTerm y)\" = 0 ] && \
	[ \$((\$(get XTerm h) + 2 * B + T)) = 720 ]"

layerc_start top layer=top anchor=top,left,right size=0x30 exclusive=30 color=$PANEL
output_is "a top panel reserves its height" 0 30 1280 690
check "a maximized X11 window moves out of its way" \
	"[ \"\$(get XTerm y)\" = 30 ] && [ \$((\$(get XTerm h) + 2 * B + T)) = 690 ]"
pixel_is "the panel is drawn" 640 10 "$PANEL"

layerc_start left layer=top anchor=left,top,bottom size=50x0 exclusive=50 color=00ffff
output_is "a left panel reserves its width as well" 50 30 1230 690
check "the window moves again" \
	"[ \"\$(get XTerm x)\" = 50 ] && [ \$((\$(get XTerm w) + 2 * B)) = 1230 ]"

layerc_stop left
output_is "the left panel is gone" 0 30 1280 690
check "the window takes the room back" "[ \"\$(get XTerm x)\" = 0 ] && [ \$((\$(get XTerm w) + 2 * B)) = 1280 ]"

do_input key alt+F10
check "xterm is back to normal" "[ \"\$(get XTerm max)\" = 0 ]"

# Wayland client on the same path.
do_input key alt+Tab
check "foot has the focus" "[ \"\$(get foot active)\" = 1 ]"
do_input key alt+F10
check "a maximized Wayland window stays below the panel" \
	"[ \"\$(get foot max)\" = 1 ] && [ \"\$(get foot y)\" = 30 ] && \
	[ \$((\$(get foot h) + 2 * B + T)) = 690 ]"
do_input key alt+F10
check "foot is back to normal" "[ \"\$(get foot max)\" = 0 ]"

# ---- a bottom panel and the icons ---------------------------------------------

layerc_start bottom layer=top anchor=bottom,left,right size=0x40 exclusive=40 color=ffff00
output_is "top and bottom panel" 0 30 1280 650
do_input key alt+F9
check "foot is an icon above the bottom panel" \
	"[ \"\$(get foot min)\" = 1 ] && [ \"\$(get foot icon_y)\" -gt 0 ] && \
	[ \$((\$(get foot icon_y) + 72)) -le 680 ]"
icon_y=$(get foot icon_y)
layerc_stop bottom
output_is "the bottom panel is gone" 0 30 1280 690
check "the icon moves down with the free space" "[ \"\$(get foot icon_y)\" -gt $icon_y ]"
do_input move $(($(get foot icon_x) + 30)) $(($(get foot icon_y) + 30))
do_input click left
do_input click left
check "foot is back" "[ \"\$(get foot min)\" = 0 ]"

# ---- stacking -----------------------------------------------------------------

# Drag foot so its title bar lies under the top panel, and see the panel win.
sleep 0.45
refresh
tx=$(($(get foot x) + 100)) ty=$(($(get foot y) + B + T / 2))
do_input move "$tx" "$ty"
do_input down left
do_input move "$tx" 5
do_input up left
check "foot is partly under the panel" "[ \"\$(get foot y)\" -lt 20 ]"
pixel_is "the top layer covers windows" "$tx" 10 "$PANEL"

layerc_start bgtool layer=bottom anchor=top,bottom,left,right size=0x0 color=112233
pixel_is "the bottom layer shows on the desktop" 1270 700 112233
refresh
pixel_is "windows cover the bottom layer" $(($(get foot x) + 3)) $(($(get foot y) + 100)) 7387ad
layerc_stop bgtool

# Fullscreen: only while it has the focus does it cover a top panel.
do_input key alt+F11
check "foot is fullscreen, over the whole output" \
	"[ \"\$(get foot full)\" = 1 ] && [ \"\$(get foot y)\" = 0 ] && [ \"\$(get foot h)\" = 720 ]"
pixel_isnt "a focused fullscreen window covers the panel" 640 10 "$PANEL"
do_input key alt+Tab
check "xterm has the focus" "[ \"\$(get XTerm active)\" = 1 ] && [ \"\$(get foot full)\" = 1 ]"
pixel_is "without the focus, the panel is visible again" 640 10 "$PANEL"
do_input key alt+Tab
check "foot has the focus again" "[ \"\$(get foot active)\" = 1 ]"
# A launcher or a prompt in the top layer takes the keys: it must be seen, or every key
# goes into a surface under the fullscreen window and Alt+Tab can't get past it.
layerc_start prompt layer=top size=200x100 keyboard=exclusive color=00ff00
wait_for "the prompt has the keyboard" has_event prompt keyboard-enter
pixel_is "a top-layer surface with the keyboard is above the fullscreen window" \
	640 360 00ff00
do_input key a
wait_for "the keys go to the prompt one can see" has_event prompt "key 30"
layerc_stop prompt
check "foot has the keyboard back, still fullscreen" \
	"[ \"\$(get foot kbfocus)\" = 1 ] && [ \"\$(get foot full)\" = 1 ]"
pixel_isnt "with the prompt gone, foot covers the panel again" 640 10 "$PANEL"
# On demand, as it appears: the same, and a click on foot takes the keys back.
layerc_stop top
layerc_start top layer=top anchor=top,left,right size=0x30 exclusive=30 \
	keyboard=on-demand color=$PANEL
wait_for "an on-demand panel appearing has the keyboard" has_event top keyboard-enter
pixel_is "and is above the fullscreen window" 640 10 "$PANEL"
do_input move 640 360
do_input click left
check "a click on foot takes the keyboard back" "[ \"\$(get foot kbfocus)\" = 1 ]"
pixel_isnt "a click on foot puts it back over the panel" 640 10 "$PANEL"
layerc_stop top
layerc_start top layer=top anchor=top,left,right size=0x30 exclusive=30 color=$PANEL
do_input key alt+F11
check "foot leaves fullscreen" "[ \"\$(get foot full)\" = 0 ]"
pixel_is "the panel is back on top" 640 10 "$PANEL"
# An X11 window both fullscreen and above the others (mpv --ontop --fs) leaves the
# fullscreen layer for the one it shares with top-layer surfaces: there too the
# prompt must stay above it.
DISPLAY=$X_DISPLAY build/xclient class=XOnTop state=above,fullscreen color=0000ff \
	>"$SMOKE_DIR/xontop.log" 2>&1 &
XONTOP=$!
check "the X11 window is fullscreen" "[ \"\$(get XOnTop full)\" = 1 ]"
# Drawn at its new size before the click, or the click lands on what was there.
pixel_is "it is drawn over the output" 640 360 0000ff
do_input move 640 360
do_input click left
check "and has the focus" "[ \"\$(get XOnTop active)\" = 1 ]"
pixel_isnt "it covers the panel" 640 10 "$PANEL"
layerc_start prompt layer=top size=200x100 keyboard=exclusive color=00ff00
wait_for "the prompt has the keyboard" has_event prompt keyboard-enter
pixel_is "the prompt is above a fullscreen window that is above the others" 640 360 00ff00
do_input move 100 600
do_input click left
pixel_is "and stays there when that window is clicked beside it" 640 360 00ff00
layerc_stop prompt
check "the X11 window has the focus back" "[ \"\$(get XOnTop kbfocus)\" = 1 ]"
kill "$XONTOP"
wait "$XONTOP" 2>/dev/null
check "the X11 window is gone" "[ -z \"\$(get XOnTop x)\" ]"

# ---- clicks and the desktop ---------------------------------------------------

do_input key alt+F9
bring() { for _ in 1 2 3; do refresh; [ "$(get XTerm active)" = 1 ] && break; do_input key alt+Tab; done; }
bring
do_input key alt+F9
check "both windows are icons" "[ \"\$(get foot min)\" = 1 ] && [ \"\$(get XTerm min)\" = 1 ]"

do_input move 640 10
do_input click left
check "a click on a panel goes to the panel, not the root menu" \
	"has_event top button && [ \"\$(global menu open)\" = 0 ]"

layerc_start wall layer=background anchor=top,bottom,left,right size=0x0 color=123456
pixel_is "the background layer is drawn" 640 360 123456
sleep 0.45
do_input move 640 360
do_input click right
check "a click on a wallpaper tool opens the root menu" "[ \"\$(global menu open)\" = 1 ]"
do_input key Escape
has_event wall button && fail "the wallpaper tool should not see the click"
echo "ok: the wallpaper tool doesn't see the click"
layerc_stop wall

layerc_start dock layer=bottom anchor=bottom size=200x100 color=00ff00
pixel_is "a surface in the bottom layer" 640 680 00ff00
do_input move 640 680
do_input click left
check "a click on it goes to the client" \
	"has_event dock button && [ \"\$(global menu open)\" = 0 ]"
layerc_stop dock

# ---- popups -------------------------------------------------------------------

layerc_start pop layer=top anchor=bottom,left size=120x40 color=aa0000 popup=00aa00
wait_for "the popup is configured" has_event pop popup-configured
# Asked to open below a panel at the bottom edge, so it has to flip upwards.
pixel_is "the panel's popup is drawn and kept on the output" 50 650 00aa00
kill -0 "$pid" || fail "mwc died from a popup"
layerc_stop pop

# ---- the keyboard -------------------------------------------------------------

do_input move $(($(get foot icon_x) + 30)) $(($(get foot icon_y) + 30))
sleep 0.45
do_input click left
do_input click left
check "foot is back with the keyboard" "[ \"\$(get foot kbfocus)\" = 1 ]"

# A popup is opened to be seen, also if its panel is below the windows.
do_input key alt+F10
check "a window fills the screen" "grep -q ' max=1 ' \"\$MWC_TEST_STATE\""
sleep 0.2
for layer in bottom background; do
	layerc_start under layer=$layer anchor=bottom,left size=120x40 color=aa0000 popup=00aa00
	wait_for "the popup of a panel in the $layer layer" has_event under popup-configured
	pixel_is "it is drawn above the windows, in the right place" 50 650 00aa00
	WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/under.png" || fail "a screenshot"
	got=$(build/pngtool pixel "$SMOKE_DIR/under.png" 50 700)
	[ -n "$got" ] && [ "$got" != aa0000 ] ||
		fail "the panel itself should stay below the windows, not show ${got:-nothing}"
	echo "ok: and the panel itself stays below them"
	layerc_stop under
done
do_input key alt+F10
check "and back" "! grep -q ' max=1 ' \"\$MWC_TEST_STATE\""

layerc_start nokbd layer=top anchor=right size=50x50 keyboard=none color=444444
pixel_is "the surface is mapped" 1270 360 444444
check "keyboard=none leaves the keyboard alone" "[ \"\$(get foot kbfocus)\" = 1 ]"
layerc_stop nokbd

# A window going away behind an on-demand surface that has the keyboard: the next
# window becomes the active one, but the keys stay where the user types.
WAYLAND_DISPLAY=wayland-0 build/xdgc app=leaving >/dev/null 2>&1 &
PID_leaving=$!
check "a window has the keyboard" "[ \"\$(get leaving kbfocus)\" = 1 ]"
layerc_start od layer=top anchor=right size=50x50 keyboard=on-demand color=444444
wait_for "an on-demand surface takes it as it appears" has_event od keyboard-enter
kill "$PID_leaving"
wait "$PID_leaving" 2>/dev/null
check "the window is gone, foot is the active one" \
	"[ -z \"\$(get leaving x)\" ] && [ \"\$(get foot active)\" = 1 ]"
do_input key a
wait_for "the keys still go to the surface" has_event od "key 30"
has_event od keyboard-leave && fail "the surface lost the keyboard to the next window"
echo "ok: it keeps the keyboard"
layerc_stop od
check "when it goes, foot has the keyboard" "[ \"\$(get foot kbfocus)\" = 1 ]"
# The same for the shown tab of a group: the next tab comes to the front and is the
# active one, the keys stay with the surface (Review 2026-11-18 #16).
WAYLAND_DISPLAY=wayland-0 build/xdgc app=tab1 >/dev/null 2>&1 &
PID_tab1=$!
check "a first tab" "[ \"\$(get tab1 kbfocus)\" = 1 ]"
WAYLAND_DISPLAY=wayland-0 build/xdgc app=tab2 >/dev/null 2>&1 &
PID_tab2=$!
check "a second one" "[ \"\$(get tab2 kbfocus)\" = 1 ]"
do_input key super+g
check "the two are a group, the second shown" \
	"[ \"\$(get tab2 tab)\" != -1 ] && [ \"\$(get tab1 tab_hidden)\" = 1 ]"
layerc_start od2 layer=top anchor=right size=50x50 keyboard=on-demand color=444444
wait_for "an on-demand surface takes the keyboard" has_event od2 keyboard-enter
kill "$PID_tab2"
wait "$PID_tab2" 2>/dev/null
check "the shown tab is gone, the other one is shown and active" \
	"[ -z \"\$(get tab2 x)\" ] && [ \"\$(get tab1 tab_hidden)\" = 0 ] && \
	 [ \"\$(get tab1 active)\" = 1 ]"
do_input key a
wait_for "the keys still go to the surface" has_event od2 "key 30"
has_event od2 keyboard-leave && fail "the surface lost the keyboard to the next tab"
echo "ok: it keeps the keyboard"
layerc_stop od2
check "when it goes, the tab has the keyboard" "[ \"\$(get tab1 kbfocus)\" = 1 ]"
kill "$PID_tab1"
wait "$PID_tab1" 2>/dev/null
check "the tab is gone" "[ -z \"\$(get tab1 x)\" ]"
# The same for a shown tab the program takes out of its group, by turning fixed-size:
# it stays the active one, the keys stay with the surface.
WAYLAND_DISPLAY=wayland-0 build/xdgc app=tab3 >/dev/null 2>&1 &
PID_tab3=$!
check "a first tab" "[ \"\$(get tab3 kbfocus)\" = 1 ]"
WAYLAND_DISPLAY=wayland-0 build/xdgc app=tab4 fixed_after="$SMOKE_DIR/fixed" \
	>"$SMOKE_DIR/tab4.events" 2>&1 &
PID_tab4=$!
check "a second one" "[ \"\$(get tab4 kbfocus)\" = 1 ]"
do_input key super+g
check "the two are a group, the second shown" \
	"[ \"\$(get tab4 tab)\" != -1 ] && [ \"\$(get tab3 tab_hidden)\" = 1 ]"
layerc_start od3 layer=top anchor=right size=50x50 keyboard=on-demand color=444444
wait_for "an on-demand surface takes the keyboard" has_event od3 keyboard-enter
touch "$SMOKE_DIR/fixed"
wait_for "the shown tab turns fixed-size" grep -q '^fixed' "$SMOKE_DIR/tab4.events"
check "it leaves the group and stays active, the other tab is shown" \
	"[ \"\$(get tab4 tab)\" = -1 ] && [ \"\$(get tab3 tab_hidden)\" = 0 ] && \
	 [ \"\$(get tab4 active)\" = 1 ] && [ \"\$(get tab3 active)\" = 0 ]"
do_input key a
wait_for "the keys still go to the surface" has_event od3 "key 30"
has_event od3 keyboard-leave && fail "the surface lost the keyboard to a tab"
echo "ok: it keeps the keyboard"
layerc_stop od3
check "when it goes, that window has the keyboard" "[ \"\$(get tab4 kbfocus)\" = 1 ]"
kill "$PID_tab3" "$PID_tab4"
wait "$PID_tab3" "$PID_tab4" 2>/dev/null
check "the two are gone" "[ -z \"\$(get tab3 x)\" ] && [ -z \"\$(get tab4 x)\" ]"
# Without a surface: a hidden tab that leaves comes to the front, without the keys; the
# shown tab that leaves keeps the keys it had, in front.
WAYLAND_DISPLAY=wayland-0 build/xdgc app=tab5 fixed_after="$SMOKE_DIR/fixed5" \
	>"$SMOKE_DIR/tab5.events" 2>&1 &
PID_tab5=$!
check "a first tab" "[ \"\$(get tab5 kbfocus)\" = 1 ]"
WAYLAND_DISPLAY=wayland-0 build/xdgc app=tab6 >/dev/null 2>&1 &
PID_tab6=$!
check "a second one" "[ \"\$(get tab6 kbfocus)\" = 1 ]"
do_input key super+g
check "the two are a group, the second shown" \
	"[ \"\$(get tab6 tab)\" != -1 ] && [ \"\$(get tab5 tab_hidden)\" = 1 ]"
touch "$SMOKE_DIR/fixed5"
wait_for "the hidden tab turns fixed-size" grep -q '^fixed' "$SMOKE_DIR/tab5.events"
check "it leaves the group, raised over the other, which keeps the keyboard" \
	"[ \"\$(get tab5 tab)\" = -1 ] && [ \"\$(get tab5 tab_hidden)\" = 0 ] && \
	 [ \"\$(get tab5 rseq)\" -gt \"\$(get tab6 rseq)\" ] && [ \"\$(get tab6 kbfocus)\" = 1 ]"
WAYLAND_DISPLAY=wayland-0 build/xdgc app=tab7 fixed_after="$SMOKE_DIR/fixed7" \
	>"$SMOKE_DIR/tab7.events" 2>&1 &
PID_tab7=$!
check "a third one" "[ \"\$(get tab7 kbfocus)\" = 1 ]"
do_input key super+g
check "a group with the second, the third shown" \
	"[ \"\$(get tab7 tab)\" != -1 ] && [ \"\$(get tab6 tab_hidden)\" = 1 ]"
touch "$SMOKE_DIR/fixed7"
wait_for "the shown tab turns fixed-size" grep -q '^fixed' "$SMOKE_DIR/tab7.events"
check "it leaves the group with the keyboard, in front" \
	"[ \"\$(get tab7 tab)\" = -1 ] && [ \"\$(get tab7 kbfocus)\" = 1 ] && \
	 [ \"\$(get tab6 tab_hidden)\" = 0 ] && [ \"\$(get tab7 rseq)\" -gt \"\$(get tab6 rseq)\" ]"
kill "$PID_tab5" "$PID_tab6" "$PID_tab7"
wait "$PID_tab5" "$PID_tab6" "$PID_tab7" 2>/dev/null
check "the three are gone" \
	"[ -z \"\$(get tab5 x)\" ] && [ -z \"\$(get tab6 x)\" ] && [ -z \"\$(get tab7 x)\" ]"

# Two that insist: when the newer one goes, the keyboard goes to the other, not a window.
layerc_start lock1 layer=overlay anchor=left size=50x50 keyboard=exclusive color=555555
wait_for "exclusive keyboard: the first has it" has_event lock1 keyboard-enter
layerc_start lock2 layer=overlay anchor=right size=50x50 keyboard=exclusive color=555555
wait_for "exclusive keyboard: the second takes it" has_event lock2 keyboard-enter
: >"$SMOKE_DIR/lock1.events"
layerc_stop lock2
wait_for "the first has it back" has_event lock1 keyboard-enter
check "and no window got it" "[ \"\$(get foot kbfocus)\" = 0 ]"
do_input key a
wait_for "its keys arrive" has_event lock1 "key 30"
layerc_stop lock1
check "with both gone, the window has it again" "[ \"\$(get foot kbfocus)\" = 1 ]"

# Key repeat from mwmrc, also changed by a reload while a key is down: clients repeat
# themselves and must hear of it then.
RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
layerc_start rep layer=overlay anchor=left size=50x50 keyboard=exclusive color=555555
wait_for "a client hears the default repeat" has_event rep "repeat 25 600"
do_input press a
wait_for "the key is down" has_event rep "key 30"
printf 'Mwm*keyboardRepeatRate: 0\nMwm*keyboardRepeatDelay: 300\n' >"$RC"
reload_mwc
wait_for "a reload with the key down turns repeating off" has_event rep "repeat 0 300"
do_input lift a
: >"$SMOKE_DIR/rep.events" # the defaults from the start must not count
echo 'Mwm*keyboardRepeatRate: fast' >"$RC"
reload_mwc
wait_for "a bad value: the defaults again" has_event rep "repeat 25 600"
grep -q 'Mwm\*keyboardRepeatRate: expected 0 to 1000, not "fast"' "$LOG" ||
	fail "the bad repeat rate is not reported"
echo "ok: the bad repeat rate is reported"
# A second layout group, for clients' shortcuts under a non-Latin layout: the group
# vinput locks reaches the client.
stop_vinput
start_vinput 1280 720 us,ru
: >"$SMOKE_DIR/rep.events"
do_input group 1
wait_for "group 1 reaches the client" has_event rep "modifiers group=1"
do_input group 0
wait_for "and group 0 again" has_event rep "modifiers group=0"
# Just past the limits: a delay of 0 would stop mwc's own repeat, a rate over 1000
# its interval.
: >"$SMOKE_DIR/rep.events"
printf 'Mwm*keyboardRepeatRate: 50\nMwm*keyboardRepeatDelay: 300\n' >"$RC"
reload_mwc
wait_for "good values are taken" has_event rep "repeat 50 300"
printf 'Mwm*keyboardRepeatRate: 1001\nMwm*keyboardRepeatDelay: 0\n' >"$RC"
reload_mwc
wait_for "values past the limits: the defaults" has_event rep "repeat 25 600"
grep -q 'keyboardRepeatRate: expected 0 to 1000, not "1001"' "$LOG" &&
	grep -q 'keyboardRepeatDelay: expected 1 to 10000, not "0"' "$LOG" ||
	fail "values past the limits are not reported"
echo "ok: and reported"
rm "$RC"
layerc_stop rep

# Of two that insist, the higher layer keeps it: layer-shell gives it to the top-most.
layerc_start high layer=overlay anchor=left size=50x50 keyboard=exclusive color=555555
wait_for "exclusive in the overlay layer has it" has_event high keyboard-enter
layerc_start low layer=top anchor=right size=50x50 keyboard=exclusive color=555555
pixel_is "the one in the top layer is mapped" 1270 360 555555
do_input key a
wait_for "the overlay one keeps the keys" has_event high "key 30"
has_event low keyboard-enter && fail "the top layer one took the keyboard"
echo "ok: the top layer one waits"
layerc_stop high
wait_for "when the overlay one goes, the waiting one gets it" has_event low keyboard-enter
layerc_stop low
check "with both gone, the window has it again" "[ \"\$(get foot kbfocus)\" = 1 ]"

# A panel changing its zone with every commit: maximized windows follow, but not once
# per commit. About one a refresh (16 ms) is the most they get.
WAYLAND_DISPLAY=wayland-0 build/xdgc app=maxw pre=max states=1 >"$SMOKE_DIR/maxw.events" 2>&1 &
PID_maxw=$!
check "a maximized window" "[ \"\$(get maxw max)\" = 1 ]"
before=$(grep -c '^configure' "$SMOKE_DIR/maxw.events")
layerc_start flicker layer=top anchor=top,left,right size=0x30 exclusive=30 repaint=2
sleep 1
layerc_stop flicker
sleep 0.1
after=$(grep -c '^configure' "$SMOKE_DIR/maxw.events")
# At most one arrange per 16 ms: some 63 a second. Without the limit it is as many as
# the window can answer, well over 100 on a test machine.
[ $((after - before)) -le 80 ] ||
	fail "a second of a flickering zone gave the window $((after - before)) configures"
echo "ok: a flickering zone gives a maximized window $((after - before)) configures a second"
kill "$PID_maxw"
wait "$PID_maxw" 2>/dev/null

# A panel reserving more than there is doesn't leave windows nowhere to go.
layerc_start huge layer=top anchor=top,left,right size=0x30 exclusive=5000 color=444444
sleep 0.3
output_is "exclusive zone beyond the output: it isn't believed" 0 0 1280 720
layerc_stop huge
# All but a sliver: no more believable, windows would be squeezed into it.
layerc_start sliver layer=top anchor=top,left,right size=0x30 exclusive=680 color=444444
sleep 0.3
output_is "an exclusive zone leaving less than 100 px isn't believed either" 0 0 1280 720
layerc_stop sliver

layerc_start lock layer=overlay anchor=right size=50x50 keyboard=exclusive color=555555
wait_for "exclusive keyboard: focus on map" has_event lock keyboard-enter
check "the window lost the keyboard" "[ \"\$(get foot kbfocus)\" = 0 ]"
do_input key a
wait_for "exclusive keyboard: keys arrive" has_event lock "key 30"
do_input key alt+F9
check "window bindings are off meanwhile" "[ \"\$(get foot min)\" = 0 ]"
refresh
do_input move $(($(get foot x) + 100)) $(($(get foot y) + 100))
do_input click left
check "a click on a window doesn't take the keyboard" "[ \"\$(get foot kbfocus)\" = 0 ]"

# A menu or a grab from the mouse doesn't get its keys, and still ends with the mouse.
sleep 0.45
do_input move 1270 700
do_input click right
check "the root menu opens by mouse" "[ \"\$(global menu open)\" = 1 ]"
do_input key r
wait_for "its keys go to the surface" has_event lock "key 19"
check "and the menu stays open" "[ \"\$(global menu open)\" = 1 ]"
do_input move 900 100
do_input click left
check "the mouse closes the menu" "[ \"\$(global menu open)\" = 0 ]"
sleep 0.45
refresh
foot_w=$(get foot w)
edge_x=$(($(get foot x) + foot_w + 2 * $(get foot bw) - 2))
edge_y=$(($(get foot y) + 200))
do_input move "$edge_x" "$edge_y"
do_input down left
do_input move $((edge_x + 60)) "$edge_y"
do_input key Escape
wait_for "Escape during a resize goes to the surface" has_event lock "key 1\$"
do_input up left
check "and doesn't cancel it" "[ \"\$(get foot w)\" -gt $foot_w ]"
refresh
edge_x=$(($(get foot x) + $(get foot w) + 2 * $(get foot bw) - 2))
sleep 0.45
do_input move "$edge_x" "$edge_y"
do_input down left
do_input move $((edge_x - 60)) "$edge_y"
do_input up left
check "the window is back" "[ \"\$(get foot w)\" = $foot_w ]"

layerc_stop lock
check "the keyboard returns to the window" "[ \"\$(get foot kbfocus)\" = 1 ]"

layerc_start menu layer=top anchor=right size=50x50 keyboard=on-demand color=666666
wait_for "on-demand: focus on map" has_event menu keyboard-enter
check "the window lost the keyboard" "[ \"\$(get foot kbfocus)\" = 0 ]"
do_input key alt+F10
check "on-demand keeps the window bindings" "[ \"\$(get foot max)\" = 1 ]"
do_input key alt+F10
check "foot is back to normal" "[ \"\$(get foot max)\" = 0 ]"
refresh
do_input move $(($(get foot x) + 100)) $(($(get foot y) + 100))
do_input click left
check "on-demand: a click on a window takes the keyboard back" "[ \"\$(get foot kbfocus)\" = 1 ]"
do_input move 1270 360
do_input click left
check "on-demand: a click on the surface gives it back" "[ \"\$(get foot kbfocus)\" = 0 ]"
layerc_stop menu
check "on-demand: closing returns the keyboard" "[ \"\$(get foot kbfocus)\" = 1 ]"
layerc_stop top
output_is "no panels left" 0 0 1280 720
stop_vinput
stop_mwc
unset MWC_TEST_STATE

# ---- rootBackground: none -----------------------------------------------------

RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
echo 'Mwm*rootBackground: none' >"$RC"
start_mwc "sleep 120"
pixel_is "rootBackground none: nothing drawn" 640 360 000000
layerc_start wall layer=background anchor=top,bottom,left,right size=0x0 color=123456
pixel_is "rootBackground none: the tool's picture shows" 640 360 123456
layerc_stop wall
echo 'Mwm*rootBackground: #445566' >"$RC"
kill -HUP "$pid"
pixel_is "reload: back to a colour" 640 360 445566
stop_mwc
rm "$RC"
export MWC_ROOT=none
start_mwc "sleep 120"
pixel_is "MWC_ROOT=none" 640 360 000000
stop_mwc
unset MWC_ROOT

echo "PASS: layer"
