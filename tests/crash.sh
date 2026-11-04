#!/bin/bash
# Headless test of things that used to end the session: clients doing what they may
# but rarely do, and normal events no other test got to. A clean exit at the end is
# part of each check; run it against a sanitizer build too, since a use-after-free
# may go unnoticed in a normal one.
# Output goes to build/crash.log. Exits non-zero on failure.
LOG=build/crash.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland
for tool in build/vinput build/xclient build/xdgc build/badclient build/layerc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state

begin() {
	rm -f "$SMOKE_DIR/display"
	start_mwc 'grep SigIgn /proc/self/status > "$SMOKE_DIR/sigign"; echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
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
alive() { kill -0 "$pid" 2>/dev/null || fail "mwc died: $1"; echo "ok: mwc survives: $1"; }
x11() {
	local name=$1
	shift
	DISPLAY=$X_DISPLAY build/xclient class="$name" "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "PID_$name=$!"
}
wayland() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$name" "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "PID_$name=$!"
}
bad() { # name mode
	WAYLAND_DISPLAY=wayland-0 build/badclient "$2" >"$SMOKE_DIR/$1.events" 2>&1 &
	eval "PID_$1=$!"
}
stop() {
	local var="PID_$1"
	kill "${!var}" 2>/dev/null
	wait "${!var}" 2>/dev/null
}
done_said() { grep -q '^done' "$SMOKE_DIR/$1.events"; }

begin

# ---- states asked for before the first commit --------------------------------------

wayland premax pre=max size=300x200
check "maximized before the first commit: it maps maximized" \
	"[ \"\$(get premax max)\" = 1 ] && [ \"\$(get premax x)\" = 0 ] && [ \"\$(get premax y)\" = 0 ] && \
	[ \$((\$(get premax w) + 2 * B)) = 1280 ] && [ \$((\$(get premax h) + 2 * B + T)) = 720 ]"
do_input key alt+F10
check "un-maximized it takes its own size, on the screen" \
	"[ \"\$(get premax max)\" = 0 ] && [ \"\$(get premax w)\" = 300 ] && [ \"\$(get premax h)\" = 200 ] && \
	[ \"\$(get premax x)\" -gt 0 ] && [ \"\$(get premax y)\" -gt 0 ]"
stop premax
check "the window is gone" "[ -z \"\$(get premax x)\" ]"

wayland prefull pre=full size=300x200
check "fullscreen before the first commit: it maps fullscreen" \
	"[ \"\$(get prefull full)\" = 1 ] && [ \"\$(get prefull frame)\" = 0 ] && \
	[ \"\$(get prefull w)\" = 1280 ] && [ \"\$(get prefull h)\" = 720 ]"
do_input key alt+F11
check "leaving fullscreen: its own size, with a frame" \
	"[ \"\$(get prefull full)\" = 0 ] && [ \"\$(get prefull frame)\" = 1 ] && [ \"\$(get prefull w)\" = 300 ]"
stop prefull
check "the window is gone" "[ -z \"\$(get prefull x)\" ]"

# Starting over with a null buffer: nothing of the old state stays, only what it asks
# for now counts, and asking must not crash either. This client has no frame.
bad remap remap
check "map, unmap, ask to maximize, map again: maximized" \
	"[ \"\$(get bad max)\" = 1 ] && [ \"\$(get bad w)\" = 1280 ]"
grep -q 'configured 1280x720' "$SMOKE_DIR/remap.events" || fail "the new first configure has no size"
echo "ok: the new first configure has the size"
stop remap
check "the window is gone" "[ -z \"\$(get bad x)\" ]"

# ---- grabs and menus on windows that were never mapped --------------------------------

bad nomove move
wait_for "the client asked to move and left" done_said nomove
check "no grab starts" "[ \"\$(global cursor mode)\" = 0 ] && [ \"\$(global outline shown)\" = 0 ]"
do_input move 400 400
do_input move 410 410
alive "a move request from a toplevel that was never mapped"
stop nomove

bad nomenu menu
wait_for "the client asked for a menu and left" done_said nomenu
check "no menu opens" "[ \"\$(global menu open)\" = 0 ]"
do_input key l
alive "a menu request from a toplevel that was never mapped"
stop nomenu

x11 xnomove moveresize=1
wait_for "the X client asked to move and left" grep -q '^done' "$SMOKE_DIR/xnomove.events"
check "no grab starts for X11" "[ \"\$(global cursor mode)\" = 0 ]"
do_input move 420 420
do_input move 430 430
alive "_NET_WM_MOVERESIZE on an unmapped window"
stop xnomove

# ---- popups of toplevels that are gone ---------------------------------------------------

bad orphan popup
wait_for "the client opened a popup on a dead toplevel" done_said orphan
do_input move 440 440
alive "a popup whose parent toplevel is gone"
stop orphan

bad orphan2 nested
wait_for "the client opened a popup on a popup of a dead toplevel" done_said orphan2
do_input move 450 450
alive "a popup of a popup whose toplevel is gone"
stop orphan2

# ---- X windows changing their mind about override-redirect --------------------------------

x11 lateor override=late pos=100,100 size=120x80
check "an X window that turns override-redirect before map isn't managed" \
	"[ \"\$(global unmanaged count)\" = 1 ] && [ -z \"\$(get lateor x)\" ]"
do_input key alt+Tab
alive "a late override-redirect window"
stop lateor
check "and is gone again" "[ \"\$(global unmanaged count)\" = 0 ]"

x11 latemgd override=clear pos=100,100 size=120x80
check "an X window that stops being override-redirect is managed" \
	"[ -n \"\$(get latemgd x)\" ] && [ \"\$(get latemgd frame)\" = 1 ] && [ \"\$(global unmanaged count)\" = 0 ]"
stop latemgd
check "and is gone again" "[ -z \"\$(get latemgd x)\" ]"

x11 mappedor override=mapped pos=100,100 size=120x80
check "a mapped X window that turns override-redirect stops being managed" \
	"[ -z \"\$(get mappedor x)\" ] && [ \"\$(global unmanaged count)\" = 1 ]"
stop mappedor
check "and is gone again" "[ \"\$(global unmanaged count)\" = 0 ]"

# ---- a paste that goes wrong ----------------------------------------------------------------

# SIGPIPE stays ignored in mwc, but programs it starts must not inherit that, or
# pipelines in them would change behaviour. Bit 13 is SIGPIPE.
wait_for "the startup command's signals are known" test -s "$SMOKE_DIR/sigign"
ign=$(awk '{ print $2 }' "$SMOKE_DIR/sigign")
[ $(((16#$ign >> 12) & 1)) = 0 ] || fail "started programs ignore SIGPIPE"
echo "ok: started programs don't ignore SIGPIPE"

bad paster paste-close
check "the pasting client is mapped" "[ -n \"\$(get bad x)\" ]"
x11 owner clipboard=1 pos=700,100
check "the clipboard owner is mapped" "[ -n \"\$(get owner x)\" ]"
wait_for "the X client owns the clipboard" grep -q '^owner' "$SMOKE_DIR/owner.events"
do_input key alt+Tab
check "the Wayland client has the focus" "[ \"\$(get bad active)\" = 1 ]"
wait_for "the client took the selection and closed its end" done_said paster
sleep 0.5
alive "a client closing its end of a paste"
stop owner
stop paster
check "both are gone" "[ -z \"\$(get bad x)\" ] && [ -z \"\$(get owner x)\" ]"

# ---- Xwayland going away by itself ----------------------------------------------------------

# It is started with -terminate 10: 10 s after the last X client it exits. wlroots
# only restarts it lazily when it had run for more than 5 s, which it has by now.
sleep 6
sleep 11
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top anchor=top,left,right size=0x30 exclusive=30 \
	>"$SMOKE_DIR/panel.events" 2>&1 &
PID_panel=$!
wait_for "the panel is configured" grep -q configured "$SMOKE_DIR/panel.events"
check "the panel reserves its space" "[ \"\$(global output uy)\" = 30 ]"
alive "a panel changing the usable area while Xwayland is gone"
stop panel
check "the space is free again" "[ \"\$(global output uy)\" = 0 ]"

# And it comes back for the next client, with _NET_WORKAREA set.
x11 again workarea=1 pos=100,100
check "the next X client gets a new Xwayland" "[ -n \"\$(get again x)\" ]"
wait_for "with the work area" grep -q 'workarea 0 0 1280 720' "$SMOKE_DIR/again.events"
stop again
finish

# ---- a keymap that doesn't exist --------------------------------------------------------------

export XKB_DEFAULT_LAYOUT=no-such-layout
begin
do_input key a
do_input key alt+Tab
alive "keys with a layout that doesn't exist"
grep -q 'falling back to the us layout' "$LOG" || fail "the bad keymap is not reported"
echo "ok: the bad keymap is reported"
unset XKB_DEFAULT_LAYOUT
finish

echo "PASS: crash"
