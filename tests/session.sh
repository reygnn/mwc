#!/bin/bash
# Headless test of session settings: monitors and keyboard from mwmrc, the environment
# programs get, and the properties X11 programs look for.
# Output goes to build/session.log. Exits non-zero on failure.
LOG=build/session.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland
for tool in build/vinput build/pngtool build/xclient; do
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

# Two outputs, the other way round.
export WLR_HEADLESS_OUTPUTS=2
cat >"$RC" <<'RC'
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
echo 'Mwm*keyboardLayout: no-such-layout' >"$RC"
begin
log_has "a layout that doesn't exist falls back" 'falling back to the us layout'
do_input key a
kill -0 "$pid" || fail "mwc died from a bad layout"
finish
rm "$RC"

echo "PASS: session"
