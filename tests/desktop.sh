#!/bin/bash
# Headless test of the virtual desktops: switching through ext-workspace-v1 as a
# tool does, moving windows through the window menu, what goes along (dialogs, tab
# groups) and what stays everywhere (panels and their dialogs), icons and tiling per
# desktop, a window on another desktop asking for attention, and the workspace group
# following its output.
# Output goes to build/desktop.log. Exits non-zero on failure.
LOG=build/desktop.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland
for tool in build/vinput build/xclient build/xdgc build/workspacec build/outputc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
mkdir -p "$XDG_CONFIG_HOME/mwc"
echo 'Mwm*outputManagement: true' >"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
wait_for "the startup command ran" test -s "$SMOKE_DIR/display"
X_DISPLAY=$(cat "$SMOKE_DIR/display")
start_vinput
refresh

x11() {
	local name=$1
	shift
	DISPLAY=$X_DISPLAY build/xclient class="$name" "$@" >"$SMOKE_DIR/$name.log" 2>&1 &
	eval "PID_$name=$!"
	check "$name is mapped" "[ -n \"\$(get $name x)\" ]"
}
wayland() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$name" "$@" >"$SMOKE_DIR/$name.log" 2>&1 &
	eval "PID_$name=$!"
	check "$name is mapped" "[ -n \"\$(get $name x)\" ]"
}
stop() {
	local var="PID_$1"
	kill "${!var}" 2>/dev/null
	wait "${!var}" 2>/dev/null
	check "$1 is gone" "[ -z \"\$(get $1 x)\" ]"
}
said() { grep -q "^$2" "$SMOKE_DIR/$1.log"; }
ws() { WAYLAND_DISPLAY=wayland-0 build/workspacec "$@"; }
switch_to() {
	ws activate "$1" >/dev/null || fail "activating desktop $1"
	check "desktop $1 is current" "[ \"\$(global desktop current)\" = $1 ]"
}
on() { # description app desktop shown(0/1)
	check "$1" "[ \"\$(get $2 desktop)\" = $3 ] && [ \"\$(get $2 visible)\" = $4 ]"
}
# urgent_is <description> <ids>: which workspaces say urgent, e.g. "" or "2".
urgent_is() {
	local out got
	out=$(ws list) || fail "workspacec list: $out"
	got=$(sed -n 's/^workspace id=\([0-9]*\) .*state=[a-z+]*urgent.*/\1/p' <<<"$out" | tr -d '\n')
	[ "$got" = "$2" ] || fail "$1: urgent are '$got', expected '$2': $out"
	echo "ok: $1"
}
to_desktop() { # app n: through its window menu
	local others
	do_input key alt+space
	check "the window menu of $1 is open" "[ \"\$(global menu open)\" = 1 ]"
	others=$(tr -d "$(get "$1" desktop)" <<<1234)
	[ "$(global menu sensitive | tr -cd 1234)" = "$others" ] ||
		fail "the menu should offer desktops $others: $(global menu sensitive)"
	do_input key "$2"
	check "$1 went to desktop $2" "[ \"\$(get $1 desktop)\" = $2 ]"
}

# ---- what a tool sees ---------------------------------------------------------------

out=$(ws list) || fail "workspacec list: $out"
echo "$out" >>"$LOG"
[ "$(grep -c '^workspace ' <<<"$out")" = 4 ] || fail "not four workspaces: $out"
grep -q '^workspace id=1 name=1 x=0 state=active caps=activate group=1$' <<<"$out" ||
	fail "desktop 1 isn't the active one: $out"
grep -q '^workspace id=4 name=4 x=3 state=none caps=activate group=1$' <<<"$out" ||
	fail "desktop 4: $out"
grep -q '^group outputs=1 caps=0$' <<<"$out" || fail "the group: $out"
echo "ok: four desktops in one group for the output, the first one active"

# ---- switching -------------------------------------------------------------------------

wayland a size=300x200
on "a window opens on the current desktop" a 1 1
switch_to 2
on "switched away: it is out of sight" a 1 0
check "and nothing has the focus" "[ \"\$(global focus any)\" = 0 ]"
wayland b size=300x200
on "a window opened now is on desktop 2" b 2 1
check "and has the focus" "[ \"\$(get b active)\" = 1 ]"
do_input key alt+Tab
check "Alt+Tab doesn't reach the other desktop" "[ \"\$(get b active)\" = 1 ] && [ \"\$(get a visible)\" = 0 ]"
urgent_is "nor does it ask for attention there" ""
# Right after a switch the other desktop's windows are next in the stacking order: a
# group can't take them.
do_input key super+g
check "Super+G doesn't make a tab of a window on another desktop" \
	"[ \"\$(get b tab)\" = -1 ] && [ \"\$(get a tab)\" = -1 ] && [ \"\$(get a desktop)\" = 1 ]"
switch_to 1
on "back on desktop 1: it is there again" a 1 1
on "and the other one isn't" b 2 0
check "the window there gets the focus" "[ \"\$(get a active)\" = 1 ] && [ \"\$(get a kbfocus)\" = 1 ]"
out=$(ws list) || fail "workspacec list: $out"
grep -q '^workspace id=1 name=1 x=0 state=active' <<<"$out" &&
	grep -q '^workspace id=2 name=2 x=1 state=none' <<<"$out" ||
	fail "the tool doesn't see the switch: $out"
echo "ok: the tool sees which one is current"

# A switch during a move ends the move.
ax=$(($(get a x) + 100)) ay=$(($(get a y) + $(get a th) + 50))
do_input hold alt
do_input move $ax $ay
do_input down left
do_input move $((ax + 40)) $((ay + 30))
check "a is being moved" "[ \"\$(global cursor mode)\" = 1 ]"
switch_to 3
check "a switch ends the move" "[ \"\$(global cursor mode)\" = 0 ] && [ \"\$(global outline shown)\" = 0 ]"
do_input up left
do_input release alt
switch_to 1

# ---- moving a window -----------------------------------------------------------------

to_desktop a 3
on "moved: it is out of sight" a 3 0
check "and has lost the focus" "[ \"\$(global focus any)\" = 0 ]"
switch_to 3
on "it is on desktop 3" a 3 1
stop a
stop b

# A dialog goes along with its window, and a tab group as a whole.
x11 parent size=300x200 dialog=kid
check "the dialog is there" "[ -n \"\$(get kid x)\" ]"
do_input move $(($(get parent x) + 30)) $(($(get parent y) + $(get parent th) + 30))
do_input click left
check "the window has the focus" "[ \"\$(get parent active)\" = 1 ]"
to_desktop parent 4
on "its dialog went along" kid 4 0
stop parent
x11 g1 size=300x200
x11 g2 size=300x200
do_input key super+g
check "a tab group" "[ \"\$(get g1 tab)\" != -1 ] && [ \"\$(get g2 tab)\" != -1 ]"
to_desktop g2 1
check "the whole group went" "[ \"\$(get g1 desktop)\" = 1 ] && [ \"\$(get g2 desktop)\" = 1 ]"
switch_to 1
check "and is shown there, one tab of it" \
	"[ \$((\$(get g1 visible) + \$(get g2 visible))) = 1 ]"
stop g1
stop g2

# A panel is on every desktop, and so are its dialogs, wherever they open.
x11 dock type=dock pos=0,0 size=1280x30 strut=0,0,30,0 dialog=dockdlg dialog_ms=2000
switch_to 2
on "a panel stays on every desktop" dock 1 1
check "its dialog opens on desktop 2 too, in sight" \
	"[ -n \"\$(get dockdlg x)\" ] && [ \"\$(get dockdlg visible)\" = 1 ]"
urgent_is "and nothing is urgent" ""
switch_to 1
stop dock

# ---- icons and tiling per desktop ------------------------------------------------------

wayland i1 size=300x200
do_input key alt+F9
check "minimized on desktop 1, its icon shows" "[ \"\$(get i1 min)\" = 1 ] && [ \"\$(get i1 icon_shown)\" = 1 ]"
switch_to 2
check "not on desktop 2" "[ \"\$(get i1 icon_shown)\" = 0 ]"
wayland i2 size=300x200
do_input key alt+F9
check "an icon on desktop 2 takes the first slot there too" \
	"[ \"\$(get i2 icon_shown)\" = 1 ] && [ \"\$(get i2 icon_x)\" = \"\$(get i1 icon_x)\" ] && \
	[ \"\$(get i2 icon_y)\" = \"\$(get i1 icon_y)\" ]"
switch_to 1
check "back: the first one's icon shows, the other's doesn't" \
	"[ \"\$(get i1 icon_shown)\" = 1 ] && [ \"\$(get i2 icon_shown)\" = 0 ]"
stop i1
stop i2

# A window on another desktop that minimizes itself takes its dialog along.
x11 selfmin size=300x200 dialog=selfkid iconify_ms=2500
check "its dialog is there" "[ -n \"\$(get selfkid x)\" ]"
do_input move $(($(get selfmin x) + 30)) $(($(get selfmin y) + $(get selfmin th) + 30))
do_input click left
to_desktop selfmin 2
wait_for "it minimizes itself on desktop 2" said selfmin iconified
switch_to 2
check "there it is an icon, and its dialog is hidden with it" \
	"[ \"\$(get selfmin min)\" = 1 ] && [ \"\$(get selfmin icon_shown)\" = 1 ] && \
	[ \"\$(get selfkid visible)\" = 0 ]"
switch_to 1
stop selfmin

# A maximized X11 window that says it isn't an icon while its desktop is away keeps
# being maximized.
x11 maxed size=300x200 state=maximized normalize=1
check "maximized" "[ \"\$(get maxed max)\" = 1 ]"
switch_to 2
wait_for "it says it is a normal window" said maxed normalized
check "mwc has had it" "[ \"\$(get maxed tlen)\" = 12 ]"
switch_to 1
check "still maximized" "[ \"\$(get maxed max)\" = 1 ] && [ \"\$(get maxed visible)\" = 1 ]"
stop maxed

do_input key super+t
check "tiling on" "[ \"\$(global tiling on)\" = 1 ]"
wayland t1 size=300x200
wayland t2 size=300x200
check "two tiled on desktop 1, side by side" \
	"[ \"\$(get t1 tiled)\" = 1 ] && [ \"\$(get t2 tiled)\" = 1 ] && [ \"\$(get t1 w)\" -lt 1000 ]"
T1="$(get t1 x) $(get t1 y) $(get t1 w) $(get t1 h)"
switch_to 2
wayland t3 size=300x200
check "alone on desktop 2, it has the whole layout" \
	"[ \"\$(get t3 tiled)\" = 1 ] && [ \"\$(get t3 tile_main)\" = 1 ] && [ \"\$(get t3 w)\" -gt 1000 ]"
check "the others stay in their layout, untouched" "[ \"\$(get t1 tiled)\" = 1 ] && \
	[ \"\$(get t2 tiled)\" = 1 ] && [ \"\$(get t1 x) \$(get t1 y) \$(get t1 w) \$(get t1 h)\" = \"$T1\" ]"
switch_to 1
check "back: they are where they were" \
	"[ \"\$(get t1 x) \$(get t1 y) \$(get t1 w) \$(get t1 h)\" = \"$T1\" ]"
do_input key super+t
stop t1
stop t2
stop t3

# ---- attention from another desktop ----------------------------------------------------

# Its dialog comes while the window is on another desktop: that desktop is marked,
# and the user stays where they are.
x11 late size=300x200 dialog=latekid dialog_ms=2500
to_desktop late 2
ws urgent 2 >"$SMOKE_DIR/urgent.out" 2>&1 &
PID_urgent=$!
check "its dialog appears, on desktop 2" "[ \"\$(get latekid desktop)\" = 2 ] && [ \"\$(get latekid visible)\" = 0 ]"
wait "$PID_urgent" || fail "desktop 2 isn't marked urgent: $(cat "$SMOKE_DIR/urgent.out")"
echo "ok: desktop 2 is marked urgent"
check "the current desktop stays" "[ \"\$(global desktop current)\" = 1 ]"
urgent_is "only that one" 2
stop late
urgent_is "the window closes there: nothing is urgent any more" ""

x11 late2 size=300x200 dialog=late2kid dialog_ms=2500
to_desktop late2 3
ws urgent 3 >"$SMOKE_DIR/urgent.out" 2>&1 || fail "desktop 3 isn't marked urgent: $(cat "$SMOKE_DIR/urgent.out")"
switch_to 3
on "there it is" late2kid 3 1
urgent_is "going there clears it" ""
stop late2
switch_to 1

# The urgency flag in WM_HINTS asks for attention as _NET_WM_STATE_DEMANDS_ATTENTION
# does: GTK's urgency hint, xterm's bellIsUrgent.
x11 bell size=300x200 urgent_after="$SMOKE_DIR/bell"
to_desktop bell 2
urgent_is "nothing is urgent yet" ""
touch "$SMOKE_DIR/bell"
wait_for "it sets the urgency flag" said bell urgent
ws urgent 2 >"$SMOKE_DIR/urgent.out" 2>&1 || fail "desktop 2 isn't marked urgent: $(cat "$SMOKE_DIR/urgent.out")"
urgent_is "its desktop is marked" 2
switch_to 2
urgent_is "going there clears it" ""
stop bell
switch_to 1

# A window withdrawn to the tray keeps no desktop for its dialogs: one opening while
# the user is elsewhere opens there, in sight, not on the desktop the window was left
# on. Nor does an owner that is never shown hold them on desktop 0.
x11 tray size=300x200 dialog=traydlg withdraw_after="$SMOKE_DIR/withdraw" \
	dialog_after="$SMOKE_DIR/traydlg"
touch "$SMOKE_DIR/withdraw"
wait_for "the window is withdrawn" said tray withdrawn
check "and out of the state" "[ -z \"\$(get tray x)\" ]"
switch_to 2
touch "$SMOKE_DIR/traydlg"
check "its dialog opens" "[ -n \"\$(get traydlg x)\" ]"
on "on the current desktop, in sight" traydlg 2 1
urgent_is "nothing asks for attention elsewhere" ""
stop tray
DISPLAY=$X_DISPLAY build/xclient class=owner nomap=1 dialog=ownerdlg \
	>"$SMOKE_DIR/owner.log" 2>&1 &
PID_owner=$!
check "a dialog of an owner never shown" "[ -n \"\$(get ownerdlg x)\" ]"
on "is on the current desktop, in sight" ownerdlg 2 1
kill "$PID_owner"
wait "$PID_owner" 2>/dev/null
check "it is gone" "[ -z \"\$(get ownerdlg x)\" ]"
switch_to 1

# ---- a program on every desktop: Mwm*NAME*allDesktops -------------------------------------

RC=$XDG_CONFIG_HOME/mwc/mwmrc
printf 'Mwm*outputManagement: true\nMwm*everywhere*allDesktops: true\n' >"$RC"
reload_mwc
wayland everywhere size=200x100
switch_to 2
check "with allDesktops it stays in sight on desktop 2" "[ \"\$(get everywhere visible)\" = 1 ]"
do_input move $(($(get everywhere x) + 30)) $(($(get everywhere y) + $(get everywhere th) + 30))
do_input click left
check "it has the focus" "[ \"\$(get everywhere active)\" = 1 ]"
do_input key alt+space
check "its window menu is open" "[ \"\$(global menu open)\" = 1 ]"
[ -z "$(global menu sensitive | tr -cd 1234)" ] ||
	fail "it can be moved to a desktop: $(global menu sensitive)"
echo "ok: and offers no desktop to move to"
do_input key Escape
echo 'Mwm*outputManagement: true' >"$RC"
reload_mwc
check "without the entry it stays where it is in sight" \
	"[ \"\$(get everywhere desktop)\" = 2 ] && [ \"\$(get everywhere visible)\" = 1 ]"
switch_to 1
on "and is on that desktop only now" everywhere 2 0
switch_to 2
stop everywhere
switch_to 1
# The name that says so comes only after mapping (a late app_id): it counts all the same,
# as for colours, dock and tiling.
printf 'Mwm*outputManagement: true\nMwm*lateall*allDesktops: true\n' >"$RC"
reload_mwc
# Long enough to be seen under its first name, under valgrind too.
wayland latewin size=200x100 late_app=lateall late_ms=1500
wait_for "it takes its name" said latewin "late app"
check "and is everywhere now" "[ \"\$(get lateall everywhere)\" = 1 ]"
switch_to 2
check "in sight on desktop 2 too" "[ \"\$(get lateall visible)\" = 1 ]"
switch_to 1
kill "$PID_latewin"
wait "$PID_latewin" 2>/dev/null
check "it is gone" "[ -z \"\$(get lateall x)\" ]"
# The same for an X11 window whose WM_CLASS comes late.
x11 xlate size=200x100 class_later=lateall class_after="$SMOKE_DIR/class"
touch "$SMOKE_DIR/class"
wait_for "it takes its class" said xlate "class set"
check "X11: everywhere now" "[ \"\$(get lateall everywhere)\" = 1 ]"
switch_to 2
check "X11: in sight on desktop 2 too" "[ \"\$(get lateall visible)\" = 1 ]"
switch_to 1
kill "$PID_xlate"
wait "$PID_xlate" 2>/dev/null
check "it is gone" "[ -z \"\$(get lateall x)\" ]"
# Its dialog with it, and _NET_WM_STATE says sticky: from desktop 2, where both are out
# of sight, they come into it.
x11 xfam size=200x100 class_later=lateall class_after="$SMOKE_DIR/famclass" \
	dialog=xfamdlg netstate=1
check "its dialog is mapped" "[ -n \"\$(get xfamdlg x)\" ]"
switch_to 2
check "both out of sight" "[ \"\$(get xfam visible)\" = 0 ] && [ \"\$(get xfamdlg visible)\" = 0 ]"
touch "$SMOKE_DIR/famclass"
wait_for "it takes its class" said xfam "class set"
check "in sight on desktop 2" "[ \"\$(get lateall visible)\" = 1 ]"
check "its dialog as well" "[ \"\$(get xfamdlg visible)\" = 1 ]"
wait_for "sticky, it says" eval "grep '^net-more' \"\$SMOKE_DIR/xfam.log\" | tail -1 | \
	grep -q 'sticky=1'"
switch_to 1
kill "$PID_xfam"
wait "$PID_xfam" 2>/dev/null
check "it is gone" "[ -z \"\$(get lateall x)\" ] && [ -z \"\$(get xfamdlg x)\" ]"
echo 'Mwm*outputManagement: true' >"$RC"
reload_mwc

# ---- On All Desktops from the window menu -------------------------------------------------

wayland stick size=200x100 dialog=stickkid
check "a window with a dialog" "[ -n \"\$(get stickkid x)\" ]"
do_input move $(($(get stick x) + 30)) $(($(get stick y) + $(get stick th) + 30))
do_input click left
check "it has the focus" "[ \"\$(get stick active)\" = 1 ]"
do_input key alt+space
check "its window menu is open" "[ \"\$(global menu open)\" = 1 ]"
do_input key o
check "On All Desktops" "[ \"\$(get stick everywhere)\" = 1 ] && [ \"\$(get stickkid everywhere)\" = 1 ]"
switch_to 3
check "it and its dialog stay in sight on desktop 3" \
	"[ \"\$(get stick visible)\" = 1 ] && [ \"\$(get stickkid visible)\" = 1 ]"
do_input move $(($(get stick x) + 30)) $(($(get stick y) + $(get stick th) + 30))
do_input click left
do_input key alt+space
check "its window menu is open" "[ \"\$(global menu open)\" = 1 ]"
[ -z "$(global menu sensitive | tr -cd 1234)" ] || fail "a window on all desktops offers one: $(global menu sensitive)"
echo "ok: and offers no single desktop"
do_input key o
check "Only This Desktop: it stays on desktop 3" \
	"[ \"\$(get stick everywhere)\" = 0 ] && [ \"\$(get stick desktop)\" = 3 ] && \
	[ \"\$(get stickkid desktop)\" = 3 ] && [ \"\$(get stick visible)\" = 1 ]"
switch_to 1
on "and is gone from desktop 1" stick 3 0
switch_to 3
stop stick
switch_to 1

# A panel is on every desktop anyway: the menu can't change that.
x11 dockmenu type=dock pos=0,690 size=1280x30
check "a panel" "[ \"\$(get dockmenu everywhere)\" = 1 ]"
stop dockmenu

# ---- the group follows its output -------------------------------------------------------

ws outputs 0 >"$SMOKE_DIR/outputs.out" 2>&1 &
PID_outputs=$!
# It must have seen the output first, or it ends at once without seeing it leave.
wait_for "the pager sees the output" grep -q '^outputs-start 1' "$SMOKE_DIR/outputs.out"
WAYLAND_DISPLAY=wayland-0 build/outputc apply HEADLESS-1 off || fail "switching the output off"
wait "$PID_outputs" || fail "the group keeps an output that left: $(cat "$SMOKE_DIR/outputs.out")"
echo "ok: an output switched off leaves the group"
WAYLAND_DISPLAY=wayland-0 build/outputc apply HEADLESS-1 on || fail "switching it on"
ws outputs 1 >"$SMOKE_DIR/outputs.out" 2>&1 || fail "the output doesn't come back: $(cat "$SMOKE_DIR/outputs.out")"
echo "ok: and a new client sees it again"

# A pager asking for several desktops at once: the first at once, then at most one per
# 50 ms, the last asked for; none is lost and the last one wins.
ws activate 2 4 1 3 >/dev/null || fail "activating 2 4 1 3"
check "the last one asked for is current" "[ \"\$(global desktop current)\" = 3 ]"
switch_to 1

# An X11 window on another desktop asks to be on all of them: it comes into sight.
x11 farstick size=200x100 script=-r,-r,+s
switch_to 2
check "farstick is out of sight" "[ \"\$(get farstick visible)\" = 0 ]"
grep -q '^sent' "$SMOKE_DIR/farstick.log" && fail "farstick asked too early for the test"
wait_for "it asks to be sticky" grep -q '^sent 3' "$SMOKE_DIR/farstick.log"
check "it is everywhere, and in sight here" \
	"[ \"\$(get farstick everywhere)\" = 1 ] && [ \"\$(get farstick visible)\" = 1 ]"
kill "$PID_farstick"
wait "$PID_farstick" 2>/dev/null
switch_to 1

kill -0 "$pid" || fail "mwc died"
stop_vinput
stop_mwc
echo "PASS: desktop"
