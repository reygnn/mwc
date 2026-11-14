#!/bin/bash
# Headless test of session settings: monitors and keyboard from mwmrc, the environment
# programs get, and the properties X11 programs look for.
# Output goes to build/session.log. Exits non-zero on failure.
LOG=build/session.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland grim
for tool in build/vinput build/pngtool build/xclient build/powerc build/outputc build/capturec; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
unset XDG_SESSION_TYPE XKB_DEFAULT_LAYOUT # the session around the test has its own
RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"

begin() { # [vinput width height]
	rm -f "$SMOKE_DIR/display"
	start_mwc 'env > "$SMOKE_DIR/env"; echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
	wait_for "the startup command ran" test -s "$SMOKE_DIR/display"
	X_DISPLAY=$(cat "$SMOKE_DIR/display")
	start_vinput "$@"
	refresh
	B=$(global theme border)
	T=$(global theme title)
}
finish() {
	stop_vinput
	stop_mwc
}
output_is() { # description name x y w h [scale]
	check "$1" "[ \"\$(global output_$2 x)\" = $3 ] && [ \"\$(global output_$2 y)\" = $4 ] && \
		[ \"\$(global output_$2 w)\" = $5 ] && [ \"\$(global output_$2 h)\" = $6 ] && \
		[ \"\$(global output_$2 scale)\" = ${7:-100} ]"
}
log_has() {
	grep -q -- "$2" "$LOG" || fail "$1: no \"$2\" in the log"
	echo "ok: $1"
}

# ---- what programs find ----------------------------------------------------------

begin
log_has "the output's name is logged, for mwmrc" "new output HEADLESS-1"
grep -q '^_JAVA_AWT_WM_NONREPARENTING=1$' "$SMOKE_DIR/env" || fail "Java isn't told"
echo "ok: Java programs are told not to expect reparenting"
grep -q '^XDG_SESSION_TYPE=wayland$' "$SMOKE_DIR/env" && fail "nested, the session type isn't ours to set"
echo "ok: outside a real session the activation environment stays alone"

DISPLAY=$X_DISPLAY build/xclient class=rep report=1 pos=100,100 size=300x200 >"$SMOKE_DIR/rep.log" 2>&1 &
PID_rep=$!
check "the window is mapped" "[ -n \"\$(get rep x)\" ]"
wait_for "a Motif program finds a window manager" grep -q 'motif-wm running' "$SMOKE_DIR/rep.log"
wait_for "_NET_FRAME_EXTENTS tells the frame" grep -q "frame-extents $B $B $((B + T)) $B" "$SMOKE_DIR/rep.log"
do_input key alt+F11
wait_for "fullscreen: none" grep -q 'frame-extents 0 0 0 0' "$SMOKE_DIR/rep.log"
do_input key alt+F11
check "back" "[ \"\$(get rep full)\" = 0 ]"
: >"$SMOKE_DIR/rep.log"
echo 'Mwm*fontList: Sans Bold 24' >"$RC"
kill -HUP "$pid"
check "a larger title bar" "[ \"\$(global theme title)\" -gt 30 ]"
refresh
wait_for "the extents follow" grep -q "frame-extents $B $B $((B + $(global theme title))) $B" "$SMOKE_DIR/rep.log"
kill "$PID_rep"
wait "$PID_rep" 2>/dev/null
rm "$RC"
finish

export _JAVA_AWT_WM_NONREPARENTING=0
begin
grep -q '^_JAVA_AWT_WM_NONREPARENTING=0$' "$SMOKE_DIR/env" || fail "a value set by the user must stay"
echo "ok: a value set by the user stays"
finish
unset _JAVA_AWT_WM_NONREPARENTING

# ---- monitors -----------------------------------------------------------------------

cat >"$RC" <<'RC'
Mwm*HEADLESS-1*outputMode: 1024x600
RC
begin
output_is "outputMode" HEADLESS-1 0 0 1024 600

cat >"$RC" <<'RC'
Mwm*HEADLESS-1*outputMode: 800x500@30
Mwm*HEADLESS-1*outputScale: 2
RC
kill -HUP "$pid"
output_is "a reload: new mode and outputScale, the layout counts in scaled pixels" \
	HEADLESS-1 0 0 400 250 200

DISPLAY=$X_DISPLAY build/xclient class=follows pos=20,20 size=100x60 >/dev/null 2>&1 &
PID_follows=$!
check "a window" "[ -n \"\$(get follows x)\" ]"
do_input key alt+F10
check "maximized, it fills what there is" \
	"[ \"\$(get follows max)\" = 1 ] && [ \$((\$(get follows w) + 2 * B)) = 400 ]"
cat >"$RC" <<'RC'
Mwm*HEADLESS-1*outputMode: 1280x720
Mwm*HEADLESS-1*outputTransform: 90
RC
kill -HUP "$pid"
output_is "outputTransform: it stands on its side" HEADLESS-1 0 0 720 1280
check "and the maximized window follows" "[ \$((\$(get follows w) + 2 * B)) = 720 ]"
kill "$PID_follows"
wait "$PID_follows" 2>/dev/null

rm "$RC"
kill -HUP "$pid"
output_is "without the file: back to the defaults" HEADLESS-1 0 0 1280 720
finish

# Nonsense is reported and changes nothing.
cat >"$RC" <<'RC'
Mwm*HEADLESS-1*outputMode: banana
Mwm*HEADLESS-1*outputScale: -1
Mwm*HEADLESS-1*outputTransform: 45
Mwm*HEADLESS-1*outputPosition: here
RC
begin
output_is "nonsense changes nothing" HEADLESS-1 0 0 1280 720
log_has "a bad mode is reported" 'ignoring outputMode "banana"'
log_has "a bad scale is reported" 'ignoring outputScale "-1"'
log_has "a bad transform is reported" 'ignoring outputTransform "45"'
log_has "a bad position is reported" 'ignoring outputPosition "here"'
finish

# Values a plain range check lets through, at startup and on a reload.
for scale in nan 1e-9 1e-300 inf; do
	echo "Mwm*HEADLESS-1*outputScale: $scale" >"$RC"
	begin
	output_is "outputScale $scale at startup changes nothing" HEADLESS-1 0 0 1280 720
	log_has "and is reported" "ignoring outputScale \"$scale\""
	kill -HUP "$pid"
	output_is "nor on a reload" HEADLESS-1 0 0 1280 720
	finish
done
echo 'Mwm*HEADLESS-1*outputPosition: 2147483647,2147483647' >"$RC"
begin
output_is "a position nothing can add to changes nothing" HEADLESS-1 0 0 1280 720
log_has "and is reported" 'ignoring outputPosition "2147483647,2147483647"'
finish

# A monitor someone switched off stays off, and gets what it missed when it comes back.
power() { WAYLAND_DISPLAY=wayland-0 build/powerc "$1" || fail "powerc $1"; }
: >"$RC"
begin
power off
check "a tool switches the monitor off" "[ \"\$(global output on)\" = 0 ]"
reloads=$(grep -c 'reloading the configuration' "$LOG")
reloaded() { [ "$(grep -c 'reloading the configuration' "$LOG")" -gt "$reloads" ]; }
echo 'Mwm*HEADLESS-1*outputMode: 1024x600' >"$RC"
kill -HUP "$pid"
wait_for "the reload" reloaded
sleep 0.5
check "a reload leaves it off" "[ \"\$(global output on)\" = 0 ]"
power on
check "switched on again" "[ \"\$(global output on)\" = 1 ]"
output_is "with the mode the reload brought" HEADLESS-1 0 0 1024 600
echo 'Mwm*HEADLESS-1*outputMode: 800x500' >"$RC"
kill -HUP "$pid"
output_is "a reload still applies to a monitor that is on" HEADLESS-1 0 0 800 500
check "and leaves it on" "[ \"\$(global output on)\" = 1 ]"
finish

# Every mode change throws away the output's buffers. wlroots 0.19.2's shm allocator
# did it without telling the renderer, which wrote into them when mwc ended, and now
# and then glibc aborted (see src/shmalloc.c). Whether that hits glibc's bookkeeping
# depends on the heap's layout, so this runs mwc under valgrind, which sees every
# such write (its report lands in the log): without valgrind it only checks that mwc
# ends cleanly.
echo 'Mwm*outputManagement: true' >"$RC"
if command -v valgrind >/dev/null; then
	cat >"$SMOKE_DIR/mwc-valgrind" <<VG
#!/bin/sh
exec valgrind -q --error-exitcode=99 "$(readlink -f "$MWC")" "\$@"
VG
	chmod +x "$SMOKE_DIR/mwc-valgrind"
	mwc_plain=$MWC MWC=$SMOKE_DIR/mwc-valgrind
fi
begin
for mode in 1024x600 1600x900 800x500 1280x720 640x480 1920x1080 1024x768 1280x720; do
	echo "Mwm*HEADLESS-1*outputMode: $mode" >"$RC"
	kill -HUP "$pid"
	for ((i = 0; i < 200; i++)); do
		refresh
		[ "$(global output_HEADLESS-1 w)x$(global output_HEADLESS-1 h)" = "$mode" ] && break
		sleep 0.05
	done
	[ "$i" -lt 200 ] || fail "the mode change to $mode never came"
done
echo "ok: eight mode changes"
# The same through wlr-output-management, which commits through the backend.
for mode in 1600x900 800x600 1280x720; do
	WAYLAND_DISPLAY=wayland-0 build/outputc apply HEADLESS-1 "$mode" ||
		fail "a client setting $mode"
done
echo "ok: and three set by a client"
finish
if [ -n "${mwc_plain-}" ]; then
	MWC=$mwc_plain
	unset mwc_plain
	echo "ok: and valgrind finds nothing when mwc ends"
else
	echo "ok: and mwc ends cleanly (no valgrind: not checked for writes into freed memory)"
fi

# ---- outputs set by a client, as a VNC or RDP server sets them ----------------------

outputc() { WAYLAND_DISPLAY=wayland-0 build/outputc "$@"; }
# refused <outputc args>: mwc says no, not a timeout or a lost connection.
refused() {
	local before status
	before=$(grep -c 'refusing an output configuration' "$LOG")
	outputc "$@" 2>/dev/null
	status=$?
	[ "$status" = 1 ] && [ "$(grep -c 'refusing an output configuration' "$LOG")" -gt "$before" ]
}
reload() {
	local before
	before=$(grep -c 'reloading the configuration' "$LOG")
	kill -HUP "$pid"
	wait_for "the reload" eval '[ "$(grep -c "reloading the configuration" "$LOG")" -gt '"$before"' ]'
	sleep 0.3
}
printf 'Mwm*outputManagement: true\nMwm*HEADLESS-1*outputMode: 1024x600\n' >"$RC"
begin
outputc list | grep -q '^HEADLESS-1 on=1 w=1024 h=600 x=0 y=0 scale=100$' ||
	fail "a client isn't told the output's size: $(outputc list)"
echo "ok: a client is told the output's size"
outputc test HEADLESS-1 1600x900 || fail "testing a mode"
output_is "a test changes nothing" HEADLESS-1 0 0 1024 600
outputc apply HEADLESS-1 1600x900 || fail "applying a mode"
output_is "a client sets a mode" HEADLESS-1 0 0 1600 900
outputc list | grep -q '^HEADLESS-1 on=1 w=1600 h=900 ' || fail "clients aren't told: $(outputc list)"
echo "ok: and clients are told"
reload
output_is "a reload leaves it as the client set it" HEADLESS-1 0 0 1600 900
power off
check "switched off" "[ \"\$(global output on)\" = 0 ]"
check "off for the time being: on for clients, with its mode" \
	"outputc list | grep -q '^HEADLESS-1 on=1 w=1600 h=900 '"
power on
check "switched on again" "[ \"\$(global output on)\" = 1 ]"
output_is "and on again: as the client set it" HEADLESS-1 0 0 1600 900
power off
outputc apply HEADLESS-1 1280x800 || fail "a client setting a mode while it is dark"
sleep 0.3
check "it stays dark" "[ \"\$(global output on)\" = 0 ]"
check "clients are told what it comes back with" \
	"outputc list | grep -q '^HEADLESS-1 on=1 w=1280 h=800 '"
power on
output_is "and it comes back with it" HEADLESS-1 0 0 1280 800
echo 'Mwm*HEADLESS-1*outputMode: 800x500' >"$RC"
reload
output_is "a change in mwmrc wins" HEADLESS-1 0 0 800 500
check "and clients are told" "outputc list | grep -q '^HEADLESS-1 on=1 w=800 h=500 '"
reload
output_is "and stays" HEADLESS-1 0 0 800 500
outputc apply HEADLESS-1 1280x720 scale 2 || fail "applying a scale"
output_is "a client sets a scale" HEADLESS-1 0 0 640 360 200
outputc apply HEADLESS-1 1280x720 scale 1 || fail "the scale back"
refused apply HEADLESS-1 99999x720 || fail "a size the renderer can't allocate is taken"
refused apply HEADLESS-1 20000x100 || fail "a side over 16384 is taken"
refused apply HEADLESS-1 1280x720@2000 || fail "a refresh over 1000 Hz is taken"
refused apply HEADLESS-1 1280x720 scale 20 || fail "a scale over 10 is taken"
refused apply HEADLESS-1 1280x720 at 0,2000000 || fail "a y the layout overflows with is taken"
refused apply HEADLESS-1 1000x1 scale 2 || fail "an output less than a pixel high is taken"
refused apply HEADLESS-1 16384x16384 || fail "a size of 1 GiB a buffer is taken"
refused test HEADLESS-1 16384x16384 || fail "a test of 1 GiB a buffer passes"
refused apply HEADLESS-1 1280x720@0.001 || fail "a refresh of a frame every quarter hour is taken"
refused apply HEADLESS-1 1x1 scale 10 || fail "an output of less than a pixel is taken"
refused apply HEADLESS-1 1280x720 scale 0.01 || fail "a scale the layout overflows with is taken"
refused apply HEADLESS-1 1280x720 at 2000000,0 || fail "a position the layout overflows with is taken"
echo "ok: mwc refuses what it can't take, also when only asked to test it"
output_is "what mwc refuses changes nothing" HEADLESS-1 0 0 1280 720
outputc apply HEADLESS-1 off || fail "switching it off"
check "a client switches it off" "[ \"\$(global output on)\" = 0 ]"
reload
check "a reload leaves it off" "[ \"\$(global output on)\" = 0 ]"
power on
sleep 0.3
check "and a power on too: a client switched it off" "[ \"\$(global output on)\" = 0 ]"
outputc apply HEADLESS-1 on 1024x768 || fail "switching it on"
check "and on" "[ \"\$(global output on)\" = 1 ]"
output_is "with the mode it asked for" HEADLESS-1 0 0 1024 768
kill -0 "$pid" || fail "mwc died"
finish

# ---- capturing the screen, as a VNC or RDP server does -------------------------------

capturec() { WAYLAND_DISPLAY=wayland-0 build/capturec "$@"; }
captured_is() { # description file x y rrggbb
	local got
	got=$(build/pngtool pixel "$2" "$3" "$4")
	[ "$got" = "$5" ] || fail "$1: pixel $3,$4 is $got, expected $5"
	echo "ok: $1"
}
echo 'Mwm*rootBackground: #204060' >"$RC"
begin
WAYLAND_DISPLAY=wayland-0 build/outputc list 2>&1 | grep -q 'no output manager' ||
	fail "wlr-output-management is offered without Mwm*outputManagement"
echo "ok: wlr-output-management only with Mwm*outputManagement"
do_input move 400 300
pixel_is "the desktop under the pointer" 600 300 204060
capturec frame HEADLESS-1 "$SMOKE_DIR/frame.png" || fail "capturing a frame"
captured_is "ext-image-copy-capture brings the desktop" "$SMOKE_DIR/frame.png" 600 300 204060
captured_is "without the pointer" "$SMOKE_DIR/frame.png" 401 301 204060
# Before any capture with the pointer painted in: wlroots draws it in software after
# that one, and has no image of its own for it until the pointer's image changes.
capturec cursor HEADLESS-1 "$SMOKE_DIR/cursor.png" >"$SMOKE_DIR/cursor.txt" ||
	fail "capturing the pointer"
read -r _ x y hotspot w h opaque <"$SMOKE_DIR/cursor.txt"
x=${x#x=} y=${y#y=}
[ "$x" -ge 400 ] && [ "$x" -le 401 ] && [ "$y" -ge 300 ] && [ "$y" -le 301 ] || fail "the pointer's position: $(cat "$SMOKE_DIR/cursor.txt")"
echo "ok: the pointer's position comes with it"
[ "${opaque#opaque=}" -gt 0 ] 2>/dev/null || fail "the pointer's image is empty: $(cat "$SMOKE_DIR/cursor.txt")"
echo "ok: and its image ($w $h $hotspot)"
capturec frame HEADLESS-1 "$SMOKE_DIR/painted.png" cursors || fail "capturing with the pointer"
[ "$(build/pngtool count "$SMOKE_DIR/painted.png" 390 290 30 30 204060)" -lt 900 ] ||
	fail "the pointer isn't painted in when asked"
captured_is "painted in when asked, on the desktop" "$SMOKE_DIR/painted.png" 600 300 204060
finish
rm "$RC"

# Two outputs, the other way round.
export WLR_HEADLESS_OUTPUTS=2
cat >"$RC" <<'RC'
Mwm*outputManagement: true
Mwm*HEADLESS-1*outputPosition: 1280,0
Mwm*HEADLESS-2*outputPosition: 0,0
RC
begin 2560 720
output_is "outputPosition: the first output on the right" HEADLESS-1 1280 0 1280 720
output_is "the second on the left" HEADLESS-2 0 0 1280 720
cat >"$RC" <<'RC'
Mwm*HEADLESS-2*outputPosition: 1280,100
Mwm*HEADLESS-1*outputPosition: 0,0
RC
kill -HUP "$pid"
output_is "a reload swaps them" HEADLESS-1 0 0 1280 720
output_is "with an offset" HEADLESS-2 1280 100 1280 720
outputc apply HEADLESS-2 at 0,720 || fail "a client moving an output"
output_is "a client moves one" HEADLESS-2 0 720 1280 720
output_is "the other stays" HEADLESS-1 0 0 1280 720
reload
output_is "a reload leaves it where the client put it" HEADLESS-2 0 720 1280 720
outputc apply HEADLESS-2 off || fail "a client switching one of two off"
check "it is off" "outputc list | grep -q '^HEADLESS-2 on=0'"
# Unlike a dark one it leaves the layout: the pointer can't go where it was.
output_is "and leaves the layout" HEADLESS-2 0 0 0 0
do_input move 100 700
check "the pointer stays on the other" "[ \"\$(global cursor y)\" -lt 720 ]"
reload
output_is "a reload leaves it out" HEADLESS-2 0 0 0 0
outputc apply HEADLESS-2 on || fail "and on again"
check "on again in its place" "outputc list | grep -q '^HEADLESS-2 on=1 w=1280 h=720 x=0 y=720 '"
finish
# Placed by wlroots, without outputPosition. A client changing one of them, naming all
# as wlr-randr does or only that one, must leave the other where it is.
echo 'Mwm*outputManagement: true' >"$RC"
begin 2560 720
# The right one: naming the left one would leave the other where it is anyway.
right=$(outputc list | awk '{ x = $5; sub("x=", "", x)
	if (n == "" || x + 0 > max) { max = x + 0; n = $1 } } END { print n }')
left=$(outputc list | grep -v "^$right ")
[ -n "$right" ] && [ -n "$left" ] || fail "two outputs: $(outputc list)"
right_line=$(outputc list | grep "^$right ")
left_name=${left%% *}
# wlroots' master turns leaving outputs out into a protocol error (f9d2520f): this
# then has to expect outputc to fail.
outputc apply "$right" alone || fail "a client naming one output alone"
check "the other output stays where wlroots put it" "[ \"\$(outputc list | grep -v '^$right ')\" = \"$left\" ]"
outputc apply "$left_name" 640x480 || fail "a client changing the left one's mode"
check "the right one stays where it was" \
	"[ \"\$(outputc list | grep '^$right ')\" = \"$right_line\" ]"
outputc apply "$right" at 0,720 || fail "a client moving the right one"
check "it goes where the client says" "outputc list | grep -q '^$right on=1 w=1280 h=720 x=0 y=720 '"
check "and the left one stays" "outputc list | grep -q '^$left_name on=1 w=640 h=480 x=0 y=0 '"
finish
# A client switching off the newest monitor while the other isn't at 0,0, as a docking
# profile does: its windows go to the monitor that is left, not to where it was. Then it
# comes back elsewhere, and its picture comes from there.
printf 'Mwm*outputManagement: true\nMwm*rootBackground: #204060\nMwm*HEADLESS-1*outputPosition: 0,0\nMwm*HEADLESS-2*outputPosition: 1280,0\n' >"$RC"
begin 2560 720
DISPLAY=$X_DISPLAY build/xclient class=stranded pos=100,100 size=300x200 >/dev/null 2>&1 &
PID_stranded=$!
check "a window on HEADLESS-1" "[ -n \"\$(get stranded x)\" ] && [ \"\$(get stranded x)\" -lt 1280 ]"
outputc apply HEADLESS-1 off || fail "a client switching off the newest monitor"
check "the window goes to the monitor that is left" \
	"[ \"\$(get stranded x)\" -ge 1280 ] && [ \"\$(get stranded x)\" -lt 2560 ]"
outputc apply HEADLESS-1 on at 2560,0 || fail "on again, elsewhere"
check "it comes back where the client says" \
	"outputc list | grep -q '^HEADLESS-1 on=1 w=1280 h=720 x=2560 y=0 '"
DISPLAY=$X_DISPLAY build/xclient class=there pos=2700,100 size=300x200 >/dev/null 2>&1 &
PID_there=$!
check "a window where it is now" "[ \"\$(get there x)\" -ge 2560 ]"
sleep 0.3
capturec frame HEADLESS-1 "$SMOKE_DIR/back.png" || fail "capturing it"
[ "$(build/pngtool pixel "$SMOKE_DIR/back.png" 1000 600)" = 204060 ] ||
	fail "its desktop isn't where it is: $(build/pngtool pixel "$SMOKE_DIR/back.png" 1000 600)"
[ "$(build/pngtool pixel "$SMOKE_DIR/back.png" 290 200)" != 204060 ] ||
	fail "its picture isn't from where it is now: no window at 290,200"
echo "ok: and its picture comes from there"
kill "$PID_stranded" "$PID_there" 2>/dev/null
finish
unset WLR_HEADLESS_OUTPUTS

# ---- the keyboard -----------------------------------------------------------------------

# The headless backend has no keyboard of its own. A virtual one brings its keymap, but
# what mwc compiled for it first is in the log.
echo 'Mwm*keyboardLayout: de' >"$RC"
begin
log_has "keyboardLayout" 'keymap compiled, layout German'
finish
export XKB_DEFAULT_LAYOUT=fr
begin
log_has "the environment wins" 'keymap compiled, layout French'
finish
unset XKB_DEFAULT_LAYOUT

# Layout and variant belong together, wherever each comes from.
echo 'Mwm*keyboardVariant: dvorak' >"$RC"
begin
log_has "a variant alone goes with the default layout" 'keymap compiled, layout English (Dvorak)'
finish
echo 'Mwm*keyboardVariant: nodeadkeys' >"$RC"
export XKB_DEFAULT_LAYOUT=de
begin
log_has "a variant from the file with a layout from the environment" \
	'keymap compiled, layout German (no dead keys)'
finish
unset XKB_DEFAULT_LAYOUT
echo 'Mwm*keyboardLayout: de' >"$RC"
export XKB_DEFAULT_VARIANT=nodeadkeys
begin
log_has "a layout from the file with a variant from the environment" \
	'keymap compiled, layout German (no dead keys)'
finish
unset XKB_DEFAULT_VARIANT

# A new keymap resets the locks, so only a change brings one.
echo 'Mwm*keyboardLayout: de' >"$RC"
begin
echo 'Mwm*rootBackground: #102030' >>"$RC"
reload_mwc
grep -q 'keymap settings changed' "$LOG" && fail "a reload for a colour rebuilt the keymap"
echo "ok: a reload for a colour leaves the keymap alone"
echo 'Mwm*keyboardLayout: fr' >"$RC"
kill -HUP "$pid"
wait_for "a reload for the layout" grep -q 'keymap settings changed' "$LOG"
finish
echo 'Mwm*keyboardLayout: no-such-layout' >"$RC"
begin
wait_for "a layout that doesn't exist falls back" grep -q 'falling back to the us layout' "$LOG"
do_input key a
kill -0 "$pid" || fail "mwc died from a bad layout"
finish
rm "$RC"

echo "PASS: session"
