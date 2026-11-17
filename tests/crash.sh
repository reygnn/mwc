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

# A frame asked for only later has to fit too. As late as xdg-decoration allows:
wayland lateframe pre=max late_frame=xdg size=300x200
check "the frame comes after the first configure, and it makes room for it" \
	"[ \"\$(get lateframe max)\" = 1 ] && [ \"\$(get lateframe frame)\" = 1 ] && \
	[ \$((\$(get lateframe w) + 2 * B)) = 1280 ] && [ \$((\$(get lateframe h) + 2 * B + T)) = 720 ]"
stop lateframe
check "the window is gone" "[ -z \"\$(get lateframe x)\" ]"
# And once mapped, with KDE's protocol, as GTK3 does:
wayland latekde pre=max late_frame=kde size=300x200
check "maximized without a frame yet, it fills the output" \
	"[ \"\$(get latekde max)\" = 1 ] && [ \"\$(get latekde frame)\" = 0 ] && [ \"\$(get latekde w)\" = 1280 ]"
check "the frame comes, and it makes room for it" \
	"[ \"\$(get latekde frame)\" = 1 ] && [ \$((\$(get latekde w) + 2 * B)) = 1280 ] && \
	[ \$((\$(get latekde h) + 2 * B + T)) = 720 ]"
stop latekde
check "the window is gone" "[ -z \"\$(get latekde x)\" ]"
wayland latesnap late_frame=kde size=300x200
check "a window without a frame yet" "[ \"\$(get latesnap frame)\" = 0 ]"
do_input key super+Left
check "snapped" "[ \"\$(get latesnap snap)\" != 0 ]"
check "the frame comes, and it still fits its half" \
	"[ \"\$(get latesnap frame)\" = 1 ] && [ \$((\$(get latesnap w) + 2 * B)) = 628 ]"
stop latesnap
check "the window is gone" "[ -z \"\$(get latesnap x)\" ]"

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

# ---- X windows asking for states before they are mapped ------------------------------

for when in before after; do
	x11 xfull$when ask_$when=fullscreen size=300x200
	check "fullscreen asked for $when mapping: it maps fullscreen" \
		"[ \"\$(get xfull$when full)\" = 1 ] && [ \"\$(get xfull$when frame)\" = 0 ] && \
		[ \"\$(get xfull$when w)\" = 1280 ] && [ \"\$(get xfull$when h)\" = 720 ]"
	do_input key alt+F11
	check "leaving fullscreen: its own size, with a frame" \
		"[ \"\$(get xfull$when full)\" = 0 ] && [ \"\$(get xfull$when frame)\" = 1 ] && \
		[ \"\$(get xfull$when w)\" = 300 ] && [ \"\$(get xfull$when h)\" = 200 ]"
	stop xfull$when
	check "the window is gone" "[ -z \"\$(get xfull$when x)\" ]"

	x11 xmax$when ask_$when=maximized size=300x200
	check "maximized asked for $when mapping: it maps maximized" \
		"[ \"\$(get xmax$when max)\" = 1 ] && [ \$((\$(get xmax$when w) + 2 * B)) = 1280 ] && \
		[ \$((\$(get xmax$when h) + 2 * B + T)) = 720 ]"
	do_input key alt+F10
	check "un-maximized: its own size, not nothing" \
		"[ \"\$(get xmax$when max)\" = 0 ] && [ \"\$(get xmax$when w)\" = 300 ] && \
		[ \"\$(get xmax$when h)\" = 200 ]"
	stop xmax$when
	check "the window is gone" "[ -z \"\$(get xmax$when x)\" ]"
done
if command -v xterm >/dev/null; then
	DISPLAY=$X_DISPLAY xterm -fullscreen -class XTermFull >/dev/null 2>&1 &
	PID_xtermfull=$!
	check "a real xterm -fullscreen" "[ \"\$(get XTermFull full)\" = 1 ] && [ \"\$(get XTermFull w)\" = 1280 ]"
	stop xtermfull
	check "the window is gone" "[ -z \"\$(get XTermFull x)\" ]"
fi

# Maximized before a null buffer, and the next life starts without it. wlroots keeps what
# was last scheduled, which the client must not be told.
bad remapplain remap-plain
wait_for "the client started over" grep -q 'configured max=' "$SMOKE_DIR/remapplain.events"
grep -q 'configured max=0' "$SMOKE_DIR/remapplain.events" || fail "the new first configure says maximized"
grep -q 'activated=0' "$SMOKE_DIR/remapplain.events" ||
	fail "the new first configure says activated, before mwc gave it the focus"
echo "ok: the new first configure says nothing of its old states"
check "and it isn't maximized" "[ \"\$(get bad max)\" = 0 ]"
stop remapplain
check "the window is gone" "[ -z \"\$(get bad x)\" ]"

# ---- frames asked for in the order nobody expected --------------------------------------

wayland earlykde early_frame=1 size=300x200
check "KDE's protocol before the surface has a role: it gets its frame" "[ \"\$(get earlykde frame)\" = 1 ]"
stop earlykde
do_input key super+t
wayland tilekde late_frame=kde size=300x200
check "tiled, without a frame yet" "[ \"\$(get tilekde tiled)\" = 1 ] && [ \"\$(get tilekde frame)\" = 0 ]"
check "the frame comes, and it stays in its cell" "[ \"\$(get tilekde frame)\" = 1 ] && \
	[ \$((\$(get tilekde y) + \$(get tilekde h) + 2 * B + T)) -le 720 ] && \
	[ \$((\$(get tilekde x) + \$(get tilekde w) + 2 * B)) -le 1280 ]"
stop tilekde
check "the window is gone" "[ -z \"\$(get tilekde x)\" ]"
do_input key super+t

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
# Ours is the one with this test's SMOKE_DIR in its environment.
xwayland_running() {
	local p
	for p in /proc/[0-9]*; do
		[ "$(cat "$p/comm" 2>/dev/null)" = Xwayland ] || continue
		tr '\0' '\n' <"$p/environ" 2>/dev/null | grep -qx "SMOKE_DIR=$SMOKE_DIR" &&
			return 0
	done
	return 1
}
for ((i = 0; i < 125; i++)); do
	xwayland_running || break
	sleep 0.2
done
xwayland_running && fail "Xwayland didn't exit 10 s after the last X client"
echo "ok: Xwayland exited by itself"
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

# ---- a huge window ----------------------------------------------------------------------------
# Through wp_viewporter a 1x1 buffer makes a window of any size for next to no memory.
# Its frame costs its border, not its area: one buffer for the whole frame would be
# 16000 x 16000 x 4 bytes, 1 GB.

begin
wayland whuge size=16000x16000 viewport=1
check "a 16000x16000 window is mapped, with its frame" \
	"[ \"\$(get whuge w)\" = 16000 ] && [ \"\$(get whuge h)\" = 16000 ] && [ \"\$(get whuge frame)\" = 1 ]"
do_input key alt+Tab
do_input key alt+Tab
sleep 0.5
alive "a 16000x16000 window"
peak=$(awk '/^VmHWM:/ { print $2 }' /proc/$pid/status)
[ "$peak" -lt 1048576 ] || fail "mwc peaked at $peak kB with a huge window"
echo "ok: mwc stays below 1 GB with it ($peak kB)"
grep -q 'cannot create a' "$LOG" && fail "a frame buffer was too large"
echo "ok: every frame buffer could be made"
stop whuge
# The largest size a viewport allows: mwc takes it at most 2^20 wide, where frame
# sizes, a pointer over it and its placement don't overflow.
wayland wmax size=2147483647x100 viewport=1
check "a 2147483647 wide window is mapped, 2^20 wide" "[ \"\$(get wmax w)\" = 1048576 ]"
do_input move 640 360
do_input move 641 361
do_input key alt+Tab
do_input key alt+Tab
alive "a window 2147483647 wide"
stop wmax
finish

# ---- client numbers past any screen ---------------------------------------------------------
# Struts, size hints and a window menu position at the ends of their types: mwc must not
# do signed arithmetic that overflows on them, which make check's UBSan aborts on.

begin
x11 strutmax type=dock pos=0,0 size=100x30 strut=0,4294967295,0,2147483648
check "a panel with absurd struts is mapped" "[ -n \"\$(get strutmax x)\" ]"
check "they are not believed" "[ \"\$(global output uw)\" = 1280 ] && [ \"\$(global output uh)\" = 720 ]"
x11 basemin pos=200,200 size=300x200 base=-2147483648x-2147483648 min=10x10
check "a window with a base size of INT_MIN is mapped" "[ -n \"\$(get basemin x)\" ]"
do_input key alt+F8
do_input key Right
do_input key Return
alive "resizing it"
wayland menuat menu_at=2147483647,2147483647
check "a window asking for a menu far away is mapped" "[ -n \"\$(get menuat x)\" ]"
do_input move $(($(get menuat x) + 50)) $(($(get menuat y) + 80))
do_input click left
wait_for "it asks for the window menu" grep -q 'menu asked' "$SMOKE_DIR/menuat.events"
check "the menu opens on the screen" "[ \"\$(global menu open)\" = 1 ] && \
	[ \"\$(global menu x)\" -lt 1280 ] && [ \"\$(global menu y)\" -lt 720 ]"
do_input key Escape
alive "a window menu asked for at INT32_MAX"
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
