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
preview_is "the first output's right edge: its right half" 640 0 640 720
do_input move 1280 360
preview_is "the second output's left edge: its left half" 1280 0 640 720
do_input move 2559 360
preview_is "the second output's right edge: its right half" 1920 0 640 720
do_input up left
frame_is "dropped there" win 1920 0 640 720
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
frame_is "Super+Left: a half of the output it is on" win 1280 0 640 720
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
preview_is "the other output has all its room" 0 0 640 720
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
frame_is "snapped there, it comes back snapped there" win 1920 0 640 720
kill "$PID_win"
wait "$PID_win" 2>/dev/null
check "the window is gone" "[ -z \"\$(get win x)\" ]"
stop_vinput
stop_mwc
echo "PASS: outputs"
