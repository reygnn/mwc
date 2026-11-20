#!/bin/bash
# Headless test of the icon box (Mwm*useIconBox): the logo while it is empty, growing
# with its icons up to iconBoxSize, scrolling, maximize and minimize, the keyboard,
# Alt+Tab, desktops, tiling and switching it off by a reload.
# Output goes to build/iconbox.log, a screenshot of the box to build/iconbox.png.
# Exits non-zero on failure.
LOG=build/iconbox.log
. "$(dirname "$0")/lib.sh"

require_tools grim
for tool in build/vinput build/pngtool build/xdgc build/workspacec build/layerc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
cat >"$RC" <<'EOF'
Mwm*useIconBox: true
Mwm*iconBoxSize: 3
EOF

export MWC_TEST_STATE=$SMOKE_DIR/state
start_mwc 'exec sleep 300'
start_vinput
refresh
T=$(global theme title)
B=3 # bevel 2 + 1: a frame without resize handles
P=4 # half the gap between icons
ICON=72

box() { global iconbox "$1"; }
wayland() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$name" "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "PID_$name=$!"
}
mapped() { check "$1 is mapped" "[ -n \"\$(get $1 x)\" ]"; }
ws() { WAYLAND_DISPLAY=wayland-0 build/workspacec "$@"; }
# Focus a window by a click on its title bar, then minimize it with Alt+F9.
minimize() {
	refresh
	do_input move $(($(get "$1" x) + 60)) $(($(get "$1" y) + B + 5))
	do_input click left
	check "$1 has the focus" "[ \"\$(get $1 active)\" = 1 ]"
	do_input key alt+F9
	check "$1 is an icon" "[ \"\$(get $1 min)\" = 1 ]"
}
click_at() { do_input move "$1" "$2"; do_input click left; }
# The middle of the box's minimize and maximize buttons, and of its title.
min_button() { refresh; echo $(($(box x) + $(box w) - B - T - T / 2)) $(($(box y) + B + T / 2)); }
max_button() { refresh; echo $(($(box x) + $(box w) - B - T / 2)) $(($(box y) + B + T / 2)); }
title_bar() { refresh; echo $(($(box x) + 20)) $(($(box y) + B + T / 2)); }

# ---- empty: the logo --------------------------------------------------------------

check "empty: the logo, bottom left as the first icon" \
	"[ \"\$(box logo)\" = 1 ] && [ \"\$(box shown)\" = 0 ] && [ \"\$(box x)\" = 8 ] && \
	 [ \"\$(box y)\" = $((720 - 8 - ICON)) ] && [ \"\$(box w)\" = $ICON ]"

# ---- growing --------------------------------------------------------------------

for app in one two three four; do
	wayland $app size=300x200
	mapped $app
done
minimize one
check "one icon: the box with its frame, bottom left" \
	"[ \"\$(box shown)\" = 1 ] && [ \"\$(box logo)\" = 0 ] && [ \"\$(box n)\" = 1 ] && \
	 [ \"\$(box x)\" = 8 ] && [ \$((\$(box y) + \$(box h))) = $((720 - 8)) ] && \
	 [ \"\$(box w)\" = $((2 * B + 2 * P + ICON)) ]"
check "the icon is in the box" \
	"[ \"\$(get one icon_shown)\" = 1 ] && [ \"\$(get one icon_x)\" = $((8 + B + P)) ] && \
	 [ \"\$(get one icon_y)\" = \$((\$(box y) + B + T + P)) ]"
minimize two
check "two icons: one slot longer" "[ \"\$(box w)\" = $((2 * B + 2 * P + 2 * ICON + 8)) ]"
check "the second icon right of the first" \
	"[ \"\$(get two icon_x)\" = $((8 + B + P + ICON + 8)) ]"
minimize three
H3=$(box h)
minimize four
check "four icons, room for three: the box scrolls" \
	"[ \"\$(box n)\" = 4 ] && [ \"\$(box visible)\" = 3 ] && \
	 [ \"\$(box w)\" = $((2 * B + 2 * P + 3 * ICON + 16)) ] && [ \"\$(box h)\" -gt $H3 ]"
check "the fourth icon is out of sight" \
	"[ \"\$(get four icon_shown)\" = 0 ] && [ \"\$(get one icon_shown)\" = 1 ]"
WAYLAND_DISPLAY=wayland-0 grim "build/iconbox.png" 2>/dev/null

# ---- scrolling --------------------------------------------------------------------

refresh
do_input move $(($(box x) + 40)) $(($(box y) + B + T + 30))
do_input wheel 1
check "the wheel scrolls by one icon" \
	"[ \"\$(box scroll)\" = 1 ] && [ \"\$(get one icon_shown)\" = 0 ] && \
	 [ \"\$(get four icon_shown)\" = 1 ]"
do_input wheel 1
check "not past the end" "[ \"\$(box scroll)\" = 1 ]"
do_input wheel -1
check "and back" "[ \"\$(box scroll)\" = 0 ]"
# The scroll bar along the bottom: an arrow at each end, the trough between.
SB=15
refresh
SBY=$(($(box y) + $(box h) - B - SB / 2))
click_at $(($(box x) + $(box w) - B - SB / 2)) $SBY
check "its right arrow scrolls on" "[ \"\$(box scroll)\" = 1 ]"
click_at $(($(box x) + B + SB / 2)) $SBY
check "its left arrow back" "[ \"\$(box scroll)\" = 0 ]"
click_at $(($(box x) + $(box w) - B - SB - 6)) $SBY
check "a click in the trough beyond the slider scrolls a page" "[ \"\$(box scroll)\" = 1 ]"
click_at $(($(box x) + B + SB + 6)) $SBY
check "and before it, back" "[ \"\$(box scroll)\" = 0 ]"

# ---- the keyboard -------------------------------------------------------------------

click_at $(title_bar)
check "a click on the box gives it the keyboard" \
	"[ \"\$(box focused)\" = 1 ] && [ \"\$(global focus any)\" = 0 ]"
# A panel that takes no keyboard leaves it with the box when clicked.
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top anchor=top size=100x30 keyboard=none \
	color=444444 >"$SMOKE_DIR/panel.events" 2>&1 &
PANEL_PID=$!
wait_for "a panel is there" grep -q configured "$SMOKE_DIR/panel.events"
sleep 0.3
click_at 640 15
check "a click on it leaves the keyboard with the box" "[ \"\$(box focused)\" = 1 ]"
kill $PANEL_PID; wait $PANEL_PID 2>/dev/null
do_input key Right
check "Right chooses the next icon" "[ \"\$(box sel)\" = 1 ]"
do_input key Right
do_input key Right
check "and scrolls it into sight" "[ \"\$(box sel)\" = 3 ] && [ \"\$(box scroll)\" = 1 ]"
do_input key Left
do_input key Left
do_input key Return
check "Enter restores the chosen window" \
	"[ \"\$(get two min)\" = 0 ] && [ \"\$(get two active)\" = 1 ] && [ \"\$(box focused)\" = 0 ]"
check "the box closes up" "[ \"\$(box n)\" = 3 ] && [ \"\$(box visible)\" = 3 ]"

# ---- Alt+Tab ------------------------------------------------------------------------

do_input key alt+Tab
check "Alt+Tab reaches the box" "[ \"\$(box focused)\" = 1 ] && [ \"\$(get two active)\" = 0 ]"
do_input key alt+Tab
check "and goes on to the window" "[ \"\$(box focused)\" = 0 ] && [ \"\$(get two active)\" = 1 ]"

# With more windows, the box has its place among them: Alt+Tab brings the bottom one to
# the front, the box included, and goes round.
for app in pp qq; do
	wayland $app size=200x150
	mapped $app
done
check "the newest has the focus" "[ \"\$(get qq active)\" = 1 ]"
focused_one() {
	if [ "$(box focused)" = 1 ]; then echo box; return; fi
	local a
	for a in two pp qq; do [ "$(get $a active)" = 1 ] && echo $a; done
}
order=
for i in 1 2 3 4 5; do
	do_input key alt+Tab
	refresh
	order="$order $(focused_one)"
done
# The box was focused before the three windows came up, so it is the bottom one.
[ "$order" = " box two pp qq box" ] || fail "Alt+Tab went$order, expected box two pp qq box"
echo "ok: Alt+Tab goes round the windows and the box:$order"
do_input key alt+shift+Tab
check "Alt+Shift+Tab sends it to the back, the top window gets the focus" \
	"[ \"\$(box focused)\" = 0 ] && [ \"\$(get qq active)\" = 1 ]"
for app in pp qq; do
	var="PID_$app"
	kill "${!var}"
	wait "${!var}" 2>/dev/null
done
check "the extra windows are gone" "[ -z \"\$(get qq x)\" ] && [ -z \"\$(get pp x)\" ]"

# ---- maximize and minimize ------------------------------------------------------------

# Two icons left, so that maximize has room to grow into. A click on an icon chooses it
# and gives the box the keyboard.
for app in one two three four; do
	[ "$(get $app icon_shown)" = 1 ] && break
done
click_at $(($(get $app icon_x) + 30)) $(($(get $app icon_y) + 30))
check "a click on an icon chooses it" \
	"[ \"\$(box focused)\" = 1 ] && [ \"\$(get $app min)\" = 1 ]"
do_input key Return
check "two icons left" "[ \"\$(box n)\" = 2 ] && [ \"\$(box visible)\" = 2 ] && \
	[ \"\$(get $app min)\" = 0 ]"
W2=$(box w)
W3=$((2 * B + 2 * P + 3 * ICON + 16))
click_at $(max_button)
check "maximize: the full length, empty slots included" \
	"[ \"\$(box max)\" = 1 ] && [ \"\$(box visible)\" = 3 ] && [ \"\$(box w)\" = $W3 ]"
click_at $(max_button)
check "and back to what the icons need" "[ \"\$(box max)\" = 0 ] && [ \"\$(box w)\" = $W2 ]"
click_at $(min_button)
check "minimize: the logo, icons hidden" \
	"[ \"\$(box logo)\" = 1 ] && [ \"\$(box min)\" = 1 ] && [ \"\$(box w)\" = $ICON ]"
for app in one two three four; do
	[ "$(get $app min)" = 1 ] && [ "$(get $app icon_shown)" = 1 ] && fail "$app's icon shows"
done
click_at $((8 + 30)) $((720 - 8 - 30))
check "a click on the logo shows the box" "[ \"\$(box shown)\" = 1 ] && [ \"\$(box min)\" = 0 ]"

# ---- desktops -------------------------------------------------------------------------

click_at $(max_button)
check "maximized" "[ \"\$(box max)\" = 1 ]"
# The chosen icon's label is highlighted while the box has the keyboard; leaving the
# desktop takes that away, also when coming back.
label_px() { # app: the middle of its icon's label
	refresh
	WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/label.png" || fail "a screenshot"
	build/pngtool pixel "$SMOKE_DIR/label.png" $(($(get "$1" icon_x) + 5)) \
		$(($(get "$1" icon_y) + ICON - 4))
}
chosen= other=
for app in one two three four; do
	[ "$(get $app icon_shown)" = 1 ] || continue
	if [ "$(get $app icon_x)" = "$(($(box x) + B + P + $(box sel) * (ICON + 8)))" ]; then
		chosen=$app
	else
		other=$app
	fi
done
[ -n "$chosen" ] && [ -n "$other" ] || fail "no chosen and other icon in sight"
check "the box has the keyboard" "[ \"\$(box focused)\" = 1 ]"
lit=$(label_px $chosen)
[ "$lit" != "$(label_px $other)" ] || fail "the chosen label looks like the others: $lit"
echo "ok: the chosen icon's label is highlighted ($lit)"
ws activate 2 >/dev/null || fail "activating desktop 2"
check "another desktop: its own, empty box" "[ \"\$(box logo)\" = 1 ] && [ \"\$(box n)\" = 0 ]"
ws activate 1 >/dev/null || fail "activating desktop 1"
check "back: the icons, maximize forgotten" \
	"[ \"\$(box shown)\" = 1 ] && [ \"\$(box n)\" = 2 ] && [ \"\$(box max)\" = 0 ]"
[ "$(label_px $chosen)" = "$(label_px $other)" ] ||
	fail "the label is still highlighted after the desktop switch"
echo "ok: the highlight went with the desktop switch"

# A button held down while the desktop changes: its release does nothing.
do_input move $(min_button)
do_input down left
check "minimize held down" "[ \"\$(global cursor mode)\" != 0 ]"
ws activate 2 >/dev/null || fail "activating desktop 2"
check "the desktop switch lets go of it" "[ \"\$(global cursor mode)\" = 0 ]"
ws activate 1 >/dev/null || fail "activating desktop 1"
do_input up left
check "and the release doesn't minimize the box" \
	"[ \"\$(box shown)\" = 1 ] && [ \"\$(box min)\" = 0 ]"

# ---- tiling ----------------------------------------------------------------------------

do_input key super+t
# Every window that isn't an icon ends above the box.
above_box() {
	local a
	for a in one two three four; do
		[ "$(get $a min)" = 1 ] && continue
		[ $(($(get $a y) + $(get $a fh))) -le "$(box y)" ] || return 1
	done
}
check "tiling: the windows end above the box" "[ \"\$(global tiling on)\" = 1 ] && above_box"
do_input key super+t

# ---- reload ----------------------------------------------------------------------------

# Switched off while it has the keyboard: a window gets it.
click_at $(title_bar)
check "the box has the keyboard" "[ \"\$(box focused)\" = 1 ]"
sed -i 's/useIconBox: true/useIconBox: false/' "$RC"
kill -HUP "$pid"
check "switched off with the keyboard, a window gets it" "[ \"\$(global focus any)\" = 1 ]"
# Every icon is shown.
icons_shown() {
	local a
	for a in one two three four; do
		[ "$(get $a min)" = 1 ] || continue
		[ "$(get $a icon_shown)" = 1 ] || return 1
	done
}
check "switched off: no box, the icons on the desktop again" \
	"[ \"\$(box on)\" = 0 ] && [ \"\$(box shown)\" = 0 ] && [ \"\$(box logo)\" = 0 ] && icons_shown"
sed -i 's/useIconBox: false/useIconBox: true/' "$RC"
kill -HUP "$pid"
check "switched on again: the icons go into the box" \
	"[ \"\$(box shown)\" = 1 ] && [ \"\$(box n)\" = 2 ]"

# Bad values are reported once, not with every arrange, and the default holds.
sed -i 's/iconBoxSize: 3/iconBoxSize: 0/' "$RC"
echo 'Mwm*badone*allDesktops: maybe' >>"$RC"
echo 'Mwm*badtwo*allDesktops: kind of' >>"$RC"
reload_mwc
for d in 2 1 2 1; do
	ws activate $d >/dev/null || fail "activating desktop $d"
	check "desktop $d" "[ \"\$(global desktop current)\" = $d ]"
done
# Two windows with bad values, mapped by turns.
for name in badone badtwo badone badtwo; do
	wayland $name
	mapped $name
	kill "$(eval echo \$PID_$name)"
	wait "$(eval echo \$PID_$name)" 2>/dev/null
	check "$name is gone" "[ -z \"\$(get $name x)\" ]"
done
n=$(grep -c 'iconBoxSize: expected' "$LOG")
[ "$n" = 1 ] || fail "a bad iconBoxSize was reported $n times"
for bad in 'allDesktops value maybe' 'allDesktops value kind of'; do
	n=$(grep -c "ignoring $bad," "$LOG")
	[ "$n" = 1 ] || fail "'$bad' was reported $n times"
done
echo "ok: bad values are reported once each"
sed -i -e 's/iconBoxSize: 0/iconBoxSize: 3/' -e '/maybe\|kind of/d' "$RC"
reload_mwc
check "back to three" "[ \"\$(box shown)\" = 1 ] && [ \"\$(box n)\" = 2 ]"

# ---- from the right ------------------------------------------------------------------

# "right bottom": the box at the bottom right, its first icon on the right, so going on
# through the icons is to the left.
echo 'Mwm*iconPlacement: right bottom' >>"$RC"
reload_mwc
check "the box stands at the bottom right" \
	"[ \"\$(box shown)\" = 1 ] && [ \$((\$(box x) + \$(box w))) = $((1280 - 8)) ]"
click_at $(title_bar)
check "it has the keyboard" "[ \"\$(box focused)\" = 1 ]"
first=$(box sel)
do_input key Left
check "Left goes on to the next icon" "[ \"\$(box sel)\" = $((first + 1)) ]"
do_input key Right
check "Right back" "[ \"\$(box sel)\" = $first ]"

# ---- a column with a large font -------------------------------------------------------

cat >>"$RC" <<'EOF2'
Mwm*iconPlacement: top right
Mwm*fontList: Sans Bold 26
EOF2
kill -HUP "$pid"
check "a large font" "[ \"\$(global theme title)\" -ge 40 ]"
T=$(global theme title)
check "the column is wide enough for its buttons and a title" \
	"[ \"\$(box shown)\" = 1 ] && [ \"\$(box w)\" -ge $((2 * B + 3 * T)) ]"
refresh
click_at $(title_bar)
click_at $(title_bar)
check "its title bar takes a double click" "[ \"\$(box max)\" = 1 ]"
do_input key Up
check "in a column, Up chooses the icon above" "[ \"\$(box sel)\" = 0 ]"
do_input key Down
check "and Down the one below" "[ \"\$(box sel)\" = 1 ]"
# A double click on an icon in the box brings its window back, as on the desktop.
for app in one two three four; do
	[ "$(get $app min)" = 1 ] && [ "$(get $app icon_shown)" = 1 ] && break
done
do_input move $(($(get $app icon_x) + 30)) $(($(get $app icon_y) + 30))
do_input click left
do_input click left
check "a double click on an icon restores its window" \
	"[ \"\$(get $app min)\" = 0 ] && [ \"\$(box n)\" = 1 ]"

# ---- the box has the keyboard when its last icon goes ------------------------------------

click_at $(title_bar)
check "the box has the keyboard" "[ \"\$(box focused)\" = 1 ] && [ \"\$(global focus any)\" = 0 ]"
# A prompt takes the keyboard and goes again: it comes back to the box, as to a window.
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top size=200x100 keyboard=exclusive \
	color=00ff00 >"$SMOKE_DIR/prompt.events" 2>&1 &
PID_prompt=$!
wait_for "a prompt has the keyboard" grep -q keyboard-enter "$SMOKE_DIR/prompt.events"
check "the box doesn't" "[ \"\$(box focused)\" = 0 ]"
kill "$PID_prompt"
wait "$PID_prompt" 2>/dev/null
check "the prompt gone, the box has it again" "[ \"\$(box focused)\" = 1 ]"
# Unless a window was given the keys meanwhile: a panel takes them from the box, a window
# is clicked, the panel goes. With that window gone too, a prompt that comes and goes
# leaves the keys with nobody.
shown=
for w in one two three four; do
	refresh
	[ "$(get $w min)" = 0 ] || continue
	minimize $w
	check "$w is an icon" "[ \"\$(get $w min)\" = 1 ]"
	shown="$shown $w"
done
wayland later size=300x200
mapped later
click_at $(title_bar)
check "the box has the keyboard" "[ \"\$(box focused)\" = 1 ]"
rm -f "$SMOKE_DIR/panel.events" "$SMOKE_DIR/prompt.events"
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top size=200x100 keyboard=on-demand \
	color=00ff00 >"$SMOKE_DIR/panel.events" 2>&1 &
PID_panel=$!
wait_for "an on-demand panel has the keyboard" grep -q keyboard-enter "$SMOKE_DIR/panel.events"
refresh
click_at $(($(get later x) + 60)) $(($(get later y) + B + 5))
check "a click gives it to a window" "[ \"\$(get later kbfocus)\" = 1 ]"
kill "$PID_panel"
wait "$PID_panel" 2>/dev/null
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top size=200x100 keyboard=exclusive \
	color=00ff00 >"$SMOKE_DIR/prompt.events" 2>&1 &
PID_prompt=$!
wait_for "a prompt has the keyboard" grep -q keyboard-enter "$SMOKE_DIR/prompt.events"
kill "$PID_later"
wait "$PID_later" 2>/dev/null
check "the window is gone" "[ -z \"\$(get later x)\" ] && [ \"\$(global focus any)\" = 0 ]"
kill "$PID_prompt"
wait "$PID_prompt" 2>/dev/null
check "the prompt is gone" "[ \"\$(global focus layer)\" = 0 ]"
[ "$(box focused)" = 0 ] || fail "the box has the keyboard, which it gave away to the panel"
echo "ok: the box doesn't get it"
for w in $shown; do
	do_input move $(($(get $w icon_x) + 30)) $(($(get $w icon_y) + 30))
	do_input click left
	do_input click left
	check "$w is back" "[ \"\$(get $w min)\" = 0 ]"
done
click_at $(title_bar)
check "a click gives the box the keyboard again" "[ \"\$(box focused)\" = 1 ]"
# The box chosen while a prompt has the keys gets them when it goes, also with a second
# prompt in between.
click_at $(($(get one x) + 60)) $(($(get one y) + B + 5))
check "a window has the keyboard" "[ \"\$(get one kbfocus)\" = 1 ]"
rm -f "$SMOKE_DIR/prompt.events" "$SMOKE_DIR/prompt2.events"
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top size=200x100 keyboard=exclusive \
	color=00ff00 >"$SMOKE_DIR/prompt.events" 2>&1 &
PID_prompt=$!
wait_for "a prompt has the keyboard" grep -q keyboard-enter "$SMOKE_DIR/prompt.events"
click_at $(title_bar)
check "the box is chosen under it" "[ \"\$(box focused)\" = 1 ]"
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top size=200x100 keyboard=exclusive \
	color=00ff00 >"$SMOKE_DIR/prompt2.events" 2>&1 &
PID_prompt2=$!
wait_for "a second prompt has it" grep -q keyboard-enter "$SMOKE_DIR/prompt2.events"
kill "$PID_prompt2"
wait "$PID_prompt2" 2>/dev/null
wait_for "back to the first" eval \
	"[ \"\$(grep -c keyboard-enter \"\$SMOKE_DIR/prompt.events\")\" -ge 2 ]"
kill "$PID_prompt"
wait "$PID_prompt" 2>/dev/null
check "the prompts are gone" "[ \"\$(global focus layer)\" = 0 ]"
check "the box has the keyboard" "[ \"\$(box focused)\" = 1 ]"
# One taken from the box on desktop 1 doesn't give the box of desktop 2 the keys.
ws activate 2 >/dev/null || fail "activating desktop 2"
check "desktop 2" "[ \"\$(global desktop current)\" = 2 ]"
wayland away size=300x200
mapped away
minimize away
check "an icon there" "[ \"\$(get away min)\" = 1 ] && [ \"\$(box n)\" = 1 ]"
ws activate 1 >/dev/null || fail "activating desktop 1"
check "desktop 1" "[ \"\$(global desktop current)\" = 1 ]"
click_at $(title_bar)
check "the box has the keyboard" "[ \"\$(box focused)\" = 1 ]"
rm -f "$SMOKE_DIR/panel.events"
WAYLAND_DISPLAY=wayland-0 build/layerc layer=top size=200x100 keyboard=on-demand \
	color=00ff00 >"$SMOKE_DIR/panel.events" 2>&1 &
PID_panel=$!
wait_for "an on-demand panel has the keyboard" grep -q keyboard-enter "$SMOKE_DIR/panel.events"
ws activate 2 >/dev/null || fail "activating desktop 2"
check "desktop 2" "[ \"\$(global desktop current)\" = 2 ]"
kill "$PID_panel"
wait "$PID_panel" 2>/dev/null
check "the panel is gone" "[ \"\$(global focus layer)\" = 0 ]"
[ "$(box focused)" = 0 ] || fail "the box of desktop 2 has the keyboard of desktop 1's"
echo "ok: the box there doesn't get it"
kill "$PID_away"
wait "$PID_away" 2>/dev/null
ws activate 1 >/dev/null || fail "activating desktop 1"
check "desktop 1" "[ \"\$(global desktop current)\" = 1 ]"
click_at $(title_bar)
check "the box has the keyboard" "[ \"\$(box focused)\" = 1 ]"
for app in one two three four; do
	[ "$(get $app min)" = 1 ] || continue
	var="PID_$app"
	kill "${!var}"
done
check "its programs quit: the logo, and a window has the keyboard" \
	"[ \"\$(box logo)\" = 1 ] && [ \"\$(box focused)\" = 0 ] && [ \"\$(global focus any)\" = 1 ]"

stop_vinput
stop_mwc
echo "PASS"
