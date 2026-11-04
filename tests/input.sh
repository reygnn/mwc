#!/bin/bash
# Headless input test: drives mwc with simulated mouse and keyboard (vinput)
# and checks window geometry and focus through the state file (src/testhook.c).
# Every window operation runs against a Wayland client (foot) and an X11 client
# (xterm), because they take different paths through the compositor.
# Output goes to build/input.log. Exits non-zero on failure.
LOG=build/input.log
. "$(dirname "$0")/lib.sh"

require_tools foot xterm Xwayland
[ -x build/vinput ] || { echo "FAIL: build/vinput not built"; exit 1; }

SCREEN_W=1280
SCREEN_H=720
export MWC_TEST_STATE=$SMOKE_DIR/state

marker='touch "$SMOKE_DIR/$0"; exec sleep 120'
start_mwc "foot sh -c '$marker' foot-up & \
	while [ ! -e \"\$SMOKE_DIR/foot-up\" ]; do sleep 0.1; done; \
	exec xterm -e sh -c '$marker' xterm-up"
wait_for "foot connected (Wayland)" test -e "$SMOKE_DIR/foot-up"
wait_for "xterm connected (XWayland)" test -e "$SMOKE_DIR/xterm-up"

start_vinput
refresh
B=$(global theme border)
T=$(global theme title)

# ---- helpers ---------------------------------------------------------------

outer_w() { echo $(($(get "$1" w) + 2 * B)); }
outer_h() { echo $(($(get "$1" h) + 2 * B + T)); }

# Remember a window's geometry in X0 Y0 W0 H0.
remember() {
	refresh
	X0=$(get "$1" x) Y0=$(get "$1" y) W0=$(get "$1" w) H0=$(get "$1" h)
}

check_same_geometry() {
	check "$1: $2" "[ \"\$(get $1 x)\" = $X0 ] && [ \"\$(get $1 y)\" = $Y0 ] && \
		[ \"\$(get $1 w)\" = $W0 ] && [ \"\$(get $1 h)\" = $H0 ]"
}

title_x() { refresh; echo $(($(get "$1" x) + $(outer_w "$1") / 2)); }
title_y() { refresh; echo $(($(get "$1" y) + B + T / 2)); }

# Let the double-click window pass (DOUBLE_CLICK_MS in input.c).
settle() { sleep 0.45; }

# Bring a window to the front with the Alt+Tab binding.
bring_to_front() {
	local i
	for ((i = 0; i < 5; i++)); do
		refresh
		[ "$(get "$1" active)" = 1 ] && return 0
		do_input key alt+Tab
	done
	fail "$1 never got the focus"
}

drag() { # from_x from_y to_x to_y [button]
	local button=${5:-left}
	do_input move "$1" "$2"
	do_input down "$button"
	do_input move "$3" "$4"
	do_input up "$button"
}

# ---- the tests -------------------------------------------------------------

test_move() {
	local app=$1 tx ty
	bring_to_front "$app"
	settle
	remember "$app"
	tx=$(title_x "$app") ty=$(title_y "$app")
	drag "$tx" "$ty" $((tx + 40)) $((ty + 30))
	check "$app: title drag moves the window" \
		"[ \"\$(get $app x)\" = $((X0 + 40)) ] && [ \"\$(get $app y)\" = $((Y0 + 30)) ]"

	remember "$app"
	local cx=$((X0 + W0 / 2)) cy=$((Y0 + H0 / 2))
	do_input hold alt
	drag "$cx" "$cy" $((cx - 20)) $((cy + 10))
	do_input release alt
	check "$app: Alt+btn1 drag moves the window" \
		"[ \"\$(get $app x)\" = $((X0 - 20)) ] && [ \"\$(get $app y)\" = $((Y0 + 10)) ]"

	remember "$app"
	do_input key alt+F7
	refresh
	local mx=$(global cursor x) my=$(global cursor y)
	do_input move $((mx + 25)) $((my + 15))
	do_input click left
	# Alt+F7 warps to a whole pixel while vinput aims at pixel centres, so
	# the distance ends in .5 and may round either way.
	check "$app: Alt+F7 moves until the click" \
		"[ \$((\$(get $app x) - $X0 - 25)) -ge 0 ] && [ \$((\$(get $app x) - $X0 - 25)) -le 1 ] && \
		[ \$((\$(get $app y) - $Y0 - 15)) -ge 0 ] && [ \$((\$(get $app y) - $Y0 - 15)) -le 1 ] && \
		[ \"\$(global cursor mode)\" = 0 ]"
}

test_resize() {
	local app=$1
	bring_to_front "$app"
	remember "$app"
	local rx=$((X0 + W0 + 2 * B - 2)) ry=$((Y0 + H0 + 2 * B + T - 2))
	drag "$rx" "$ry" $((rx + 60)) $((ry + 40))
	check "$app: bottom-right corner grows the window" \
		"[ \"\$(get $app w)\" -gt $W0 ] && [ \"\$(get $app h)\" -gt $H0 ] && \
		[ \"\$(get $app x)\" = $X0 ] && [ \"\$(get $app y)\" = $Y0 ]"

	remember "$app"
	local right=$((X0 + W0)) bottom=$((Y0 + H0))
	drag $((X0 + 2)) $((Y0 + 2)) $((X0 + 32)) $((Y0 + 22))
	check "$app: top-left corner keeps the opposite edges" \
		"[ \"\$(get $app w)\" -lt $W0 ] && [ \"\$(get $app h)\" -lt $H0 ] && \
		[ \$((\$(get $app x) + \$(get $app w))) = $right ] && \
		[ \$((\$(get $app y) + \$(get $app h))) = $bottom ]"

	remember "$app"
	local cx=$((X0 + W0 / 2)) cy=$((Y0 + H0 / 2))
	do_input hold alt
	drag "$cx" "$cy" $((cx + 40)) $((cy + 30)) right
	do_input release alt
	check "$app: Alt+btn3 drag resizes the window" \
		"[ \"\$(get $app w)\" != $W0 ] || [ \"\$(get $app h)\" != $H0 ]"

	remember "$app"
	do_input key alt+F8
	refresh
	local mx=$(global cursor x) my=$(global cursor y)
	do_input move $((mx + 30)) $((my + 20))
	do_input click left
	check "$app: Alt+F8 resizes until the click" \
		"[ \"\$(get $app w)\" -gt $W0 ] && [ \"\$(get $app h)\" -gt $H0 ] && \
		[ \"\$(global cursor mode)\" = 0 ]"
}

check_maximized() {
	check "$1: $2 maximizes" \
		"[ \"\$(get $1 max)\" = 1 ] && [ \"\$(outer_w $1)\" = $SCREEN_W ] && \
		[ \"\$(outer_h $1)\" = $SCREEN_H ] && [ \"\$(get $1 x)\" = 0 ] && \
		[ \"\$(get $1 y)\" = 0 ]"
}

test_maximize() {
	local app=$1
	bring_to_front "$app"
	settle
	remember "$app"
	refresh
	do_input move $(($(get "$app" x) + $(outer_w "$app") - B - T / 2)) $(title_y "$app")
	do_input click left
	check_maximized "$app" "the maximize button"
	do_input key alt+F5
	check_same_geometry "$app" "Alt+F5 restores the geometry"

	settle
	do_input move "$(title_x "$app")" "$(title_y "$app")"
	do_input click left
	do_input click left
	check_maximized "$app" "a double click on the title"
	settle
	do_input move "$(title_x "$app")" "$(title_y "$app")"
	do_input click left
	do_input click left
	check_same_geometry "$app" "a second double click restores"

	do_input key alt+F10
	check_maximized "$app" "Alt+F10"
	do_input key alt+F10
	check_same_geometry "$app" "Alt+F10 toggles back"

	do_input key alt+F10
	check_maximized "$app" "Alt+F10"
	settle
	local tx ty
	tx=$(title_x "$app") ty=$(title_y "$app")
	drag "$tx" "$ty" $((tx + 50)) $((ty + 120))
	check "$app: dragging a maximized window restores its size" \
		"[ \"\$(get $app max)\" = 0 ] && [ \"\$(get $app w)\" = $W0 ] && \
		[ \"\$(get $app h)\" = $H0 ]"
}

test_fullscreen() {
	local app=$1
	bring_to_front "$app"
	remember "$app"
	do_input key alt+F11
	check "$app: Alt+F11 goes fullscreen without a frame" \
		"[ \"\$(get $app full)\" = 1 ] && [ \"\$(get $app frame)\" = 0 ] && \
		[ \"\$(get $app w)\" = $SCREEN_W ] && [ \"\$(get $app h)\" = $SCREEN_H ]"
	do_input key alt+F11
	check "$app: Alt+F11 leaves fullscreen" \
		"[ \"\$(get $app full)\" = 0 ] && [ \"\$(get $app frame)\" = 1 ]"
	check_same_geometry "$app" "fullscreen restores the geometry"
}

test_minimize() {
	local app=$1 other=$2
	bring_to_front "$app"
	settle
	remember "$app"
	refresh
	do_input move $(($(get "$app" x) + $(outer_w "$app") - B - T - T / 2)) $(title_y "$app")
	do_input click left
	check "$app: the minimize button makes an icon, $other gets the focus" \
		"[ \"\$(get $app min)\" = 1 ] && [ \"\$(get $app active)\" = 0 ] && \
		[ \"\$(get $other active)\" = 1 ] && [ \"\$(get $other kbfocus)\" = 1 ]"

	settle
	do_input move $(($(get "$app" icon_x) + 30)) $(($(get "$app" icon_y) + 30))
	do_input click left
	do_input click left
	check "$app: a double click on the icon restores it with the focus" \
		"[ \"\$(get $app min)\" = 0 ] && [ \"\$(get $app active)\" = 1 ] && \
		[ \"\$(get $app kbfocus)\" = 1 ]"
	check_same_geometry "$app" "the icon restores the geometry"

	do_input key alt+F9
	check "$app: Alt+F9 minimizes" "[ \"\$(get $app min)\" = 1 ]"
	settle
	refresh
	do_input move $(($(get "$app" icon_x) + 30)) $(($(get "$app" icon_y) + 30))
	do_input click right
	check "$app: btn3 on an icon posts the window menu" "[ \"\$(global menu open)\" = 1 ]"
	do_input key Escape
	do_input click left
	do_input click left
	check "$app: icon restored again" "[ \"\$(get $app min)\" = 0 ]"
}

test_menus() {
	local app=$1
	bring_to_front "$app"
	settle
	do_input key alt+space
	check "$app: Alt+Space opens the window menu" "[ \"\$(global menu open)\" = 1 ]"
	do_input key Escape
	check "$app: Escape closes it" "[ \"\$(global menu open)\" = 0 ]"
	do_input key shift+Escape
	check "$app: Shift+Escape opens it" "[ \"\$(global menu open)\" = 1 ]"
	do_input key Escape

	remember "$app"
	do_input key alt+space
	do_input key x
	check_maximized "$app" "the menu mnemonic x"
	do_input key alt+space
	do_input key r
	check_same_geometry "$app" "the menu mnemonic r restores"

	settle
	do_input move "$(title_x "$app")" "$(title_y "$app")"
	do_input click right
	check "$app: btn3 on the title opens the menu" "[ \"\$(global menu open)\" = 1 ]"
	do_input key Escape
	refresh
	do_input move $(($(get "$app" x) + B + T / 2)) "$(title_y "$app")"
	do_input click left
	check "$app: the menu button opens the menu" "[ \"\$(global menu open)\" = 1 ]"
	do_input key Escape
	check "$app: menus are closed again" "[ \"\$(global menu open)\" = 0 ]"
}

test_focus() {
	bring_to_front foot
	check "foot has the keyboard" "[ \"\$(get foot kbfocus)\" = 1 ] && [ \"\$(get XTerm active)\" = 0 ]"
	do_input key alt+Tab
	check "Alt+Tab focuses xterm" "[ \"\$(get XTerm kbfocus)\" = 1 ] && [ \"\$(get foot active)\" = 0 ]"
	do_input key alt+shift+Tab
	check "Alt+Shift+Tab focuses foot" "[ \"\$(get foot kbfocus)\" = 1 ]"
	do_input key alt+Escape
	check "Alt+Escape focuses xterm" "[ \"\$(get XTerm kbfocus)\" = 1 ]"
	do_input key alt+F3
	check "Alt+F3 lowers xterm, foot gets the focus" \
		"[ \"\$(get foot kbfocus)\" = 1 ] && [ \"\$(get XTerm active)\" = 0 ]"
}

test_root_menu() {
	# Minimize everything so the corner is free for the desktop.
	bring_to_front foot
	do_input key alt+F9
	bring_to_front XTerm
	do_input key alt+F9
	check "both windows are icons" "[ \"\$(get foot min)\" = 1 ] && [ \"\$(get XTerm min)\" = 1 ]"
	settle
	do_input move $((SCREEN_W - 10)) 10
	do_input click right
	check "btn3 on the desktop opens the root menu" \
		"[ \"\$(global menu open)\" = 1 ] && [ \"\$(global menu title)\" = yes ]"
	do_input key Escape
	do_input click left
	check "btn1 on the desktop opens it too" "[ \"\$(global menu open)\" = 1 ]"
	do_input key Escape
	check "Escape closes the root menu" "[ \"\$(global menu open)\" = 0 ]"

	refresh
	do_input move $(($(get foot icon_x) + 30)) $(($(get foot icon_y) + 30))
	do_input click left
	do_input click left
	check "foot is back" "[ \"\$(get foot min)\" = 0 ]"
	settle
	refresh
	do_input move $(($(get XTerm icon_x) + 30)) $(($(get XTerm icon_y) + 30))
	do_input click left
	do_input click left
	check "xterm is back" "[ \"\$(get XTerm min)\" = 0 ]"
}

test_close() {
	bring_to_front XTerm
	do_input key alt+F4
	check "Alt+F4 closes xterm, foot gets the focus" \
		"[ -z \"\$(get XTerm x)\" ] && [ \"\$(get foot kbfocus)\" = 1 ]"
}

# The window must be gone first: Quit is checked through the exit status.
test_quit() {
	settle
	do_input move $((SCREEN_W - 10)) 10
	do_input click right
	do_input key q
	do_input key c
	check "Quit... then Cancel keeps mwc running" "[ \"\$(global menu open)\" = 0 ]"
	do_input click right
	do_input key q
	echo "key o" >&"${VINPUT[1]}"
	local i status=
	for ((i = 0; i < 100; i++)); do
		kill -0 "$pid" 2>/dev/null || break
		sleep 0.05
	done
	kill -0 "$pid" 2>/dev/null && fail "Quit... then OK did not end mwc"
	wait "$pid"
	status=$?
	pid=
	[ "$status" = 0 ] || fail "mwc exited with status $status"
	echo "ok: Quit... then OK ends mwc cleanly"
}

check "both windows are mapped" "[ -n \"\$(get foot x)\" ] && [ -n \"\$(get XTerm x)\" ]"
for app in foot XTerm; do
	test_move "$app"
	test_resize "$app"
	test_maximize "$app"
	test_fullscreen "$app"
	test_menus "$app"
done
test_minimize foot XTerm
test_minimize XTerm foot
test_focus
test_root_menu
test_close
test_quit

echo "PASS: input"
