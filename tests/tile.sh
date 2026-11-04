#!/bin/bash
# Headless test of tiling: the layout, what the title bar buttons do in it, which
# windows stay out, the settings, and getting back out of it all.
# Output goes to build/tile.log. Exits non-zero on failure.
LOG=build/tile.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland grim
for tool in build/vinput build/pngtool build/xclient build/xdgc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"

begin() { # [startup env is already set]
	rm -f "$SMOKE_DIR/display"
	start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
	wait_for "the startup command ran" test -s "$SMOKE_DIR/display"
	X_DISPLAY=$(cat "$SMOKE_DIR/display")
	start_vinput
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

frame_w() { echo $(($(get "$1" w) + 2 * $(get "$1" bw))); }
# The height of the frame on screen, which is what a shaded window has left.
frame_h() { echo $(($(get "$1" fh))); }

cell_is() { # description app x y w h: the frame's box
	check "$1" "[ \"\$(get $2 tiled)\" = 1 ] && [ \"\$(get $2 x)\" = $3 ] && \
		[ \"\$(get $2 y)\" = $4 ] && [ \"\$(frame_w $2)\" = $5 ] && [ \"\$(frame_h $2)\" = $6 ]"
}
floats_at() { # description app x y w h: the client's size
	check "$1" "[ \"\$(get $2 tiled)\" = 0 ] && [ \"\$(get $2 x)\" = $3 ] && \
		[ \"\$(get $2 y)\" = $4 ] && [ \"\$(get $2 w)\" = $5 ] && [ \"\$(get $2 h)\" = $6 ]"
}
button() { # app min|max: click a title bar button
	refresh
	local right=$(($(get "$1" x) + $(frame_w "$1") - $(get "$1" bw)))
	local y=$(($(get "$1" y) + $(get "$1" bw) + $(get "$1" th) / 2)) x
	if [ "$2" = max ]; then
		x=$((right - $(get "$1" th) / 2))
	else
		x=$((right - $(get "$1" th) - $(get "$1" th) / 2))
	fi
	settle
	do_input move "$x" "$y"
	do_input click left
}
click_title() { # app
	refresh
	settle
	do_input move $(($(get "$1" x) + $(frame_w "$1") / 2)) $(($(get "$1" y) + $(get "$1" bw) + 4))
	do_input click left
}
window_menu_is() { # description app expected
	do_input key alt+space
	check "$1" "[ \"\$(global menu sensitive)\" = $3 ]"
	do_input key Escape
}
root_menu_is() { # description expected
	settle
	do_input move 1279 719
	do_input click right
	check "$1" "[ \"\$(global menu sensitive)\" = $2 ]"
	do_input key Escape
}

# With the default width on 1280x720: gaps of 8, main 690 wide on the right, stack
# 566 on the left.
MAIN_X=582 MAIN_W=690 STACK_W=566 FULL_H=704

# ---- the layout ------------------------------------------------------------------

begin
x11 a pos=40,40 size=300x200
x11 b pos=200,200 size=300x200
x11 c pos=400,350 size=300x200
root_menu_is "the root menu offers tiling" NUDTRQ

do_input key super+t
check "Super+T switches it on" "[ \"\$(global tiling on)\" = 1 ]"
cell_is "the newest window is the main one, on the right" c "$MAIN_X" 8 "$MAIN_W" "$FULL_H"
cell_is "the others stack on the left" b 8 8 "$STACK_W" 348
cell_is "sharing the height" a 8 364 "$STACK_W" 348
pixel_is "the desktop stays in reach in the gaps" 4 360 5b7792
window_menu_is "the main window can't move, resize or maximize" c nLC
root_menu_is "the root menu offers the way back" NUDtRQ

# Nothing moves a tiled window.
settle
do_input move $((MAIN_X + 300)) $((8 + B + T / 2))
do_input down left
do_input move $((MAIN_X + 100)) 200
do_input up left
do_input key alt+F7
do_input key alt+F8
check "Alt+F7 and Alt+F8 do nothing" "[ \"\$(global cursor mode)\" = 0 ]"
do_input hold alt
do_input move $((MAIN_X + 300)) 300
do_input down left
do_input move $((MAIN_X + 200)) 200
do_input up left
do_input release alt
do_input move $((MAIN_X + 2)) 360
do_input down left
do_input move $((MAIN_X - 100)) 360
do_input up left
do_input key super+Left
do_input key super+shift+Left
do_input key alt+F10
cell_is "and nothing moves it: not drag, border, Alt, Super or Alt+F10" c "$MAIN_X" 8 "$MAIN_W" "$FULL_H"
check "no grab is left" "[ \"\$(global cursor mode)\" = 0 ] && [ \"\$(global outline shown)\" = 0 ]"

# ---- the buttons -----------------------------------------------------------------

button a min
SHADED=$((2 * B + T))
check "minimize on a stack window shades it" "[ \"\$(get a shaded)\" = 1 ] && [ \"\$(get a min)\" = 0 ]"
cell_is "it keeps its title bar" a 8 $((8 + 696 - SHADED + 8)) "$STACK_W" "$SHADED"
cell_is "the other one takes the room" b 8 8 "$STACK_W" $((696 - SHADED))
pixel_is "what shows below it is the desktop" 300 $((8 + 696 - SHADED + 8 + SHADED + 2)) 5b7792
check "the client keeps its height for later" "[ \"\$(get a h)\" -gt 100 ]"
window_menu_is "with a window shaded, the main window's menu is the same" c nLC
click_title a
check "a click on it unrolls it" "[ \"\$(get a shaded)\" = 0 ] && [ \"\$(get a active)\" = 1 ]"
cell_is "back to sharing" a 8 364 "$STACK_W" 348
window_menu_is "a stack window can shade and maximize" a nxLC

button a min
check "shaded again, and it lost the focus" "[ \"\$(get a shaded)\" = 1 ] && [ \"\$(get a active)\" = 0 ]"
button a min
check "the click on its minimize button only unrolls it" "[ \"\$(get a shaded)\" = 0 ]"
button a min
for _ in 1 2 3; do
	refresh
	[ "$(get a active)" = 1 ] && break
	do_input key alt+Tab
done
check "Alt+Tab reaches it and unrolls it" "[ \"\$(get a shaded)\" = 0 ] && [ \"\$(get a active)\" = 1 ]"
button a min
button b min
check "the other stack window is shaded too" "[ \"\$(get b shaded)\" = 1 ]"
do_input key alt+Tab
button b max
cell_is "maximize on a stack window makes it the main one" b "$MAIN_X" 8 "$MAIN_W" "$FULL_H"
check "unrolled" "[ \"\$(get b shaded)\" = 0 ] && [ \"\$(get b active)\" = 1 ]"
check "the old main window goes to its place" "[ \"\$(get c tile_main)\" = 0 ] && [ \"\$(get c x)\" = 8 ]"
button b max
cell_is "maximize on the main window does nothing" b "$MAIN_X" 8 "$MAIN_W" "$FULL_H"
click_title a
do_input key alt+F5
check "Alt+F5 unrolls" "[ \"\$(get a shaded)\" = 0 ]"
button b min
cell_is "minimize on the main window sends it into the stack" c "$MAIN_X" 8 "$MAIN_W" "$FULL_H"
check "the first stack window takes its place" "[ \"\$(get b tile_main)\" = 0 ] && [ \"\$(get b x)\" = 8 ]"

# ---- windows coming and going -------------------------------------------------------

x11 d pos=600,100 size=300x200
cell_is "a new window becomes the main one" d "$MAIN_X" 8 "$MAIN_W" "$FULL_H"
check "with the focus" "[ \"\$(get d active)\" = 1 ]"
stop d
cell_is "when the main window goes, the next one takes over" c "$MAIN_X" 8 "$MAIN_W" "$FULL_H"
check "and the focus goes somewhere" "[ \"\$(global focus any)\" = 1 ]"
stop a
cell_is "the stack closes the gap" b 8 8 "$STACK_W" "$FULL_H"
stop b
cell_is "alone, a window gets all of it" c 8 8 1264 "$FULL_H"
window_menu_is "alone, there is nothing to go to" c LC
button c min
cell_is "and minimize does nothing" c 8 8 1264 "$FULL_H"
do_input key super+t
floats_at "switched off, it goes back to where it floated" c 400 350 300 200
check "Super+T off" "[ \"\$(global tiling on)\" = 0 ]"
window_menu_is "and may move and resize again" c MSnxLC
do_input key super+t
do_input key alt+F11
check "from the layout to fullscreen" "[ \"\$(get c full)\" = 1 ] && [ \"\$(get c w)\" = 1280 ] && [ \"\$(get c tiled)\" = 0 ]"
do_input key alt+F11
cell_is "and back into the layout" c 8 8 1264 "$FULL_H"
do_input key super+t
floats_at "its floating place survives even that" c 400 350 300 200

# A maximized or snapped window comes back like that.
do_input key alt+F10
do_input key super+t
cell_is "a maximized window tiles" c 8 8 1264 "$FULL_H"
do_input key super+t
check "and is maximized again afterwards" "[ \"\$(get c max)\" = 1 ] && [ \"\$(get c w)\" = $((1280 - 2 * B)) ]"
do_input key alt+F10
floats_at "and knows its place" c 400 350 300 200
do_input key super+Left
do_input key super+t
cell_is "a snapped window tiles" c 8 8 1264 "$FULL_H"
do_input key super+t
check "and is snapped again afterwards" "[ \"\$(get c snap)\" != 0 ] && [ \"\$(get c x)\" = 0 ] && [ \"\$(frame_w c)\" = 640 ]"
do_input key alt+F5
floats_at "and knows its place" c 400 350 300 200

# ---- windows that stay out -----------------------------------------------------------

do_input key super+t
x11 dlgmain size=300x200 dialog=dlg
check "a dialog floats" "[ \"\$(get dlg tiled)\" = 0 ] && [ \"\$(get dlgmain tiled)\" = 1 ]"
check "over its window" "[ \"\$(stack_pos dlg)\" = 0 ]"
cell_is "its window is the main one" dlgmain "$MAIN_X" 8 "$MAIN_W" "$FULL_H"
do_input key alt+F4
check "the dialog is gone" "[ -z \"\$(get dlg x)\" ]"
stop dlgmain

x11 fixed pos=800,300 size=200x100 min=200x100 max=200x100
floats_at "a fixed size floats" fixed 800 300 200 100
cell_is "and the layout ignores it" c 8 8 1264 "$FULL_H"
stop fixed

x11 toowide pos=100,100 size=900x200 min=900x100
check "too wide for the stack, it floats" "[ \"\$(get toowide tiled)\" = 0 ]"
cell_is "and the other keeps the room" c 8 8 1264 "$FULL_H"
stop toowide
x11 splashy type=splash
check "a splash screen floats" "[ \"\$(get splashy tiled)\" = 0 ]"
stop splashy

# Panels: the layout stays inside the usable area.
x11 panel type=dock pos=0,0 size=1280x40 strut=0,0,40,0
cell_is "below a panel" c 8 48 1264 664
stop panel
cell_is "and back when it goes" c 8 8 1264 "$FULL_H"
do_input key super+t

# ---- limits ---------------------------------------------------------------------------

do_input key super+t
x11 stepped size=300x200 inc=30x40
refresh
check "resize steps: the main window keeps to the right edge" \
	"[ \$((\$(get stepped x) + \$(frame_w stepped))) = $((MAIN_X + MAIN_W)) ] && [ \$(((\$(get stepped w)) % 30)) = 0 ]"
stop stepped

# Too many for the column: lower ones roll up, then the oldest become icons.
x11 w1 size=200x200 min=100x280
x11 w2 size=200x200 min=100x280
x11 w3 size=200x200 min=100x280
x11 w4 size=200x200 min=100x280
check "lower windows roll up to make room" \
	"[ \"\$(get w1 shaded)\" = 1 ] || [ \"\$(get c shaded)\" = 1 ]"
check "the main window never does" "[ \"\$(get w4 shaded)\" = 0 ] && [ \"\$(get w4 tile_main)\" = 1 ]"
for n in 5 6 7 8 9 10 11 12 13 14 15 16 17; do
	x11 "w$n" size=200x200 min=100x280
done
check "when even that isn't enough, the oldest becomes an icon" "[ \"\$(get c min)\" = 1 ]"
check "the layout ends above the icons" \
	"[ \$((\$(get w17 y) + \$(frame_h w17))) -le \$(get c icon_y) ]"
ICON_X=$(get c icon_x) ICON_Y=$(get c icon_y)
for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17; do
	stop "w$n"
done
settle
do_input move $((ICON_X + 30)) $((ICON_Y + 30))
do_input click left
do_input click left
cell_is "back from the icon, it joins again" c 8 8 1264 "$FULL_H"
do_input key super+t

# ---- raising and lowering the layout --------------------------------------------------

do_input key super+t
stop c
echo 'Mwm*DoNotTile: floaty' >"$RC"
kill -HUP "$pid"
sleep 0.1
x11 tiled1 size=300x200 color=0000ff
x11 floaty pos=600,100 size=300x200 color=00ff00
check "DoNotTile floats" "[ \"\$(get floaty tiled)\" = 0 ] && [ \"\$(get tiled1 tiled)\" = 1 ]"
root_menu_is "a floating window on top: Raise Tiled Windows" NUDtaRQ
pixel_is "it lies above the layout" 700 200 00ff00
settle
do_input move 1279 719
do_input click right
do_input key a
check "the layout comes to the front, with the focus" \
	"[ \"\$(stack_pos tiled1)\" = 0 ] && [ \"\$(get tiled1 active)\" = 1 ]"
pixel_is "above the floating window" 700 200 0000ff
root_menu_is "now: Lower Tiled Windows" NUDtLRQ
settle
do_input move 1279 719
do_input click right
do_input key l
check "and goes below again" "[ \"\$(stack_pos floaty)\" = 0 ] && [ \"\$(get floaty active)\" = 1 ]"
pixel_is "behind the floating window" 700 200 00ff00
stop floaty
stop tiled1

# ---- Wayland clients -------------------------------------------------------------------

wayland wa size=300x200
wayland wb size=300x200
cell_is "Wayland: the main window" wb "$MAIN_X" 8 "$MAIN_W" "$FULL_H"
cell_is "Wayland: the stack" wa 8 8 "$STACK_W" "$FULL_H"
button wa min
cell_is "Wayland: shading keeps the title bar" wa 8 8 "$STACK_W" "$SHADED"
pixel_is "Wayland: nothing of the client shows" 300 $((8 + SHADED + 3)) 5b7792
click_title wa
check "Wayland: a click unrolls it" "[ \"\$(get wa shaded)\" = 0 ]"
cell_is "Wayland: back to its cell" wa 8 8 "$STACK_W" "$FULL_H"
button wa max
cell_is "Wayland: promoted" wa "$MAIN_X" 8 "$MAIN_W" "$FULL_H"
wayland wlim size=300x200 max=400x300
check "Wayland: a maximum size stays at the right edge" \
	"[ \"\$(get wlim w)\" = 400 ] && [ \$((\$(get wlim x) + \$(frame_w wlim))) = $((MAIN_X + MAIN_W)) ]"
stop wlim
stop wa
stop wb
do_input key super+t
finish

# ---- the settings -----------------------------------------------------------------------

cat >"$RC" <<'EOF'
Mwm*tiling: true
Mwm*tileMainWidth: 70%
EOF
begin
x11 a size=300x200
x11 b size=300x200
check "Mwm*tiling: true starts with tiling on" "[ \"\$(global tiling on)\" = 1 ]"
# inner 1256: 70 % is 879, leaving 377
cell_is "tileMainWidth: main" b $((2 * 8 + 377)) 8 879 "$FULL_H"
cell_is "tileMainWidth: stack" a 8 8 377 "$FULL_H"

echo 'Mwm*tileMainWidth: 40' >"$RC"
kill -HUP "$pid"
# 40 % is 502, leaving 754. No Mwm*tiling any more, but a reload keeps the choice.
cell_is "a reload applies the new width and keeps tiling on" b $((2 * 8 + 754)) 8 502 "$FULL_H"

echo 'Mwm*tileMainWidth: 95' >"$RC"
kill -HUP "$pid"
cell_is "out of range: the default" b "$MAIN_X" 8 "$MAIN_W" "$FULL_H"
grep -q 'ignoring tileMainWidth value 95' "$LOG" || fail "the bad width is not reported"
echo "ok: the bad width is reported"

# The filters, by class or instance and ignoring case, the later line winning.
echo 'Mwm*DoNotTile: Nothing, A' >"$RC"
kill -HUP "$pid"
check "DoNotTile: a reload takes it out" "[ \"\$(get a tiled)\" = 0 ] && [ \"\$(get b tiled)\" = 1 ]"
printf 'Mwm*DoNotTile: a\nMwm*TileOnly: a\n' >"$RC"
kill -HUP "$pid"
check "TileOnly, the later one wins" "[ \"\$(get a tiled)\" = 1 ] && [ \"\$(get b tiled)\" = 0 ]"
printf 'Mwm*TileOnly: a\nMwm*DoNotTile:\n' >"$RC"
kill -HUP "$pid"
check "an empty value switches the filter off" "[ \"\$(get a tiled)\" = 1 ] && [ \"\$(get b tiled)\" = 1 ]"
stop a
stop b
finish

printf 'Mwm*tiling: true\nMwm*tileMainWidth: 70\n' >"$RC"
export MWC_TILING=0 MWC_TILE_MAIN_WIDTH=30
begin
check "MWC_TILING=0 beats the file" "[ \"\$(global tiling on)\" = 0 ]"
do_input key super+t
x11 a size=300x200
x11 b size=300x200
# 30 % of 1256 is 376, leaving 880
cell_is "MWC_TILE_MAIN_WIDTH beats the file" b $((2 * 8 + 880)) 8 376 "$FULL_H"
stop a
stop b
finish
unset MWC_TILING MWC_TILE_MAIN_WIDTH

echo "PASS: tile"
