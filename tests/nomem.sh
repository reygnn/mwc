#!/bin/bash
# Headless test of mwc without the memory for a scene node. build/mwc-nomem fails the
# call tests/nomem.c is told to through <state file>.nomem. Without its nodes mwc
# doesn't start; a window, popup or layer surface costs its client the connection, and
# nothing of it stays in the scene; an output isn't used; the wallpaper, a side of the
# outline and the menu are left out once and come the next time.
# Output goes to build/nomem.log. Exits non-zero on failure.
LOG=build/nomem.log
MWC=${MWC_NOMEM:-build/mwc-nomem} # make check: the sanitizer build of it
. "$(dirname "$0")/lib.sh"

for tool in build/vinput build/pngtool build/xdgc build/layerc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
export MWC_MOVE_OPAQUE=False # the outline, whose sides are nodes of their own

# nomem <function> [n [k]]: from the nth call of it from now (1), k fail (1).
nomem() {
	FAILS=$(grep -c '^nomem: ' "$LOG")
	echo "$1 ${2:-1} ${3:-1}" >"$MWC_TEST_STATE.nomem"
}
# failed <function>: it did, since nomem, or the test checks nothing.
failed() {
	wait_for "$1 failed" eval "[ \$(grep -c '^nomem: $1 fails' \"$LOG\") -gt 0 ] && \
		[ \$(grep -c '^nomem: ' \"$LOG\") -gt $FAILS ]"
}
alive() { kill -0 "$pid" 2>/dev/null || fail "mwc died: $1"; echo "ok: mwc survives: $1"; }
nodes() { refresh; echo "$(global scene trees) $(global scene rects) $(global scene buffers)"; }
wayland() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$name" "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "PID_$name=$!"
}
layer() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/layerc "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "PID_$name=$!"
}
stop() {
	local var="PID_$1"
	kill "${!var}" 2>/dev/null
	wait "${!var}" 2>/dev/null
}
# cut_off <client> <function>: that call failed, and mwc told the client.
cut_off() {
	failed "$2"
	wait_for "$1 is told: no memory" grep -q '^error: no memory' "$SMOKE_DIR/$1.events"
	stop "$1"
}

# ---- startup --------------------------------------------------------------------------

# Each node of the scene's layers, the icon box's and the dock's: mwc says so and ends.
startup_fails() { # function n
	local status
	echo "$1 $2" >"$MWC_TEST_STATE.nomem"
	XDG_RUNTIME_DIR=$SMOKE_DIR WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
		WLR_RENDERER=pixman timeout "$TIMEOUT" "$MWC" -s true >"$LOG" 2>&1
	status=$?
	grep -q "^nomem: $1 fails" "$LOG" || fail "startup: call $2 of $1 never came"
	[ "$status" = 1 ] && grep -q 'out of memory for the scene' "$LOG" ||
		fail "startup without call $2 of $1: status $status, not 1 with the reason"
	rm -f "$MWC_TEST_STATE.nomem" "$SMOKE_DIR"/wayland-*
}
startup_fails wlr_scene_create 1
startup_fails wlr_scene_attach_output_layout 1
for n in $(seq 1 13); do startup_fails wlr_scene_tree_create "$n"; done
startup_fails wlr_scene_rect_create 1
for n in $(seq 1 6); do startup_fails wlr_scene_buffer_create "$n"; done
echo "ok: no start without the scene's nodes, 22 of them"

start_mwc "sleep 300"
start_vinput
base=$(nodes)

# ---- windows ---------------------------------------------------------------------------

# view_init_common(): its tree, the two rectangles, the four sides and the icon; then
# the client's surfaces.
i=0
for f in "wlr_scene_tree_create 1" "wlr_scene_rect_create 1" "wlr_scene_rect_create 2" \
		"wlr_scene_buffer_create 1" "wlr_scene_buffer_create 4" \
		"wlr_scene_buffer_create 5" "wlr_scene_xdg_surface_create 1"; do
	i=$((i + 1))
	nomem $f
	wayland "w$i"
	cut_off "w$i" "${f% *}"
	check "nothing of window $i stays in the scene" "[ \"\$(nodes)\" = \"$base\" ]"
done
alive "windows without their nodes"

# A popup of a window.
wayland pw popup=20,20,80,40 popup_after="$SMOKE_DIR/popup"
check "pw is mapped" "[ -n \"\$(get pw x)\" ]"
nomem wlr_scene_xdg_surface_create
touch "$SMOKE_DIR/popup"
cut_off pw wlr_scene_xdg_surface_create
check "nothing of pw stays in the scene" "[ \"\$(nodes)\" = \"$base\" ]"

# A layer surface: its own nodes, the tree for its popups, a popup.
nomem wlr_scene_layer_surface_v1_create
layer l1
cut_off l1 wlr_scene_layer_surface_v1_create
nomem wlr_scene_tree_create
layer l2
cut_off l2 wlr_scene_tree_create
nomem wlr_scene_xdg_surface_create
layer l3 popup=00ff00
cut_off l3 wlr_scene_xdg_surface_create
check "nothing of the layer surfaces stays in the scene" "[ \"\$(nodes)\" = \"$base\" ]"
alive "layer surfaces without their nodes"

# Still a window as usual.
wayland ok
check "ok is mapped" "[ -n \"\$(get ok x)\" ] && [ \"\$(get ok active)\" = 1 ]"

# ---- the menu and the outline ----------------------------------------------------------

# The first menu makes its tree and two buffers: without one of them no menu this time.
for f in "wlr_scene_tree_create 1" "wlr_scene_buffer_create 2"; do
	before=$(nodes)
	nomem $f
	do_input key alt+space
	failed "${f% *}"
	check "no menu without its ${f% *}" "[ \"\$(global menu open)\" = 0 ] && \
		[ \"\$(nodes)\" = \"$before\" ]"
done
do_input key alt+space
check "the next time the menu opens" "[ \"\$(global menu open)\" = 1 ]"
do_input key Escape
check "the menu is closed" "[ \"\$(global menu open)\" = 0 ]"

# The outline's first side.
x=$(get ok x)
nomem wlr_scene_buffer_create
do_input key alt+F7
failed wlr_scene_buffer_create
check "the move goes on, the outline short of a side" "[ \"\$(global outline shown)\" = 1 ]"
short=$(global scene buffers)
do_input key Right
check "the outline drawn next has the side" "[ \"\$(global scene buffers)\" = $((short + 1)) ]"
do_input key Return
check "the window moved" "[ \"\$(get ok x)\" -gt $x ]"
alive "the menu and the outline without their nodes"
stop ok

# ---- the wallpaper ---------------------------------------------------------------------

build/pngtool stripes "$SMOKE_DIR/wall.png" 64 64 ff0000 0000ff || fail "pngtool made no image"
RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
echo "Mwm*wallpaper: $SMOKE_DIR/wall.png" >"$RC"
# A reload places it twice: after the outputs' settings, and with the colours.
before=$(nodes)
nomem wlr_scene_buffer_create 1 2
reload_mwc
failed wlr_scene_buffer_create
check "no wallpaper this time" "[ \"\$(nodes)\" = \"$before\" ]"
reload_mwc
check "the next reload places it" \
	"[ \"\$(global scene buffers)\" = $(($(echo "$before" | cut -d' ' -f3) + 1)) ]"
rm -f "$RC"
reload_mwc

# ---- outputs ---------------------------------------------------------------------------

outputs() { refresh; grep -c '^output_' "$MWC_TEST_STATE"; }
for f in wlr_scene_rect_create wlr_scene_output_create; do
	nomem $f
	echo + >"$MWC_TEST_STATE.unplug"
	kill -USR2 "$pid"
	failed $f
	check "an output without its $f isn't used" "[ \"\$(outputs)\" = 1 ]"
done
echo + >"$MWC_TEST_STATE.unplug"
kill -USR2 "$pid"
check "the next one is" "[ \"\$(outputs)\" = 2 ]"
alive "outputs without their nodes"

stop_vinput
stop_mwc
echo "PASS: nomem"
