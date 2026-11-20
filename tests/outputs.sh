#!/bin/bash
# Headless test with two 1280x720 outputs side by side: what happens where the
# pointer or the window is must stay on that output.
# Output goes to build/outputs.log. Exits non-zero on failure.
LOG=build/outputs.log
. "$(dirname "$0")/lib.sh"

require_tools grim
for tool in build/vinput build/pngtool build/xdgc build/layerc build/outputc build/badclient; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
export WLR_HEADLESS_OUTPUTS=2
build/pngtool stripes "$SMOKE_DIR/wall.png" 1280 720 ff0000 0000ff
export MWC_WALLPAPER=$SMOKE_DIR/wall.png
start_mwc 'exec sleep 300'
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

do_input move 640 360
WAYLAND_DISPLAY=wayland-0 build/xdgc app=win size=300x200 >/dev/null 2>&1 &
PID_win=$!
check "the window is mapped on the first output" \
	"[ -n \"\$(get win x)\" ] && [ \"\$(get win x)\" -lt 1280 ]"
WX=$(get win x) WY=$(get win y)

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
client_is "Alt+F5 comes back from the other output" win "$WX" "$WY" 300 200

drag_to win 1920 0
check "dropped on the second output's top edge: maximized" "[ \"\$(get win max)\" = 1 ]"
frame_is "maximized on that output, not on the one it came from" win 1280 0 1280 720
do_input key alt+F5
client_is "and back" win "$WX" "$WY" 300 200

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
# A prompt with the keyboard on one output leaves a fullscreen window on the other
# where it is, over that output's panel.
WIN=808080
do_input key alt+F11
check "fullscreen on the second output" \
	"[ \"\$(get win full)\" = 1 ] && [ \"\$(get win x)\" = 1280 ]"
pixel_is "it covers the panel there" 1920 10 "$WIN"
# Its black backdrop follows it there, off the first output.
pixel_is "the first output keeps its wallpaper" 640 360 0000ff
do_input move 640 360
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top size=200x100 keyboard=exclusive \
	color=00ff00 >"$SMOKE_DIR/prompt.events" 2>&1 &
PID_prompt=$!
wait_for "a prompt on the first output has the keyboard" \
	grep -q keyboard-enter "$SMOKE_DIR/prompt.events"
pixel_is "it is drawn there" 640 360 00ff00
pixel_is "the window on the other output still covers its panel" 1920 10 "$WIN"
kill "$PID_prompt"
wait "$PID_prompt" 2>/dev/null
check "the window has the keyboard back" "[ \"\$(get win kbfocus)\" = 1 ]"
do_input key alt+F11
check "not fullscreen any more" "[ \"\$(get win full)\" = 0 ]"
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
reload_mwc
for edge in 2560 1280; do
	do_input move $((edge - 640)) 300
	WAYLAND_DISPLAY=wayland-0 build/xdgc app=corner size=300x200 >/dev/null 2>&1 &
	PID_corner=$!
	check "geometry -0-0 is in the corner of the output ending at $edge" \
		"[ -n \"\$(get corner x)\" ] && \
		[ \$((\$(get corner x) + \$(get corner w) + 2 * \$(get corner bw))) = $edge ] && \
		[ \$((\$(get corner y) + \$(get corner h) + 2 * \$(get corner bw) + \$(get corner th))) = 720 ]"
	kill "$PID_corner"
	wait "$PID_corner" 2>/dev/null
	check "the window is gone" "[ -z \"\$(get corner x)\" ]"
done
rm "$RC"
reload_mwc

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

# The oldest monitor unplugged while the places are fixed: the other one doesn't move.
mkdir -p "$XDG_CONFIG_HOME/mwc"
printf 'Mwm*HEADLESS-1*outputPosition: 0,0\nMwm*HEADLESS-2*outputPosition: 1280,0\n' \
	>"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'exec sleep 300'
start_vinput 2560 720
WAYLAND_DISPLAY=wayland-0 build/xdgc app=wa size=200x100 >/dev/null 2>&1 &
PID_wa=$!
check "a window" "[ -n \"\$(get wa x)\" ]"
# The oldest is the one announced first, whichever number the backend gave it.
oldest=$(sed -n 's/.*new output \(HEADLESS-[0-9]*\).*/\1/p' "$LOG" | head -1)
case $oldest in HEADLESS-1) other=HEADLESS-2 ;; HEADLESS-2) other=HEADLESS-1 ;;
*) fail "which output came first: '$oldest'" ;; esac
area() { refresh; echo "$(global "output_$1" x) $(global "output_$1" y) 1280 720"; }
other_area=$(area "$other")
# A panel on it, with the keyboard: closed with its monitor, the keyboard back to a window.
do_input move $(($(global "output_$oldest" x) + 640)) 360
WAYLAND_DISPLAY=wayland-0 build/layerc layer=overlay anchor=top,left,right size=0x30 \
	exclusive=30 keyboard=exclusive color=444444 >"$SMOKE_DIR/gone.events" 2>&1 &
PID_gone=$!
wait_for "a panel with the keyboard" grep -q keyboard-enter "$SMOKE_DIR/gone.events"
echo "$oldest" >"$MWC_TEST_STATE.unplug"
kill -USR2 "$pid"
check "$oldest is unplugged, the other stays where it was" \
	"[ -z \"\$(global output_$oldest x)\" ] && \
	[ \"\$(global output_$other x) \$(global output_$other y) 1280 720\" = \"$other_area\" ]"
wait_for "the panel is closed with it" grep -q '^closed' "$SMOKE_DIR/gone.events"
check "the window has the keyboard back" "[ \"\$(get wa kbfocus)\" = 1 ]"
kill "$PID_gone" 2>/dev/null
wait "$PID_gone" 2>/dev/null
kill "$PID_wa"
wait "$PID_wa" 2>/dev/null
rm "$XDG_CONFIG_HOME/mwc/mwmrc"
stop_vinput
stop_mwc

# An output shrinking by its scale: windows filling a state on it stay there, though
# their centre is now on the other output, which wlroots moved closer (mwmrc, placed
# automatically), or in the gap left beside it (wlr-output-management, placed as given).
RC=$XDG_CONFIG_HOME/mwc/mwmrc
echo 'Mwm*outputManagement: true' >"$RC"
start_mwc 'exec sleep 300'
start_vinput 2560 720
refresh
left=HEADLESS-1 right=HEADLESS-2
[ "$(global output_HEADLESS-2 x)" = 0 ] && left=HEADLESS-2 right=HEADLESS-1
do_input move 640 360
WAYLAND_DISPLAY=wayland-0 build/xdgc app=half size=300x200 >/dev/null 2>&1 &
PID_half=$!
check "a window on the left output" "[ \"\$(get half active)\" = 1 ]"
do_input key super+Right
frame_is "snapped to its right half" half 644 8 628 704
WAYLAND_DISPLAY=wayland-0 build/xdgc app=full size=300x200 >/dev/null 2>&1 &
PID_full=$!
check "another one" "[ \"\$(get full active)\" = 1 ]"
do_input key alt+F11
check "fullscreen on the left output" \
	"[ \"\$(get full full)\" = 1 ] && [ \"\$(get full x)\" = 0 ] && [ \"\$(get full w)\" = 1280 ]"
on_left() { # <description> <width of the left output>
	check "$1: the fullscreen window fills the left output" \
		"[ \"\$(get full full)\" = 1 ] && [ \"\$(get full x)\" = 0 ] && \
		[ \"\$(get full w)\" = $2 ]"
	check "$1: the snapped window is the left output's right half" \
		"[ \"\$(get half x)\" -ge $(($2 / 2)) ] && \
		[ \$((\$(get half x) + \$(get half w) + 2 * \$(get half bw))) -le $2 ]"
}
printf 'Mwm*outputManagement: true\nMwm*%s*outputScale: 2\n' "$left" >"$RC"
reload_mwc
check "mwmrc: the left output is half as wide, the other moved beside it" \
	"[ \"\$(global output_$left w)\" = 640 ] && [ \"\$(global output_$right x)\" = 640 ]"
on_left "mwmrc" 640
echo 'Mwm*outputManagement: true' >"$RC"
reload_mwc
check "scale 1 again" "[ \"\$(global output_$left w)\" = 1280 ]"
on_left "back" 1280
WAYLAND_DISPLAY=wayland-0 build/outputc apply "$left" scale 2 || fail "scale 2 on $left"
check "outputc: the left output is half as wide, a gap beside it" \
	"[ \"\$(global output_$left w)\" = 640 ] && [ \"\$(global output_$right x)\" = 1280 ]"
on_left "outputc" 640
kill "$PID_half" "$PID_full"
wait "$PID_half" "$PID_full" 2>/dev/null
rm "$XDG_CONFIG_HOME/mwc/mwmrc"
stop_vinput
stop_mwc

# The dock and the icon box are on the first monitor: unplugged, they go to the one that
# is first now, and its usable area leaves the dock's strip free (Review 2026-11-18 #36).
printf 'Mwm*HEADLESS-1*outputPosition: 0,0\nMwm*HEADLESS-2*outputPosition: 1280,0\n%s\n%s\n' \
	'Mwm*dtool*dock: true' 'Mwm*useIconBox: true' >"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'exec sleep 300'
oldest=$(sed -n 's/.*new output \(HEADLESS-[0-9]*\).*/\1/p' "$LOG" | head -1)
case $oldest in HEADLESS-1) other=HEADLESS-2 ;; HEADLESS-2) other=HEADLESS-1 ;;
*) fail "which output came first: '$oldest'" ;; esac
WAYLAND_DISPLAY=wayland-0 build/xdgc app=dtool size=100x40 >/dev/null 2>&1 &
PID_dtool=$!
check "a tool in the dock" "[ -n \"\$(get dtool x)\" ] && [ \"\$(global dock w)\" -gt 0 ]"
refresh
OX=$(global "output_$oldest" x) NX=$(global "output_$other" x)
inside() { # <x> <w> <output x>: x..x+w within that output
	[ "$1" -ge "$3" ] && [ $(($1 + $2)) -le $(($3 + 1280)) ]
}
inside "$(global dock x)" "$(global dock w)" "$OX" || fail "the dock isn't on $oldest"
inside "$(global iconbox x)" "$(global iconbox w)" "$OX" || fail "the icon box isn't on $oldest"
echo "ok: the dock and the icon box are on the first monitor"
echo "$oldest" >"$MWC_TEST_STATE.unplug"
kill -USR2 "$pid"
check "$oldest is unplugged" "[ -z \"\$(global output_$oldest x)\" ]"
check "the dock went to the other monitor" \
	"inside \"\$(global dock x)\" \"\$(global dock w)\" $NX && [ \"\$(global dock w)\" -gt 0 ]"
check "its tool with it" "inside \"\$(get dtool x)\" \"\$(get dtool w)\" $NX"
check "the icon box too" "inside \"\$(global iconbox x)\" \"\$(global iconbox w)\" $NX"
check "that monitor's usable area leaves the dock's strip free" \
	"[ \"\$(global output ux)\" = $NX ] && \
	 [ \$((\$(global output uy) + \$(global output uh))) -le \"\$(global dock y)\" ]"
kill "$PID_dtool"
wait "$PID_dtool" 2>/dev/null
rm "$XDG_CONFIG_HOME/mwc/mwmrc"
stop_mwc

# An output a client switches off has no place in the layout, and no wallpaper to show:
# the one rendered for it must not turn up at the layout's origin, over the other one.
# Rendered for 2560x720 its red edge is 640 wide, against 320 on the first output's own.
echo 'Mwm*outputManagement: true' >"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'exec sleep 300'
refresh
left=HEADLESS-1 right=HEADLESS-2
[ "$(global output_HEADLESS-2 x)" = 0 ] && left=HEADLESS-2 right=HEADLESS-1
WAYLAND_DISPLAY=wayland-0 build/outputc apply "$right" 2560x720 || fail "a wider $right"
check "the second output is 2560 wide" "[ \"\$(global output_$right w)\" = 2560 ]"
pixel_is "the first output has its own wallpaper" 400 360 0000ff
WAYLAND_DISPLAY=wayland-0 build/outputc apply "$right" off || fail "$right off"
check "the second output is out of the layout" "[ \"\$(global output_$right w)\" = 0 ]"
pixel_is "and the first still has its own wallpaper" 400 360 0000ff
# A wallpaper changed while it is off is the one it shows when it comes back.
build/pngtool stripes "$SMOKE_DIR/wall.png" 1280 720 00ff00 00ff00
reload_mwc
pixel_is "the first output has the new wallpaper" 400 360 00ff00
WAYLAND_DISPLAY=wayland-0 build/outputc apply "$right" on 2560x720 || fail "$right on"
check "the second output is back, 2560 wide" "[ \"\$(global output_$right w)\" = 2560 ]"
refresh
pixel_is "with the new wallpaper" $(($(global output_$right x) + 1280)) 360 00ff00
rm "$XDG_CONFIG_HOME/mwc/mwmrc"
stop_mwc
echo "PASS: outputs"
