#!/bin/bash
# Headless layer-shell test: panels reserving space, stacking against windows,
# keyboard rules and clicks, using tests/layerc.c as the client.
# Output goes to build/layer.log. Exits non-zero on failure.
LOG=build/layer.log
. "$(dirname "$0")/lib.sh"

require_tools foot xterm Xwayland grim
for tool in build/vinput build/pngtool build/layerc; do
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
start_mwc "foot sh -c '$marker' foot-up & \
	while [ ! -e \"\$SMOKE_DIR/foot-up\" ]; do sleep 0.1; done; \
	exec xterm -e sh -c '$marker' xterm-up"
wait_for "foot connected (Wayland)" test -e "$SMOKE_DIR/foot-up"
wait_for "xterm connected (XWayland)" test -e "$SMOKE_DIR/xterm-up"
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
sleep 0.2
WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/shot.png"
[ "$(build/pngtool pixel "$SMOKE_DIR/shot.png" 640 10)" != "$PANEL" ] ||
	fail "a focused fullscreen window should cover the panel"
echo "ok: a focused fullscreen window covers the panel"
do_input key alt+Tab
check "xterm has the focus" "[ \"\$(get XTerm active)\" = 1 ] && [ \"\$(get foot full)\" = 1 ]"
pixel_is "without the focus, the panel is visible again" 640 10 "$PANEL"
do_input key alt+Tab
check "foot has the focus again" "[ \"\$(get foot active)\" = 1 ]"
do_input key alt+F11
check "foot leaves fullscreen" "[ \"\$(get foot full)\" = 0 ]"
pixel_is "the panel is back on top" 640 10 "$PANEL"

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

layerc_start nokbd layer=top anchor=right size=50x50 keyboard=none color=444444
pixel_is "the surface is mapped" 1270 360 444444
check "keyboard=none leaves the keyboard alone" "[ \"\$(get foot kbfocus)\" = 1 ]"
layerc_stop nokbd

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

# A panel reserving more than there is doesn't leave windows nowhere to go.
layerc_start huge layer=top anchor=top,left,right size=0x30 exclusive=5000 color=444444
sleep 0.3
output_is "exclusive zone beyond the output: it isn't believed" 0 0 1280 720
layerc_stop huge

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
