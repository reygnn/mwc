#!/bin/bash
# Headless test of what an RDP server such as wayrdp does to mwc, with mwc's own
# tools in its place:
# - its virtual keyboard and pointer go when it ends or crashes, also with a key
#   or a button down and in the middle of a move;
# - each connection captures the screen afresh, on a still desktop, after the
#   output changed its size or was switched off and on;
# - each connection gives the output the size the client asks for, and the
#   windows in their states have to follow.
# Output goes to build/rdp.log. Exits non-zero on failure.
LOG=build/rdp.log
. "$(dirname "$0")/lib.sh"

require_tools foot Xwayland
for tool in build/vinput build/xdgc build/outputc build/capturec build/powerc \
		build/pngtool; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
printf 'Mwm*outputManagement: true\nMwm*rootBackground: #204060\n' >"$RC"

wayland() { # name key=value...: an xdgc window
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$name" "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "PID_$name=$!"
	check "$name is mapped" "[ -n \"\$(get $name x)\" ]"
}
outputc() { WAYLAND_DISPLAY=wayland-0 build/outputc "$@"; }
capturec() { WAYLAND_DISPLAY=wayland-0 build/capturec "$@"; }
power() { WAYLAND_DISPLAY=wayland-0 build/powerc "$1" || fail "powerc $1"; }
png_size() { # file: prints WxH from the PNG header
	local b
	read -r -a b < <(od -An -tu1 -j16 -N8 "$1")
	echo "$(((b[0] << 24) + (b[1] << 16) + (b[2] << 8) + b[3]))x$(((b[4] << 24) + \
		(b[5] << 16) + (b[6] << 8) + b[7]))"
}
now_ms() { echo $(($(date +%s%N) / 1000000)); }

start_mwc 'exec sleep 300'
start_vinput
refresh

# ---- 1. the virtual devices go in the middle of a press ---------------------------

# A key that is down when the keyboard goes must not stay down for the client: a
# Wayland client repeats a held key itself until it hears of the release.
foot -a typer -W 40x4 sh -c "stty raw -echo; exec cat > '$SMOKE_DIR/typed'" \
	>>"$LOG" 2>&1 &
PID_typer=$!
check "the typing window is mapped" "[ \"\$(get typer kbfocus)\" = 1 ]"
do_input key a
wait_for "a key reaches it" grep -q a "$SMOKE_DIR/typed"
count_b() { tr -cd b <"$SMOKE_DIR/typed" | wc -c; }
do_input press b
sleep 1.2 # past the repeat delay (600 ms): the client repeats it
n1=$(count_b)
[ "$n1" -gt 3 ] || fail "a held key doesn't repeat, the test can't tell: $n1 times b"
echo "ok: a held key repeats ($n1 times b)"
stop_vinput
sleep 1.2
n2=$(count_b)
[ $((n2 - n1)) -le 2 ] || fail "a key held when the keyboard went goes on repeating: $n1, then $n2 times b"
echo "ok: a key held when the keyboard goes is released (then $n2 times b)"
start_vinput

# A move with Alt and the left button, and the pointer goes before the release.
wayland mover size=300x200
B=$(global theme border)
T=$(global theme title)
mx=$(($(get mover x) + 150)) my=$(($(get mover y) + 100))
x0=$(get mover x) y0=$(get mover y)
do_input hold alt
do_input move $mx $my
do_input down left
do_input move $((mx + 120)) $((my + 60))
check "an Alt+drag moves it" "[ \"\$(global cursor mode)\" = 1 ]"
stop_vinput
start_vinput
do_input move 900 500
do_input move 950 520
check "the move ends when the pointer that held it goes" \
	"[ \"\$(global cursor mode)\" = 0 ] && [ \"\$(global outline shown)\" = 0 ]"
x1=$(get mover x) y1=$(get mover y)
do_input move 1000 600
sleep 0.2
refresh
[ "$(get mover x)" = "$x1" ] && [ "$(get mover y)" = "$y1" ] ||
	fail "the window still follows the new pointer: from $x1,$y1 to $(get mover x),$(get mover y)"
echo "ok: and the window doesn't follow the next pointer (moved from $x0,$y0 to $x1,$y1)"

# Alt was down when the keyboard went: a plain drag with the next one is no move.
mx=$(($(get mover x) + 150)) my=$(($(get mover y) + 100))
do_input move $mx $my
do_input down left
do_input move $((mx + 40)) $((my + 20))
sleep 0.1
refresh
[ "$(global cursor mode)" = 0 ] || fail "a plain drag moves the window: Alt still counts"
do_input up left
echo "ok: Alt doesn't outlive its keyboard"

# A button a client saw go down, and the pointer goes: the next one must reach
# other windows.
wayland other size=300x200
check "the other window is active" "[ \"\$(get other active)\" = 1 ]"
# Near its bottom right corner: the other window opened over the rest of it.
corner() { # app: a point inside the client area, 10 pixels from its corner
	echo $(($(get "$1" x) + $(get "$1" bw) + $(get "$1" w) - 10)) \
		$(($(get "$1" y) + $(get "$1" bw) + $(get "$1" th) + $(get "$1" h) - 10))
}
topleft() { # app: the same, from the top left corner
	echo $(($(get "$1" x) + $(get "$1" bw) + 10)) \
		$(($(get "$1" y) + $(get "$1" bw) + $(get "$1" th) + 10))
}
do_input move $(corner mover)
do_input down left
check "the press brings the window under it to the front" "[ \"\$(get mover active)\" = 1 ]"
stop_vinput
start_vinput
do_input move $(topleft other)
do_input click left
check "a click with the next pointer focuses another window" \
	"[ \"\$(get other active)\" = 1 ] && [ \"\$(get other kbfocus)\" = 1 ]"
do_input move $(corner mover)
do_input click left
check "and back" "[ \"\$(get mover active)\" = 1 ] && [ \"\$(get mover kbfocus)\" = 1 ]"
kill "$PID_mover" "$PID_other" 2>/dev/null
check "the windows are gone" "[ -z \"\$(get mover x)\" ] && [ -z \"\$(get other x)\" ]"
kill "$PID_typer" 2>/dev/null
check "the typing window is gone" "[ -z \"\$(get typer x)\" ]"

# ---- 2. a fresh capture on a still desktop -------------------------------------------

# wayrdp waits up to 1.5 s for the first frame of a connection; nothing moves.
fresh_frame() { # description WxH
	local t0 t1 size
	sleep 0.5
	t0=$(now_ms)
	capturec frame HEADLESS-1 "$SMOKE_DIR/fresh.png" || fail "$1: no frame"
	t1=$(now_ms)
	size=$(png_size "$SMOKE_DIR/fresh.png")
	[ "$size" = "$2" ] || fail "$1: the frame is $size, expected $2"
	[ $((t1 - t0)) -lt 1500 ] || fail "$1: the frame took $((t1 - t0)) ms"
	[ "$(build/pngtool pixel "$SMOKE_DIR/fresh.png" 10 10)" = 204060 ] ||
		fail "$1: the frame doesn't show the desktop"
	echo "ok: $1 ($((t1 - t0)) ms)"
}
do_input move 5 5
fresh_frame "a capture on a still desktop gets its frame" 1280x720
outputc apply HEADLESS-1 1024x768 || fail "setting 1024x768"
fresh_frame "and after the output changed its size" 1024x768
power off
check "the output is off" "[ \"\$(global output on)\" = 0 ]"
power on
check "and on again" "[ \"\$(global output on)\" = 1 ]"
fresh_frame "and after it was off" 1024x768
outputc apply HEADLESS-1 1280x720 || fail "back to 1280x720"

# ---- 3. the windows follow each connection's size -----------------------------------

# Whatever state a window is in, it stays on the output and keeps its state.
wayland snapped size=300x200
do_input key super+Left
check "a window snapped to the left half" "[ \"\$(get snapped snap)\" != 0 ]"
wayland iconic size=300x200
do_input key alt+F9
check "a window minimized" "[ \"\$(get iconic min)\" = 1 ]"
wayland maxed size=300x200 pre=max
check "a window maximized" "[ \"\$(get maxed max)\" = 1 ]"
wayland full size=300x200 pre=full
check "a window fullscreen" "[ \"\$(get full full)\" = 1 ]"

fw() { echo $(($(get "$1" w) + 2 * $(get "$1" bw))); }
fh() { echo $(($(get "$1" h) + 2 * $(get "$1" bw) + $(get "$1" th))); }
near() { [ $(($1 - $2)) -le 1 ] && [ $(($2 - $1)) -le 1 ]; }
states_fit() { # W H
	local w=$1 h=$2
	check "${w}x$h: fullscreen covers the output" \
		"[ \"\$(get full full)\" = 1 ] && [ \"\$(get full x)\" = 0 ] && \
		[ \"\$(get full y)\" = 0 ] && [ \"\$(get full w)\" = $w ] && \
		[ \"\$(get full h)\" = $h ]"
	check "${w}x$h: maximized fills the usable area" \
		"[ \"\$(get maxed max)\" = 1 ] && [ \"\$(get maxed x)\" = \"\$(global output ux)\" ] && \
		[ \"\$(get maxed y)\" = \"\$(global output uy)\" ] && \
		[ \"\$(fw maxed)\" = \"\$(global output uw)\" ] && \
		[ \"\$(fh maxed)\" = \"\$(global output uh)\" ]"
	# A half keeps MWC_GAP (8) to the edges and to the other half.
	check "${w}x$h: snapped keeps the left half" \
		"[ \"\$(get snapped snap)\" != 0 ] && [ \"\$(get snapped x)\" = \$((\$(global output ux) + 8)) ] && \
		near \"\$(fw snapped)\" \$(((\$(global output uw) - 24) / 2)) && \
		[ \"\$(fh snapped)\" = \$((\$(global output uh) - 16)) ]"
	check "${w}x$h: the icon stays on the output" \
		"[ \"\$(get iconic min)\" = 1 ] && [ \"\$(get iconic icon_x)\" -ge 0 ] && \
		[ \"\$(get iconic icon_y)\" -ge 0 ] && [ \"\$(get iconic icon_x)\" -lt $w ] && \
		[ \"\$(get iconic icon_y)\" -lt $h ]"
}
states_fit 1280 720
for size in 1024x768 1600x900 900x700 1280x720; do
	outputc apply HEADLESS-1 "$size" || fail "setting $size"
	output_w=${size%x*} output_h=${size#*x}
	states_fit "$output_w" "$output_h"
done

# Tiled windows share whatever width there is.
kill "$PID_snapped" "$PID_iconic" "$PID_maxed" "$PID_full" 2>/dev/null
check "the windows are gone" "[ -z \"\$(get maxed x)\" ] && [ -z \"\$(get full x)\" ]"
wayland t1 size=300x200
wayland t2 size=300x200
wayland t3 size=300x200
do_input key super+t
check "tiling is on" "[ \"\$(global tiling on)\" = 1 ] && [ \"\$(get t1 tiled)\" = 1 ]"
tiles_fit() { # W H
	local w=$1 h=$2 a
	for a in t1 t2 t3; do
		check "${w}x$h: $a is tiled within the output" \
			"[ \"\$(get $a tiled)\" = 1 ] && [ \"\$(get $a x)\" -ge 0 ] && \
			[ \"\$(get $a y)\" -ge 0 ] && [ \$((\$(get $a x) + \$(fw $a))) -le $w ] && \
			[ \$((\$(get $a y) + \$(fh $a))) -le $h ]"
	done
	# With the same gap on the right as on the left.
	check "${w}x$h: and they fill its width" \
		"r=0; l=$w; for a in t1 t2 t3; do e=\$((\$(get \$a x) + \$(fw \$a))); \
		[ \$e -gt \$r ] && r=\$e; [ \$(get \$a x) -lt \$l ] && l=\$(get \$a x); done; \
		near \$r \$((2 * \$(global output ux) + \$(global output uw) - l))"
}
tiles_fit 1280 720
for size in 1600x900 900x700 1280x720; do
	outputc apply HEADLESS-1 "$size" || fail "setting $size"
	tiles_fit "${size%x*}" "${size#*x}"
done

kill -0 "$pid" || fail "mwc died"
stop_vinput
stop_mwc
echo "PASS: rdp"
