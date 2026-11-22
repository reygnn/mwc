#!/bin/bash
# Headless test of who gets the pointer and keyboard when: the implicit grab while a
# button is down, presses mwc keeps to itself, the surface under a pointer that didn't
# move, popup grabs, drag and drop, and client-initiated moves.
# Output goes to build/pointer.log. Exits non-zero on failure.
LOG=build/pointer.log
. "$(dirname "$0")/lib.sh"

require_tools grim
for tool in build/vinput build/pngtool build/badclient build/xdgc build/capturec build/powerc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
BTN_LEFT=272
start_mwc 'exec sleep 300'
start_vinput
refresh
# A pointer image before any motion: mwc's, as the first output joins the layout.
WAYLAND_DISPLAY=wayland-0 build/capturec cursor HEADLESS-1 "$SMOKE_DIR/first.png" \
	>"$SMOKE_DIR/first.txt" || fail "capturing the pointer before any motion"
grep -q 'opaque=[1-9]' "$SMOKE_DIR/first.txt" ||
	fail "no pointer before the first motion: $(cat "$SMOKE_DIR/first.txt")"
echo "ok: a pointer before the first motion"
B=$(global theme border)
T=$(global theme title)

client() { # name mode
	WAYLAND_DISPLAY=wayland-0 build/badclient "$2" "$1" >"$SMOKE_DIR/$1.log" 2>&1 &
	eval "PID_$1=$!"
	eval "SYNCS_$1=1" # badclient syncs on SIGUSR1, see lacks()
	check "$1 is mapped" "[ -n \"\$(get $1 x)\" ]"
}
stop() {
	local var="PID_$1"
	kill "${!var}" 2>/dev/null
	wait "${!var}" 2>/dev/null
	check "$1 is gone" "[ -z \"\$(get $1 x)\" ]"
}
settle() { sleep 0.45; }
clear_log() { : >"$SMOKE_DIR/$1.log"; }
# has <client> <line regex>, polling; lacks <client> <regex> once all mwc sent has come.
has() { grep -qE "$2" "$SMOKE_DIR/$1.log"; }
saw() { wait_for "$1" has "$2" "$3"; }
lacks() { # description client regex
	local syncs="SYNCS_$2" pid="PID_$2" n
	if [ -n "${!syncs-}" ]; then
		# A roundtrip of the client's: what mwc sent before it is in the log after it.
		n=$(grep -c '^synced' "$SMOKE_DIR/$2.log")
		kill -USR1 "${!pid}" || fail "$1: $2 is gone"
		local i
		for ((i = 0; i < TIMEOUT * 25; i++)); do
			[ "$(grep -c '^synced' "$SMOKE_DIR/$2.log")" -gt "$n" ] && break
			sleep 0.04
		done
		[ "$(grep -c '^synced' "$SMOKE_DIR/$2.log")" -gt "$n" ] || fail "$1: $2 didn't sync"
	else
		sleep 0.2 # a client with no way to ask it
	fi
	has "$2" "$3" && { cat "$SMOKE_DIR/$2.log"; fail "$1"; }
	echo "ok: $1"
}

# Put the focused window at x y with the keyboard, these clients have no title bar to
# drag. Alt+F7 warps to a whole pixel and vinput aims at pixel centres, so the last
# pixel comes from Shift+arrows.
place() { # app x y
	do_input key alt+F7
	refresh
	do_input move $(($(global cursor x) + $2 - $(get "$1" x))) $(($(global cursor y) + $3 - $(get "$1" y)))
	refresh
	local i
	for ((i = $(global outline x); i < $2; i++)); do do_input key shift+Right; done
	for ((i = $(global outline x); i > $2; i--)); do do_input key shift+Left; done
	for ((i = $(global outline y); i < $3; i++)); do do_input key shift+Down; done
	for ((i = $(global outline y); i > $3; i--)); do do_input key shift+Up; done
	do_input key Return
	check "$1 is at $2,$3" "[ \"\$(get $1 x)\" = $2 ] && [ \"\$(get $1 y)\" = $3 ]"
}
focus_on() { # app
	local i
	for i in 1 2 3; do
		refresh
		[ "$(get "$1" active)" = 1 ] && return
		do_input key alt+Tab
	done
	fail "$1 never got the focus"
}

# Side by side, without frames: A at 100,100 and B at 500,100, both 300x200.
client A log
client B log
settle

focus_on A
place A 100 100
focus_on B
place B 500 100

# ---- the implicit grab -----------------------------------------------------------

do_input move 200 200
saw "the pointer enters A" A '^enter'
clear_log A
do_input down left
saw "A gets the press" A "^button $BTN_LEFT down"
do_input move 450 200
lacks "dragged out onto the desktop, A keeps the pointer" A '^leave'
saw "and is told where it is, in its own coordinates" A '^motion 350 100'
do_input move 600 200
lacks "dragged over B too" A '^leave'
lacks "B hears nothing of it" B '^button|^motion 100 100'
saw "with coordinates beyond its size" A '^motion 500 100'
clear_log B
do_input up left
saw "the release goes to A" A "^button $BTN_LEFT up"
saw "then A loses the pointer" A '^leave'
saw "and B gets it" B '^enter'
check "nothing opened on the way" "[ \"\$(global menu open)\" = 0 ]"

# Two buttons: only the last release ends it.
do_input move 200 200
clear_log A
do_input down left
do_input down middle
do_input move 450 200
do_input up left
lacks "one button up of two: A keeps the pointer" A '^leave'
do_input up middle
saw "the last one up: A loses it" A '^leave'

# ---- presses mwc keeps to itself --------------------------------------------------

do_input move 200 200
settle
focus_on A
clear_log A
do_input hold alt
do_input click middle
do_input release alt
lacks "Alt+btn2 is not for the client, neither down" A '^button 274 down'
lacks "nor up" A '^button 274 up'
do_input key alt+Tab
settle

# A release after the menu closed mustn't reach anyone either.
do_input move 1000 600
do_input click left
check "the root menu is open" "[ \"\$(global menu open)\" = 1 ]"
do_input move 200 200
clear_log A
do_input down left
check "a click elsewhere closes it" "[ \"\$(global menu open)\" = 0 ]"
do_input up left
lacks "and its release goes nowhere" A '^button'

# ---- the pointer doesn't move, but what is under it does ------------------------------

# Over A's left half, then B moves over it.
focus_on B
place B 300 100
do_input move 350 200
saw "the pointer is over B" B '^enter'
clear_log A
clear_log B
focus_on A
saw "A was raised over it: the pointer enters A without moving" A '^enter'
saw "and leaves the one below" B '^leave'
clear_log A
clear_log B
do_input click left
saw "a click goes to A" A "^button $BTN_LEFT down"
lacks "not to the one below" B '^button'

clear_log A
do_input key alt+F9
check "A is an icon" "[ \"\$(get A min)\" = 1 ]"
clear_log B
do_input click left
saw "minimized: without moving, a click goes to what is left" B "^button $BTN_LEFT down"
lacks "not to the icon's client" A '^button'
refresh
do_input move $(($(get A icon_x) + 30)) $(($(get A icon_y) + 30))
settle
do_input click left
do_input click left
check "A is back" "[ \"\$(get A min)\" = 0 ]"
stop A
stop B

# ---- client menus and their grabs ------------------------------------------------------

client P popup-grab
place P 100 100
do_input move 200 200
do_input click left
saw "a press opens its menu" P '^popup-open'
sleep 0.3
do_input move 1000 600
settle
do_input click left
saw "a click on the desktop closes it" P '^popup-done'
check "and opens the root menu as well" "[ \"\$(global menu open)\" = 1 ]"
do_input key Escape
clear_log P
do_input move 200 200
do_input click left
saw "a new menu" P '^popup-open'
sleep 0.3
clear_log P
do_input move 215 215
do_input click left
lacks "a click in its own menu keeps the grab" P '^popup-done'
check "and opens nothing of ours" "[ \"\$(global menu open)\" = 0 ]"
do_input move 1000 600
settle
do_input click left
saw "the desktop again" P '^popup-done'
do_input key Escape

# Press to open, drag onto the menu, release: the way context menus get used. The menu
# lies at 111,111 and is 100x50.
settle
clear_log P
do_input move 200 200
do_input down left
saw "press, and the menu opens" P '^popup-open'
sleep 0.3
do_input move 150 130
saw "dragged onto the menu, the pointer enters it" P '^motion|^enter'
clear_log P
do_input up left
saw "the release reaches the client on the menu" P "^button $BTN_LEFT up"
lacks "and the menu stays open for it" P '^popup-done'
do_input move 1000 600
settle
do_input click left
saw "closed with a click outside" P '^popup-done'
do_input key Escape

# The same, but letting go somewhere else: that means no.
settle
clear_log P
do_input move 200 200
do_input down left
saw "press, and the menu opens" P '^popup-open'
sleep 0.3
do_input move 150 130
do_input move 1000 600
do_input up left
saw "released outside everything, the menu closes" P '^popup-done'
check "and that release opens nothing of ours" "[ \"\$(global menu open)\" = 0 ]"
stop P

# A menu closing by itself under a pointer that doesn't move: what is below gets the pointer
# right away, or the wheel goes nowhere until the next motion. The menu lies at 111,111.
client I popup-item
focus_on I
place I 100 100
do_input move 120 120
do_input click left
saw "a click opens its menu" I '^popup-open'
sleep 0.3
do_input move 150 130
sleep 0.3
do_input click left
saw "an entry is chosen, and the client closes the menu" I '^popup-closed'
after_closed() { sed -n '/^popup-closed/,$p' "$SMOKE_DIR/I.log" | grep -q "$1"; }
wait_for "without any motion, the window below has the pointer" after_closed '^enter'
do_input wheel 2
wait_for "and the wheel reaches it" after_closed '^axis'
stop I

# ---- drag and drop ----------------------------------------------------------------------

client S drag
client D drop
focus_on S
place S 100 100
focus_on D
place D 500 100

do_input move 200 200
do_input down left
saw "a press starts the drag" S '^drag-start'
sleep 0.2
do_input move 600 200
saw "over the other client, it is offered the drag" D '^dnd-enter'
pixel_is "the icon follows the pointer" 615 215 ff0000
do_input move 650 220
pixel_is "as it moves" 665 235 ff0000
do_input up left
saw "dropped there" D '^dnd-drop'
saw "the source hears of it" S '^drag-dropped'
saw "and of the end" S '^drag-finished'
check "nothing opened" "[ \"\$(global menu open)\" = 0 ]"

clear_log S
do_input move 200 200
do_input down left
saw "another drag" S '^drag-start'
sleep 0.2
do_input move 1000 600
do_input up left
saw "dropped on the desktop, it is cancelled" S '^drag-cancelled'
check "without a root menu" "[ \"\$(global menu open)\" = 0 ]"

clear_log S
do_input move 200 200
do_input down left
saw "a drag" S '^drag-start'
sleep 0.2
do_input move 1000 600
do_input down right
sleep 0.2
check "another button on the desktop opens no menu" "[ \"\$(global menu open)\" = 0 ]"
do_input up right
lacks "and doesn't end the drag" S '^drag-cancelled'
do_input up left
saw "the drag ends with its own button" S '^drag-cancelled'
check "still no menu" "[ \"\$(global menu open)\" = 0 ]"
stop S
stop D

# ---- client-initiated moves ---------------------------------------------------------------

client M move-press
place M 100 100
do_input move 200 200
do_input down left
check "a press asks for a move, with its serial: granted" "[ \"\$(global cursor mode)\" = 1 ]"
do_input move 300 260
check "the outline follows" "[ \"\$(global outline x)\" = 200 ] && [ \"\$(global outline y)\" = 160 ]"
do_input up left
check "the release ends it" "[ \"\$(global cursor mode)\" = 0 ] && [ \"\$(get M x)\" = 200 ]"
stop M

client L move-late
place L 100 100
do_input move 200 200
do_input click left
saw "the client asks too late" L '^button 272 up'
sleep 0.2
check "nothing is glued to the pointer" "[ \"\$(global cursor mode)\" = 0 ]"
stop L

# ---- keys that went to a binding ----------------------------------------------------------------

client K log
wait_for "it has the keyboard" has K '^kb-enter'
do_input key a
saw "a normal key goes down" K '^key 30 down'
saw "and up" K '^key 30 up'
clear_log K
do_input key alt+space
do_input key Escape
lacks "Alt+Space went to a binding, not down" K '^key 57 down'
lacks "not up" K '^key 57 up'
lacks "Escape closed the menu, not up" K '^key 1 up'
saw "Alt is the client's, it's a modifier" K '^key 56 up'

# Alt+Tab away and back: the window coming in is told what is down, and Tab isn't, or it
# would wait for a release it never gets.
client K2 log
focus_on K2
clear_log K
do_input key alt+Tab
saw "the window coming in is told of Alt" K '^kb-enter 56$'
lacks "and not of Tab" K '^kb-enter.* 15( |$)'
do_input key Escape
stop K2
stop K

# Caps Lock doesn't turn a letter off, while Shift still means something else.
do_input caps on
do_input key super+t
check "Super+T with Caps Lock on" "[ \"\$(global tiling on)\" = 1 ]"
do_input caps off
do_input key super+t
check "and off again" "[ \"\$(global tiling on)\" = 0 ]"
do_input key super+shift+t
check "Super+Shift+T is something else" "[ \"\$(global tiling on)\" = 0 ]"

# ---- the focus moving during a grab of the keyboard -----------------------------------------

client G popup-grab
focus_on G
place G 100 100
client H log
focus_on H
place H 500 100
focus_on G
do_input move 200 200
do_input click left
saw "a press opens its menu" G '^popup-open'
sleep 0.3
clear_log H
do_input key alt+Tab
saw "Alt+Tab closes the client's menu" G '^popup-done'
check "the other window has the keyboard" \
	"[ \"\$(get H active)\" = 1 ] && [ \"\$(get H kbfocus)\" = 1 ] && [ \"\$(get G kbfocus)\" = 0 ]"
do_input key a
saw "and hears the keys" H '^key 30 down'
stop G
stop H

client S drag
focus_on S
place S 100 100
client D drop
focus_on D
place D 500 100
focus_on S
do_input move 200 200
do_input down left
saw "a press starts the drag" S '^drag-start'
do_input key alt+Tab
check "Alt+Tab during a drag" "[ \"\$(get D active)\" = 1 ]"
do_input move 600 200
saw "doesn't end it: the other client is offered the drag" D '^dnd-enter'
do_input up left
saw "and it can be dropped there" S '^drag-dropped'
check "and the keyboard follows once it is over" \
	"[ \"\$(get D active)\" = 1 ] && [ \"\$(get D kbfocus)\" = 1 ]"
clear_log D
do_input key a
saw "the keys with it" D '^key 30 down'
stop S
stop D

# ---- more than one button -------------------------------------------------------------------

BTN_RIGHT=273
BTN_MIDDLE=274
client A2 log
focus_on A2
place A2 100 100
client B2 log
focus_on B2
place B2 500 100
focus_on A2

do_input move 200 200
clear_log A2
do_input down left
saw "A has the pointer" A2 "^button $BTN_LEFT down"
do_input move 1000 600
do_input down right
saw "a second button is A's too, over the desktop" A2 "^button $BTN_RIGHT down"
check "and opens no root menu" "[ \"\$(global menu open)\" = 0 ]"
lacks "A keeps the pointer" A2 '^leave'
do_input up right
do_input up left
saw "only the last release lets go of it" A2 '^leave'

do_input move 200 200
clear_log A2
do_input down left
do_input move 600 200
do_input down right
saw "over another window as well" A2 "^button $BTN_RIGHT down"
check "which is neither raised nor focused" \
	"[ \"\$(get A2 active)\" = 1 ] && [ \"\$(get B2 active)\" = 0 ]"
do_input up right
do_input up left

# A press nobody saw has no release to report.
do_input move 1000 600
settle
do_input down middle
do_input move 600 200
saw "over the other window" B2 '^enter'
clear_log B2
do_input up middle
lacks "the release of a press on the desktop goes nowhere" B2 "^button $BTN_MIDDLE up"
clear_log B2
do_input click left
saw "and the next click works" B2 "^button $BTN_LEFT down"

# Nor has a client that left.
focus_on A2
do_input move 200 200
clear_log A2
do_input down left
saw "A has the pointer" A2 "^button $BTN_LEFT down"
stop A2
clear_log B2
do_input move 600 210
saw "A dies, and the pointer is free" B2 '^enter|^motion'
clear_log B2
do_input up left
lacks "and the release of its press goes nowhere" B2 "^button $BTN_LEFT up"
stop B2

# Another button doesn't end a drag or set off a frame button.
WAYLAND_DISPLAY=wayland-0 build/xdgc app=titled size=300x100 >"$SMOKE_DIR/titled.log" 2>&1 &
PID_titled=$!
check "titled is mapped" "[ -n \"\$(get titled x)\" ]"
focus_on titled
place titled 100 300
TY=$((300 + B + T / 2))
do_input move 250 "$TY"
do_input down left
do_input move 300 $((TY + 40))
check "a drag on the title" "[ \"\$(global cursor mode)\" != 0 ]"
do_input click right
check "goes on past a click of another button" "[ \"\$(global cursor mode)\" != 0 ]"
do_input up left
check "and ends with its own" "[ \"\$(global cursor mode)\" = 0 ] && [ \"\$(get titled y)\" = 340 ]"
refresh
TY=$(($(get titled y) + B + T / 2))
MINX=$(($(get titled x) + B + 300 - T - T / 2))
do_input move "$MINX" "$TY"
do_input down left
do_input click right
check "a frame button waits for its own release too" "[ \"\$(get titled min)\" = 0 ]"
do_input up left
check "and then works" "[ \"\$(get titled min)\" = 1 ]"
stop titled

# ---- menus a client asks for ----------------------------------------------------------------

client N menu-press
focus_on N
place N 100 100
do_input move 200 200
do_input click left
check "a click may ask for the window menu" "[ \"\$(global menu open)\" = 1 ]"
check "and it opens where the client said" \
	"[ \"\$(global menu x)\" = 140 ] && [ \"\$(global menu y)\" = 150 ]"
do_input key Escape
stop N

client O menu-bad
focus_on O
place O 100 100
do_input move 200 200
do_input click left
saw "a client with a made-up serial" O "^button $BTN_LEFT down"
check "gets no menu" "[ \"\$(global menu open)\" = 0 ]"
clear_log O
do_input key a
saw "and keeps the keyboard" O '^key 30 down'
stop O

client Q menu-leave
focus_on Q
place Q 100 100
do_input move 200 200
do_input click left
client R log
saw "a client losing the focus" Q '^kb-leave'
check "gets no menu with an old serial" "[ \"\$(global menu open)\" = 0 ]"
stop Q
stop R

# A monitor plugged in and out through the test hook, one at a time.
outputs_now() { grep -o '^output_[^ ]*' "$MWC_TEST_STATE"; }
plug_in() {
	refresh
	local before
	before=$(outputs_now | sort)
	echo + >"$MWC_TEST_STATE.unplug"
	kill -USR2 "$pid"
	check "a monitor is plugged in" "[ \$(outputs_now | wc -l) -gt $(wc -l <<<"$before") ]"
	PLUGGED=$(comm -13 <(echo "$before") <(outputs_now | sort))
	PLUGGED=${PLUGGED#output_}
	[ -n "$PLUGGED" ] && [ "$PLUGGED" != HEADLESS-1 ] || fail "which monitor is new: '$PLUGGED'"
}
unplug_again() {
	echo "$PLUGGED" >"$MWC_TEST_STATE.unplug"
	kill -USR2 "$pid"
	check "and unplugged again" "! grep -q '^output_$PLUGGED ' \"\$MWC_TEST_STATE\""
}
# The pointer's image, as a capture of it sees it: its hotspot and size.
image() {
	WAYLAND_DISPLAY=wayland-0 build/capturec cursor HEADLESS-1 "$SMOKE_DIR/image.png" \
		>"$SMOKE_DIR/image.txt" || fail "capturing the pointer"
	local _ x y hotspot w h opaque
	read -r _ x y hotspot w h opaque <"$SMOKE_DIR/image.txt"
	echo "$hotspot $w $h"
}

# A client's own cursor: the state shows its buffer and scale, for the tools that
# load a cursor theme at their output's scale. 64 at scale 3 is what Adwaita gives
# for 24 at 3; wlroots only logs that it doesn't divide.
WAYLAND_DISPLAY=wayland-0 build/xdgc app=curs cursor=64x64@3 >"$SMOKE_DIR/curs.log" 2>&1 &
PID_curs=$!
check "curs is mapped" "[ -n \"\$(get curs x)\" ]"
do_input move $(($(get curs x) + 150)) $(($(get curs y) + 100))
check "its cursor is in the state" \
	"[ \"\$(global cursor cw)x\$(global cursor ch)@\$(global cursor cscale)\" = 64x64@3 ]"
plug_in
refresh
[ "$(global cursor cw)" = 64 ] || fail "a monitor plugged in replaced it: cw=$(global cursor cw)"
echo "ok: a monitor plugged in leaves it"
unplug_again
do_input move 5 5
check "and gone over the desktop" "[ \"\$(global cursor cw)\" = 0 ]"
stop curs

# Or one by name, cursor-shape-v1: mwc takes it from the theme, only from the client
# under the pointer and not while mwc moves a window; its own again over the desktop.
DEFAULT=$(image)
mkfifo "$SMOKE_DIR/shaped.ctl"
exec {shapedfd}<>"$SMOKE_DIR/shaped.ctl"
WAYLAND_DISPLAY=wayland-0 build/xdgc app=shaped shape=text control="$SMOKE_DIR/shaped.ctl" \
	>"$SMOKE_DIR/shaped.log" 2>&1 &
PID_shaped=$!
check "shaped is mapped" "[ -n \"\$(get shaped x)\" ]"
SX=$(($(get shaped x) + 150)) SY=$(($(get shaped y) + 100))
do_input move "$SX" "$SY"
check "over it, the shape it names" "[ \"\$(global cursor shape)\" = text ]"
TEXT=$(image)
[ "$TEXT" != "$DEFAULT" ] || fail "the image is still mwc's: $TEXT"
echo "ok: and the image is the theme's for it ($TEXT, mwc's $DEFAULT)"
dids() { grep -c '^did' "$SMOKE_DIR/shaped.log"; }
ask() { # shape: shaped asks for it now, and mwc has had the request
	local n
	n=$(dids)
	echo "shape $1" >&$shapedfd
	wait_for "shaped asks for $1" eval '[ "$(dids)" -gt "$n" ]'
	refresh
}
ask crosshair
[ "$(global cursor shape)" = crosshair ] || fail "a new shape isn't taken: $(global cursor shape)"
echo "ok: a new one while the pointer is on it"
ask text
plug_in
refresh
[ "$(global cursor shape)" = text ] || fail "a monitor plugged in replaced it: $(global cursor shape)"
echo "ok: a monitor plugged in leaves it"
unplug_again
refresh
[ "$(global cursor shape)" = text ] || fail "a monitor unplugged replaced it: $(global cursor shape)"
echo "ok: and one unplugged"
WAYLAND_DISPLAY=wayland-0 build/powerc off || fail "powerc off"
WAYLAND_DISPLAY=wayland-0 build/powerc on || fail "powerc on"
check "the monitor is off and on again" "[ \"\$(global output on)\" = 1 ]"
[ "$(image)" = "$TEXT" ] || fail "after the monitor was off the image is $(image), not $TEXT"
echo "ok: after the monitor was off it is the shape's again"
do_input hold alt
do_input down left
do_input move $((SX + 20)) $((SY + 20))
check "an Alt+drag moves it" "[ \"\$(global cursor mode)\" = 1 ]"
ask crosshair
[ "$(global cursor shape)" != crosshair ] || fail "a shape is taken during a move"
echo "ok: not during a move"
do_input up left
do_input release alt
WAYLAND_DISPLAY=wayland-0 build/xdgc app=other size=200x150 >"$SMOKE_DIR/other.log" 2>&1 &
PID_other=$!
check "other is mapped" "[ -n \"\$(get other x)\" ]"
do_input move $(($(get other x) + 100)) $(($(get other y) + 80))
ask wait
[ "$(global cursor shape)" != wait ] || fail "a client not under the pointer set it, over another"
echo "ok: not while another client has the pointer"
kill "$PID_other"
wait "$PID_other" 2>/dev/null
do_input move 5 5
check "over the desktop, mwc's own" \
	"[ \"\$(global cursor name)\" = default ] && [ \"\$(global cursor shape)\" = none ]"
ask crosshair
[ "$(global cursor name)" = default ] && [ "$(global cursor shape)" = none ] ||
	fail "a client not under the pointer set it: $(global cursor name) $(global cursor shape)"
echo "ok: nor over the desktop"
stop shaped
exec {shapedfd}>&-

stop_vinput
stop_mwc
echo "PASS: pointer"
