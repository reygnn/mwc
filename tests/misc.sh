#!/bin/bash
# Headless test of what the others don't reach: key bindings that start or end
# something, autostart, settings from the environment, clients without a server-side
# frame, and that the test hooks are off without being asked for.
# Output goes to build/misc.log. Exits non-zero on failure.
LOG=build/misc.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland grim
for tool in build/vinput build/pngtool build/xclient build/xdgc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
mkdir -p "$XDG_CONFIG_HOME/mwc"
begin() {
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
	DISPLAY=$X_DISPLAY build/xclient class="$name" "$@" >"$SMOKE_DIR/$name.log" 2>&1 &
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
started() { test -e "$SMOKE_DIR/started-$1"; }

# ---- the terminal -----------------------------------------------------------------

export MWC_TERMINAL="touch $SMOKE_DIR/started-1"
begin
do_input key ctrl+alt+Return
wait_for "Ctrl+Alt+Return starts the terminal" started 1
finish
export MWC_TERMINAL="touch $SMOKE_DIR/started-2"
begin
do_input key super+Return
wait_for "Super+Return too" started 2
finish
# From the file, also after a reload, and the environment goes first.
unset MWC_TERMINAL
echo "Mwm*terminal: touch $SMOKE_DIR/from-file-1" >"$XDG_CONFIG_HOME/mwc/mwmrc"
begin
do_input key super+Return
wait_for "Mwm*terminal" test -e "$SMOKE_DIR/from-file-1"
echo "Mwm*terminal: touch $SMOKE_DIR/from-file-2" >"$XDG_CONFIG_HOME/mwc/mwmrc"
kill -HUP "$pid"
sleep 0.5
do_input key super+Return
wait_for "a reload takes up a new one" test -e "$SMOKE_DIR/from-file-2"
finish
export MWC_TERMINAL="touch $SMOKE_DIR/from-env"
begin
do_input key super+Return
wait_for "MWC_TERMINAL beats Mwm*terminal" test -e "$SMOKE_DIR/from-env"
finish
unset MWC_TERMINAL
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

export MWC_TERMINAL="touch $SMOKE_DIR/started-3"
begin
do_input move 1000 600
do_input click right
do_input key n
wait_for "New Window in the root menu too" started 3
unset MWC_TERMINAL

# ---- lowering ----------------------------------------------------------------------

x11 under pos=100,100 size=300x200
x11 over pos=150,150 size=300x200
check "the newer window is in front" "[ \"\$(stack_pos over)\" = 0 ]"
settle
do_input move 300 $((150 + B + T / 2))
do_input click middle
check "btn2 on the title lowers it" "[ \"\$(stack_pos over)\" = 1 ]"
settle
do_input move 200 $((100 + B + T / 2))
do_input click left
check "the other one has the focus" "[ \"\$(get under active)\" = 1 ] && [ \"\$(stack_pos under)\" = 0 ]"
do_input move 200 200
do_input hold alt
do_input click middle
do_input release alt
check "Alt+btn2 in the window lowers it too" "[ \"\$(stack_pos under)\" = 1 ]"
stop under
stop over

# ---- no way out? --------------------------------------------------------------------

do_input move 1000 600
do_input click right
check "a menu is open" "[ \"\$(global menu open)\" = 1 ]"
echo 'key ctrl+alt+BackSpace' >&"${VINPUT[1]}"
for ((i = 0; i < 50; i++)); do
	kill -0 "$pid" 2>/dev/null || break
	sleep 0.1
done
kill -0 "$pid" 2>/dev/null && fail "Ctrl+Alt+Backspace with a menu open should end mwc"
wait "$pid"
status=$?
pid=
[ "$status" = 0 ] || fail "mwc exited with status $status"
echo "ok: Ctrl+Alt+Backspace ends mwc, also with a menu open"
rm -f "$SMOKE_DIR"/wayland-*

# ---- autostart ------------------------------------------------------------------------

# A path with a space: it is no shell command.
export XDG_CONFIG_HOME="$SMOKE_DIR/con fig"
mkdir -p "$XDG_CONFIG_HOME/mwc"
cat >"$XDG_CONFIG_HOME/mwc/autostart" <<AUTOSTART
#!/bin/sh
touch "$SMOKE_DIR/started-autostart"
AUTOSTART
chmod +x "$XDG_CONFIG_HOME/mwc/autostart"
export XDG_RUNTIME_DIR=$SMOKE_DIR WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
unset WAYLAND_DISPLAY DISPLAY
"$MWC" >"$LOG" 2>&1 &
pid=$!
wait_for "autostart runs without -s, wherever it lies" started autostart
stop_mwc
# Without a #! line, as a shell would run it.
rm -f "$SMOKE_DIR"/wayland-* "$SMOKE_DIR/started-autostart"
echo "touch \"$SMOKE_DIR/started-autostart\"" >"$XDG_CONFIG_HOME/mwc/autostart"
"$MWC" >"$LOG" 2>&1 &
pid=$!
wait_for "an autostart without a #! line runs too" started autostart
stop_mwc
rm "$XDG_CONFIG_HOME/mwc/autostart"
export XDG_CONFIG_HOME=$SMOKE_DIR/config

# ---- settings from the environment -------------------------------------------------------

export MWC_ACTIVE='#102030' MWC_INACTIVE='#405060' MWC_FONT='Sans Bold 24'
begin
check "MWC_FONT: the title bar grows with it" "[ \"\$(global theme title)\" -gt 30 ]"
x11 first pos=100,100 size=300x200
x11 second pos=600,100 size=300x200
pixel_is "MWC_ACTIVE: the focused frame" $((600 + 3)) 200 102030
pixel_is "MWC_INACTIVE: the other one" $((100 + 3)) 200 405060
stop first
stop second
finish
unset MWC_ACTIVE MWC_INACTIVE MWC_FONT

export MWC_FONT='Sans Bold 1000'
begin
check "an absurd font: the title bar stays usable" "[ \"\$(global theme title)\" -le 128 ]"
finish
export MWC_FONT='Sans-12'
begin
check "a name without a size is not taken" "[ \"\$(global theme title)\" -lt 30 ]"
grep -q 'ignoring font "Sans-12"' "$LOG" || fail "the font is not reported"
echo "ok: the font is reported"
finish
unset MWC_FONT

# ---- configuration details ------------------------------------------------------------------

cat >"$XDG_CONFIG_HOME/mwc/mwmrc" <<'RC'
Mwm*org.mwc.Test*activeBackground: #00aa00
Mwm*rootBackground: rgb:-1/-1/-1
RC
begin
pixel_is "rgb: with a sign is not a colour" 1000 600 5b7792
grep -q 'not a colour' "$LOG" || fail "the bad colour is not reported"
echo "ok: the bad colour is reported"
WAYLAND_DISPLAY=wayland-0 build/xdgc app=org.mwc.Test size=300x200 >/dev/null 2>&1 &
PID_dotted=$!
check "the client is mapped" "[ -n \"\$(get org.mwc.Test x)\" ]"
refresh
pixel_is "an app_id with dots gets its colours" $(($(get org.mwc.Test x) + 3)) $(($(get org.mwc.Test y) + 100)) 00aa00
kill "$PID_dotted"
wait "$PID_dotted" 2>/dev/null
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

# ---- clients that draw their own frame ---------------------------------------------------------

WAYLAND_DISPLAY=wayland-0 build/xdgc app=own csd=1 size=300x200 >/dev/null 2>&1 &
PID_own=$!
check "a client asking for no server-side frame has none" \
	"[ -n \"\$(get own x)\" ] && [ \"\$(get own frame)\" = 0 ] && [ \"\$(get own bw)\" = 0 ]"
stop own
finish

# ---- the middle button on the desktop ---------------------------------------------------

mkdir "$SMOKE_DIR/bin"
printf '#!/bin/sh\n: >"%s/ran-mrun"\n' "$SMOKE_DIR" >"$SMOKE_DIR/bin/mrun"
chmod +x "$SMOKE_DIR/bin/mrun"
PLAIN_PATH=$PATH
export PATH=$SMOKE_DIR/bin:$PATH
begin
do_input move 1200 700
do_input click middle
wait_for "the middle button on the desktop runs mrun" test -e "$SMOKE_DIR/ran-mrun"
rm "$SMOKE_DIR/ran-mrun"
refresh
[ "$(global menu open)" = 0 ] || fail "the middle button opened the root menu"

WAYLAND_DISPLAY=wayland-0 build/xdgc app=midwin size=300x200 >/dev/null 2>&1 &
MIDWIN=$!
check "a window is up" '[ -n "$(get midwin w)" ]'
do_input move $(($(get midwin x) + B + 50)) $(($(get midwin y) + T + B + 50))
do_input click middle
sleep 0.3
[ ! -e "$SMOKE_DIR/ran-mrun" ] || fail "the middle button on a window ran mrun"
echo "ok: on a window it stays the window's"
kill "$MIDWIN"
wait "$MIDWIN" 2>/dev/null
check "the window is gone" '[ -z "$(get midwin w)" ]'

printf 'Mwm*runDialog: touch %s/ran-other\n' "$SMOKE_DIR" >"$XDG_CONFIG_HOME/mwc/mwmrc"
kill -HUP "$pid"
sleep 0.5
do_input move 1200 700
do_input click middle
wait_for "Mwm*runDialog is what it runs" test -e "$SMOKE_DIR/ran-other"
[ ! -e "$SMOKE_DIR/ran-mrun" ] || fail "mrun ran as well"
rm "$XDG_CONFIG_HOME/mwc/mwmrc"
finish
export PATH=$PLAIN_PATH

# ---- the version -------------------------------------------------------------------------------

version=$(sed -n 's/^#define MWC_VERSION "\(.*\)"$/\1/p' src/version.h)
[ -n "$version" ] || fail "no MWC_VERSION in src/version.h"
[ "$("$MWC" -v)" = "mwc $version" ] || fail "mwc -v should print: mwc $version"
echo "ok: mwc -v prints mwc $version"

# ---- the hooks are off unless asked for --------------------------------------------------------

unset MWC_TEST_STATE
export XDG_RUNTIME_DIR=$SMOKE_DIR WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
unset WAYLAND_DISPLAY DISPLAY
"$MWC" -s "true" >"$LOG" 2>&1 &
pid=$!
wait_for "mwc is up" test -S "$SMOKE_DIR/wayland-0"
if WAYLAND_DISPLAY=wayland-0 build/vinput 1280 720 </dev/null >"$SMOKE_DIR/vinput.log" 2>&1; then
	fail "the virtual input protocols should be off"
fi
grep -q 'protocols are missing' "$SMOKE_DIR/vinput.log" || fail "vinput failed for another reason"
echo "ok: without MWC_TEST_STATE no client can inject input"
stop_mwc

echo "PASS: misc"
