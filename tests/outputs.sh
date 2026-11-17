#!/bin/bash
# Headless test with two 1280x720 outputs side by side: what happens where the
# pointer or the window is must stay on that output.
# Output goes to build/outputs.log. Exits non-zero on failure.
LOG=build/outputs.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland grim
for tool in build/vinput build/pngtool build/xclient build/layerc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
export WLR_HEADLESS_OUTPUTS=2
build/pngtool stripes "$SMOKE_DIR/wall.png" 1280 720 ff0000 0000ff
export MWC_WALLPAPER=$SMOKE_DIR/wall.png
start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
wait_for "the startup command ran" test -s "$SMOKE_DIR/display"
X_DISPLAY=$(cat "$SMOKE_DIR/display")
start_vinput 2560 720
refresh
B=$(global theme border)
T=$(global theme title)

settle() { sleep 0.45; }
frame_is() { # description app x y w h
	check "$1" "[ \"\$(get $2 x)\" = $3 ] && [ \"\$(get $2 y)\" = $4 ] && \
		[ \$((\$(get $2 w) + 2 * \$(get $2 bw))) = $5 ] && \
		[ \$((\$(get $2 h) + 2 * \$(get $2 bw) + \$(get $2 th))) = $6 ]"
}
client_is() { # description app x y w h
	check "$1" "[ \"\$(get $2 x)\" = $3 ] && [ \"\$(get $2 y)\" = $4 ] && \
		[ \"\$(get $2 w)\" = $5 ] && [ \"\$(get $2 h)\" = $6 ]"
}
preview_is() { # description x y w h
	check "$1" "[ \"\$(global preview shown)\" = 1 ] && [ \"\$(global preview x)\" = $2 ] && \
		[ \"\$(global preview y)\" = $3 ] && [ \"\$(global preview w)\" = $4 ] && \
		[ \"\$(global preview h)\" = $5 ]"
}
title_x() { refresh; echo $(($(get "$1" x) + ($(get "$1" w) + 2 * $(get "$1" bw)) / 2)); }
title_y() { refresh; echo $(($(get "$1" y) + $(get "$1" bw) + $(get "$1" th) / 2)); }
grab_title() {
	settle
	do_input move "$(title_x "$1")" "$(title_y "$1")"
	do_input down left
}
drag_to() { # app x y
	grab_title "$1"
	do_input move "$2" "$3"
	do_input up left
}

pixel_is "the wallpaper fills the first output" 640 360 0000ff
pixel_is "and the second, on its own" 1920 360 0000ff
pixel_is "starting at its own edge" 1290 360 ff0000

DISPLAY=$X_DISPLAY build/xclient class=win pos=100,100 size=300x200 >/dev/null 2>&1 &
PID_win=$!
check "the window is mapped" "[ -n \"\$(get win x)\" ]"

# ---- snapping follows the pointer -----------------------------------------------

grab_title win
do_input move 1279 360
preview_is "the first output's right edge: its right half" 644 8 628 704
do_input move 1280 360
preview_is "the second output's left edge: its left half" 1288 8 628 704
do_input move 2559 360
preview_is "the second output's right edge: its right half" 1924 8 628 704
do_input up left
frame_is "dropped there" win 1924 8 628 704
do_input key alt+F5
client_is "Alt+F5 comes back from the other output" win 100 100 300 200

drag_to win 1920 0
check "dropped on the second output's top edge: maximized" "[ \"\$(get win max)\" = 1 ]"
frame_is "maximized on that output, not on the one it came from" win 1280 0 1280 720
do_input key alt+F5
client_is "and back" win 100 100 300 200

# ---- a window on the second output ----------------------------------------------

drag_to win 1500 200
check "the window is on the second output" "[ \"\$(get win x)\" -gt 1280 ]"
refresh
X=$(get win x) Y=$(get win y)

do_input key super+Left
frame_is "Super+Left: a half of the output it is on" win 1288 8 628 704
do_input key super+Right
do_input key alt+F10
frame_is "Alt+F10: that output" win 1280 0 1280 720
do_input key alt+F10
do_input key alt+F11
check "Alt+F11: that output" \
	"[ \"\$(get win full)\" = 1 ] && [ \"\$(get win x)\" = 1280 ] && [ \"\$(get win w)\" = 1280 ]"
do_input key alt+F11
client_is "and back to its place" win "$X" "$Y" 300 200
do_input key super+shift+Right
client_is "Super+Shift+Right: that output's edge" win $((2560 - 300 - 2 * B)) "$Y" 300 200
do_input key alt+F7
check "the box is in the middle of that output" \
	"[ \"\$(global feedback x)\" -gt 1280 ] && [ \"\$(global feedback x)\" -lt 2560 ]"
do_input key Escape

# ---- panels belong to one output -------------------------------------------------

do_input move 1920 600
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top anchor=top,left,right size=0x30 exclusive=30 color=ff00ff \
	>"$SMOKE_DIR/panel.events" 2>&1 &
PID_panel=$!
wait_for "the panel is configured" grep -q configured "$SMOKE_DIR/panel.events"
pixel_is "the panel opens on the output under the pointer" 1920 10 ff00ff
pixel_is "and only there" 640 10 0000ff
do_input key alt+F10
frame_is "a maximized window there stays below it" win 1280 30 1280 690
do_input key alt+F10
check "not maximized any more" "[ \"\$(get win max)\" = 0 ]"
settle
do_input move "$(title_x win)" "$(title_y win)"
do_input down left
do_input move 0 360
preview_is "the other output has all its room" 8 8 628 704
do_input key Escape
do_input up left
kill "$PID_panel"
wait "$PID_panel" 2>/dev/null

# ---- the tiling layout covers the first output --------------------------------------

refresh
X=$(get win x) Y=$(get win y)
do_input key super+t
check "tiling: the window goes to the first output" \
	"[ \"\$(get win tiled)\" = 1 ] && [ \"\$(get win x)\" = 8 ] && \
	[ \$((\$(get win w) + 2 * B)) = 1264 ]"
do_input key super+t
client_is "tiling off: back on the output it came from" win "$X" "$Y" 300 200
do_input key super+Right
do_input key super+t
do_input key super+t
frame_is "snapped there, it comes back snapped there" win 1924 8 628 704
kill "$PID_win"
wait "$PID_win" 2>/dev/null
check "the window is gone" "[ -z \"\$(get win x)\" ]"

# ---- a place from mwmrc is on the output under the pointer ------------------------

RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
echo 'Mwm*corner*geometry: -0-0' >"$RC"
kill -HUP "$pid"
settle
for edge in 2560 1280; do
	do_input move $((edge - 640)) 300
	DISPLAY=$X_DISPLAY build/xclient class=corner size=300x200 >/dev/null 2>&1 &
	PID_corner=$!
	check "the corner of the output ending at $edge" \
		"[ -n \"\$(get corner x)\" ] && \
		[ \$((\$(get corner x) + \$(get corner w) + 2 * \$(get corner bw))) = $edge ] && \
		[ \$((\$(get corner y) + \$(get corner h) + 2 * \$(get corner bw) + \$(get corner th))) = 720 ]"
	kill "$PID_corner"
	wait "$PID_corner" 2>/dev/null
	check "the window is gone" "[ -z \"\$(get corner x)\" ]"
done
rm "$RC"
kill -HUP "$pid"
settle

# A panel on the inner edge of the right output: a strut counts from the edge of the whole
# screen, so it reaches over all of the left output, where it means nothing.
DISPLAY=$X_DISPLAY build/xclient class=topdock type=dock pos=0,0 size=1280x40 strut=0,0,40,0 >/dev/null 2>&1 &
PID_topdock=$!
check "a panel at the top" "[ \"\$(global output uy)\" = 40 ]"
DISPLAY=$X_DISPLAY build/xclient class=innerdock type=dock pos=1280,0 size=30x720 strut=1310,0,0,0 >/dev/null 2>&1 &
PID_innerdock=$!
check "a panel on the inner edge" "[ -n \"\$(get innerdock x)\" ]"
sleep 0.3
check "the left output keeps its own strut and ignores the other" \
	"[ \"\$(global output ux)\" = 0 ] && [ \"\$(global output uy)\" = 40 ] && \
	[ \"\$(global output uw)\" = 1280 ] && [ \"\$(global output uh)\" = 680 ]"
do_input move 1900 400
DISPLAY=$X_DISPLAY build/xclient class=rightwin pos=1500,100 size=300x200 >/dev/null 2>&1 &
PID_rightwin=$!
check "a window on the right" "[ \"\$(get rightwin active)\" = 1 ]"
do_input key alt+F10
frame_is "maximized on the right, it stays out of both panels" rightwin 1310 40 1250 680
kill "$PID_rightwin" "$PID_innerdock" "$PID_topdock"
wait "$PID_rightwin" "$PID_innerdock" "$PID_topdock" 2>/dev/null

# A panel changing its strut as fast as it can: the usable area is worked out again
# at most once a refresh (16 ms), as for a layer surface, not once per change.
DISPLAY=$X_DISPLAY build/xclient class=waf workarea=1 >"$SMOKE_DIR/wa-flicker.events" 2>&1 &
PID_waf=$!
check "a window that hears of the workarea" "[ -n \"\$(get waf x)\" ]"
DISPLAY=$X_DISPLAY build/xclient class=flicker type=dock pos=0,0 size=1280x40 \
	strut=0,0,40,0 strut_flicker=300 >"$SMOKE_DIR/flicker.events" 2>&1 &
PID_flicker=$!
wait_for "the panel has flickered" grep -q flickered "$SMOKE_DIR/flicker.events"
check "the last strut counts" "[ \"\$(global output uy)\" = 40 ]"
n=$(grep -c '^workarea' "$SMOKE_DIR/wa-flicker.events")
[ "$n" -le 60 ] || fail "300 strut changes made $n workarea changes"
echo "ok: 300 strut changes made $n workarea changes"
kill "$PID_flicker" "$PID_waf"
wait "$PID_flicker" "$PID_waf" 2>/dev/null

# A submenu of a window across both monitors stays on the monitor its menu opened on,
# the right one: not slid back onto the left one, where the window begins.
do_input move 100 100
WAYLAND_DISPLAY=wayland-0 build/badclient submenu sub >"$SMOKE_DIR/sub.log" 2>&1 &
PID_sub=$!
check "the window is mapped" "[ \"\$(get sub active)\" = 1 ]"
do_input key alt+F7
refresh
do_input move $(($(global cursor x) + 1100 - $(get sub x))) $(($(global cursor y) + 100 - $(get sub y)))
do_input key Return
# Give or take a pixel: vinput aims at pixel centres.
check "a window across both monitors" "[ \"\$(get sub x)\" -ge 1095 ] && [ \"\$(get sub x)\" -le 1105 ]"
do_input move $(($(get sub x) + 50)) $(($(get sub y) + 50))
do_input click left
wait_for "its menu opens a submenu" grep -q '^sub-at' "$SMOKE_DIR/sub.log"
grep -q '^sub-at 51 26$' "$SMOKE_DIR/sub.log" ||
	fail "the submenu was moved: $(grep '^sub-at' "$SMOKE_DIR/sub.log")"
echo "ok: the submenu is where it asked to be"
kill "$PID_sub"
wait "$PID_sub" 2>/dev/null

# Fullscreen on the output the client names, not the one the window is on.
do_input move 100 100
WAYLAND_DISPLAY=wayland-0 build/xdgc app=fsout size=300x200 fs_output=1 \
	>"$SMOKE_DIR/fsout.events" 2>&1 &
PID_fsout=$!
check "it opens on the left output" "[ -n \"\$(get fsout x)\" ] && [ \"\$(get fsout x)\" -lt 1280 ]"
wait_for "it asks for fullscreen on the right one" grep -q 'fullscreen asked' "$SMOKE_DIR/fsout.events"
check "it is fullscreen there" "[ \"\$(get fsout full)\" = 1 ] && [ \"\$(get fsout x)\" = 1280 ] && \
	[ \"\$(get fsout w)\" = 1280 ]"
kill "$PID_fsout"
wait "$PID_fsout" 2>/dev/null
stop_vinput
stop_mwc

# The oldest monitor unplugged while the places are fixed: the other one doesn't move,
# and X11 programs must still hear its workarea, now the first one's.
mkdir -p "$XDG_CONFIG_HOME/mwc"
printf 'Mwm*HEADLESS-1*outputPosition: 0,0\nMwm*HEADLESS-2*outputPosition: 1280,0\n' \
	>"$XDG_CONFIG_HOME/mwc/mwmrc"
rm -f "$SMOKE_DIR/display"
start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
wait_for "the startup command ran" test -s "$SMOKE_DIR/display"
X_DISPLAY=$(cat "$SMOKE_DIR/display")
start_vinput 2560 720
DISPLAY=$X_DISPLAY build/xclient class=wa workarea=1 size=200x100 >"$SMOKE_DIR/wa.log" 2>&1 &
PID_wa=$!
check "a window that watches the workarea" "[ -n \"\$(get wa x)\" ]"
workarea_is() { [ "$(grep '^workarea' "$SMOKE_DIR/wa.log" | tail -1)" = "workarea $1" ]; }
# The oldest is the one announced first, whichever number the backend gave it.
oldest=$(sed -n 's/.*new output \(HEADLESS-[0-9]*\).*/\1/p' "$LOG" | head -1)
case $oldest in HEADLESS-1) other=HEADLESS-2 ;; HEADLESS-2) other=HEADLESS-1 ;;
*) fail "which output came first: '$oldest'" ;; esac
area() { refresh; echo "$(global "output_$1" x) $(global "output_$1" y) 1280 720"; }
first_area=$(area "$oldest") other_area=$(area "$other")
wait_for "the workarea is the oldest monitor's ($oldest)" workarea_is "$first_area"
echo "$oldest" >"$MWC_TEST_STATE.unplug"
kill -USR2 "$pid"
check "$oldest is unplugged, the other stays where it was" \
	"[ -z \"\$(global output_$oldest x)\" ] && \
	[ \"\$(global output_$other x) \$(global output_$other y) 1280 720\" = \"$other_area\" ]"
wait_for "the workarea is now the other monitor's" workarea_is "$other_area"
kill "$PID_wa"
wait "$PID_wa" 2>/dev/null
rm "$XDG_CONFIG_HOME/mwc/mwmrc"
stop_vinput
stop_mwc
echo "PASS: outputs"
