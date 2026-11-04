#!/bin/bash
# Headless test of snapping: dragging against edges and corners, the Super+arrow
# steps, and coming back from a snapped state in every way there is.
# Output goes to build/snap.log. Exits non-zero on failure.
LOG=build/snap.log
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

x11() {
	local name=$1
	shift
	DISPLAY=$X_DISPLAY build/xclient class="$name" "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "PID_$name=$!"
	check "$name is mapped" "[ -n \"\$(get $name x)\" ]"
}
wayland() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$name" "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
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

# The frame's box on screen.
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
# Drag by the title to (x, y) and let go.
drag_to() { # app x y
	grab_title "$1"
	do_input move "$2" "$3"
	do_input up left
}

# ---- every zone, with the mouse ------------------------------------------------

x11 win pos=100,100 size=300x200

# The state file writes spaces as _.
zone() { # x y name preview:x y w h
	grab_title win
	do_input move "$1" "$2"
	preview_is "dragging to $3: the target is shown" "$4" "$5" "$6" "$7"
	check "dragging to $3: the box names it" "[ \"\$(global feedback text)\" = '$3' ]"
	pixel_is "dragging to $3: the outline is drawn" $(($4 + 3)) $(($5 + $7 / 4)) 7387ad
	do_input up left
	if [ "$3" = Maximize ]; then
		check "dropped: maximized" "[ \"\$(get win max)\" = 1 ] && [ \"\$(get win snap)\" = 0 ]"
	else
		frame_is "dropped: $3" win "$4" "$5" "$6" "$7"
		check "dropped: it counts as snapped" "[ \"\$(get win snap)\" != 0 ]"
	fi
	check "the target is gone" "[ \"\$(global preview shown)\" = 0 ]"
	do_input key alt+F5
	client_is "Alt+F5 puts it back" win 100 100 300 200
}
zone 0 360 Left_half 0 0 640 720
zone 1279 360 Right_half 640 0 640 720
zone 0 10 Top_left 0 0 640 360
zone 1279 10 Top_right 640 0 640 360
zone 0 710 Bottom_left 0 360 640 360
zone 1279 710 Bottom_right 640 360 640 360
zone 10 0 Top_left 0 0 640 360
zone 1270 0 Top_right 640 0 640 360
zone 10 719 Bottom_left 0 360 640 360
zone 1270 719 Bottom_right 640 360 640 360
zone 640 0 Maximize 0 0 1280 720

grab_title win
do_input move 640 719
check "the bottom edge away from the corners is no zone" "[ \"\$(global preview shown)\" = 0 ]"
do_input key Escape
do_input up left
client_is "and Escape cancels" win 100 100 300 200

# ---- coming back from a snapped state --------------------------------------------

drag_to win 0 360
frame_is "snapped to the left" win 0 0 640 720
grab_title win
do_input move 320 300
check "dragging it away gives it back its size" \
	"[ \"\$(get win snap)\" = 0 ] && [ \"\$(global outline w)\" = $((300 + 2 * B)) ] && \
	[ \"\$(global outline h)\" = $((200 + 2 * B + T)) ]"
do_input key Escape
frame_is "Escape snaps it again" win 0 0 640 720
check "and it counts as snapped" "[ \"\$(get win snap)\" != 0 ]"
do_input up left
do_input key alt+F5
client_is "it still remembers its place" win 100 100 300 200

drag_to win 0 360
grab_title win
do_input move 320 300
do_input up left
check "dragged away it is free, with the old size" \
	"[ \"\$(get win snap)\" = 0 ] && [ \"\$(get win w)\" = 300 ] && [ \"\$(get win h)\" = 200 ]"
do_input key alt+F10
check "maximized" "[ \"\$(get win max)\" = 1 ]"
do_input key alt+F10
check "not maximized any more" "[ \"\$(get win max)\" = 0 ] && [ \"\$(get win w)\" = 300 ]"
WX=$(get win x) WY=$(get win y)

# From one zone straight to another: the place from before the first one counts.
drag_to win 0 360
drag_to win 1279 360
frame_is "from half to half" win 640 0 640 720
do_input key alt+F5
client_is "Alt+F5 goes back to the place before the first snap" win "$WX" "$WY" 300 200

# Snapped, then maximized and back.
drag_to win 0 360
do_input key alt+F10
check "a snapped window maximizes" "[ \"\$(get win max)\" = 1 ] && [ \"\$(get win snap)\" = 0 ]"
do_input key alt+F10
client_is "and un-maximizes to its place from before" win "$WX" "$WY" 300 200

# Snapped, then fullscreen and back.
drag_to win 0 360
do_input key alt+F11
check "a snapped window goes fullscreen" "[ \"\$(get win full)\" = 1 ] && [ \"\$(get win w)\" = 1280 ]"
do_input key alt+F11
frame_is "and comes back snapped" win 0 0 640 720
do_input key alt+F5
client_is "its place from before is still known" win "$WX" "$WY" 300 200

# Resizing a snapped window frees it.
drag_to win 0 360
refresh
RX=$((640 - 2)) RY=360
do_input move "$RX" "$RY"
do_input down left
do_input move $((RX - 100)) "$RY"
do_input key Escape
frame_is "Escape keeps a snapped window snapped" win 0 0 640 720
check "still counted as snapped" "[ \"\$(get win snap)\" != 0 ]"
do_input up left
do_input move "$RX" "$RY"
do_input down left
do_input move $((RX - 100)) "$RY"
do_input up left
frame_is "resized, it is narrower" win 0 0 540 720
check "and no longer snapped" "[ \"\$(get win snap)\" = 0 ]"
do_input key alt+F5
frame_is "so Alt+F5 has nothing to restore" win 0 0 540 720

# With opaque dragging the window has already moved when it is dropped.
mkdir -p "$XDG_CONFIG_HOME/mwc"
echo 'Mwm*moveOpaque: true' >"$XDG_CONFIG_HOME/mwc/mwmrc"
kill -HUP "$pid"
sleep 0.2
stop win
x11 opaque pos=100,100 size=300x200
drag_to opaque 0 360
frame_is "moveOpaque: snapped" opaque 0 0 640 720
do_input key alt+F5
client_is "moveOpaque: back to where the drag began, not to the screen's edge" opaque 100 100 300 200
grab_title opaque
do_input move 0 360
do_input key Escape
client_is "moveOpaque: Escape in a zone puts it back" opaque 100 100 300 200
check "and nothing is snapped" "[ \"\$(get opaque snap)\" = 0 ] && [ \"\$(global preview shown)\" = 0 ]"
do_input up left
stop opaque
rm "$XDG_CONFIG_HOME/mwc/mwmrc"
kill -HUP "$pid"
sleep 0.2

# ---- Super+arrows ----------------------------------------------------------------

x11 keys pos=100,100 size=300x200
do_input key super+Left
frame_is "Super+Left: left half" keys 0 0 640 720
do_input key super+Right
client_is "Super+Right goes back" keys 100 100 300 200
do_input key super+Right
frame_is "Super+Right: right half" keys 640 0 640 720
do_input key super+Up
frame_is "Super+Up from a half: top quarter" keys 640 0 640 360
do_input key super+Left
frame_is "Super+Left: the other top quarter" keys 0 0 640 360
do_input key super+Down
frame_is "Super+Down: back to the half" keys 0 0 640 720
do_input key super+Down
frame_is "Super+Down: bottom quarter" keys 0 360 640 360
do_input key super+Right
frame_is "Super+Right: the other bottom quarter" keys 640 360 640 360
do_input key super+Up
frame_is "Super+Up: back to the half" keys 640 0 640 720
do_input key super+Left
client_is "Super+Left goes back" keys 100 100 300 200
do_input key super+Up
check "Super+Up from normal: maximized" "[ \"\$(get keys max)\" = 1 ]"
do_input key super+Up
check "Super+Up again: nothing more" "[ \"\$(get keys max)\" = 1 ]"
do_input key super+Down
client_is "Super+Down from maximized: normal" keys 100 100 300 200
do_input key super+Down
check "Super+Down from normal: minimized" "[ \"\$(get keys min)\" = 1 ]"
do_input move $(($(get keys icon_x) + 30)) $(($(get keys icon_y) + 30))
settle
do_input click left
do_input click left
check "back from the icon" "[ \"\$(get keys min)\" = 0 ]"

# ---- Super+Shift+arrows ----------------------------------------------------------

client_is "at its place" keys 100 100 300 200
OW=$((300 + 2 * B)) OH=$((200 + 2 * B + T))
do_input key super+shift+Left
client_is "Super+Shift+Left: the left edge" keys 0 100 300 200
do_input key super+shift+Up
client_is "Super+Shift+Up: a corner" keys 0 0 300 200
do_input key super+shift+Right
client_is "Super+Shift+Right: the right edge" keys $((1280 - OW)) 0 300 200
do_input key super+shift+Down
client_is "Super+Shift+Down: the bottom edge" keys $((1280 - OW)) $((720 - OH)) 300 200
do_input key super+Left
do_input key super+shift+Right
frame_is "Super+Shift does nothing to a snapped window" keys 0 0 640 720
do_input key super+Right
do_input key alt+F10
do_input key super+shift+Left
check "nor to a maximized one" "[ \"\$(get keys max)\" = 1 ] && [ \"\$(get keys x)\" = 0 ]"
do_input key alt+F10
stop keys

# ---- what may not snap -----------------------------------------------------------

x11 fixed pos=100,100 size=300x200 min=300x200 max=300x200
grab_title fixed
do_input move 0 360
check "a fixed size has no zones" "[ \"\$(global preview shown)\" = 0 ]"
do_input key Escape
do_input up left
do_input key super+Left
client_is "and no Super+arrows" fixed 100 100 300 200
stop fixed

# Maximize is taken away, the other zones remain.
x11 nomax pos=100,100 size=300x200 motif=17,1
grab_title nomax
do_input move 640 0
check "without Maximize, the top edge is no zone" "[ \"\$(global preview shown)\" = 0 ]"
do_input move 0 360
preview_is "but the side is" 0 0 640 720
do_input key Escape
do_input up left
do_input key super+Up
client_is "and Super+Up doesn't maximize" nomax 100 100 300 200
stop nomax

# ---- limits, panels, Wayland ----------------------------------------------------

x11 limited pos=100,100 size=300x200 max=400x300
do_input key super+Left
check "a maximum size holds when snapped" "[ \"\$(get limited w)\" = 400 ] && [ \"\$(get limited h)\" = 300 ] && [ \"\$(get limited x)\" = 0 ]"
do_input key super+Right
stop limited

x11 normal pos=100,100 size=300x200
do_input key super+Right
frame_is "snapped, no panel" normal 640 0 640 720
x11 panel type=dock pos=0,0 size=1280x40 strut=0,0,40,0
frame_is "snapped windows follow a new panel" normal 640 40 640 680
grab_title normal
do_input move 0 360
preview_is "the target leaves the panel out" 0 40 640 680
do_input key Escape
do_input up left
stop panel
frame_is "and the free space when it goes" normal 640 0 640 720
stop normal

wayland wl size=300x200
refresh
WX=$(get wl x) WY=$(get wl y)
do_input key super+Left
frame_is "Wayland: left half" wl 0 0 640 720
do_input key super+Up
frame_is "Wayland: top quarter" wl 0 0 640 360
do_input key super+Down
do_input key super+Right
client_is "Wayland: and back" wl "$WX" "$WY" 300 200
drag_to wl 1279 360
frame_is "Wayland: dragged to a half" wl 640 0 640 720
grab_title wl
do_input move 640 300
do_input up left
check "Wayland: dragged away, the old size" "[ \"\$(get wl w)\" = 300 ] && [ \"\$(get wl h)\" = 200 ]"
stop wl

stop_vinput
stop_mwc
echo "PASS: snap"
