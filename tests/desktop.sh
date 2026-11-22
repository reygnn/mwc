#!/bin/bash
# Headless test of the virtual desktops: switching through ext-workspace-v1 as a
# tool does, moving windows through the window menu, what goes along (dialogs, tab
# groups) and what stays everywhere, icons and tiling per desktop, a window on another
# desktop asking for attention, and the workspace group following its output.
# Output goes to build/desktop.log. Exits non-zero on failure.
LOG=build/desktop.log
. "$(dirname "$0")/lib.sh"

for tool in build/vinput build/xdgc build/workspacec build/outputc build/layerc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
export MWC_TOAST_MS=3000 # see the toast below
mkdir -p "$XDG_CONFIG_HOME/mwc"
echo 'Mwm*outputManagement: true' >"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'exec sleep 300'
start_vinput
refresh

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
wayland parent size=300x200 dialog=kid
check "the dialog is there" "[ -n \"\$(get kid x)\" ]"
do_input move $(($(get parent x) + 30)) $(($(get parent y) + $(get parent th) + 30))
do_input click left
check "the window has the focus" "[ \"\$(get parent active)\" = 1 ]"
to_desktop parent 4
on "its dialog went along" kid 4 0
stop parent
wayland g1 size=300x200
wayland g2 size=300x200
do_input key super+g
check "a tab group" "[ \"\$(get g1 tab)\" != -1 ] && [ \"\$(get g2 tab)\" != -1 ]"
to_desktop g2 1
check "the whole group went" "[ \"\$(get g1 desktop)\" = 1 ] && [ \"\$(get g2 desktop)\" = 1 ]"
switch_to 1
check "and is shown there, one tab of it" \
	"[ \$((\$(get g1 visible) + \$(get g2 visible))) = 1 ]"
stop g1
stop g2

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
wayland selfmin size=300x200 dialog=selfkid minimize_after="$SMOKE_DIR/selfmin"
check "its dialog is there" "[ -n \"\$(get selfkid x)\" ]"
do_input move $(($(get selfmin x) + 30)) $(($(get selfmin y) + $(get selfmin th) + 30))
do_input click left
check "it has the focus" "[ \"\$(get selfmin active)\" = 1 ]"
to_desktop selfmin 2
touch "$SMOKE_DIR/selfmin"
wait_for "it asks to be minimized on desktop 2" said selfmin "minimize asked"
switch_to 2
check "there it is an icon, and its dialog is hidden with it" \
	"[ \"\$(get selfmin min)\" = 1 ] && [ \"\$(get selfmin icon_shown)\" = 1 ] && \
	[ \"\$(get selfkid visible)\" = 0 ]"
switch_to 1
stop selfmin

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
wayland late size=300x200 dialog=latekid dialog_after="$SMOKE_DIR/late"
to_desktop late 2
ws urgent 2 >"$SMOKE_DIR/urgent.out" 2>&1 &
PID_urgent=$!
touch "$SMOKE_DIR/late"
check "its dialog appears, on desktop 2" "[ \"\$(get latekid desktop)\" = 2 ] && [ \"\$(get latekid visible)\" = 0 ]"
wait "$PID_urgent" || fail "desktop 2 isn't marked urgent: $(cat "$SMOKE_DIR/urgent.out")"
echo "ok: desktop 2 is marked urgent"
check "the current desktop stays" "[ \"\$(global desktop current)\" = 1 ]"
urgent_is "only that one" 2
stop late
urgent_is "the window closes there: nothing is urgent any more" ""

wayland late2 size=300x200 dialog=late2kid dialog_after="$SMOKE_DIR/late2"
to_desktop late2 3
touch "$SMOKE_DIR/late2"
ws urgent 3 >"$SMOKE_DIR/urgent.out" 2>&1 || fail "desktop 3 isn't marked urgent: $(cat "$SMOKE_DIR/urgent.out")"
switch_to 3
on "there it is" late2kid 3 1
urgent_is "going there clears it" ""
stop late2
switch_to 1

# ---- a program on every desktop: Mwm*NAME*allDesktops -------------------------------------

RC=$XDG_CONFIG_HOME/mwc/mwmrc
printf 'Mwm*outputManagement: true\nMwm*everywhere*allDesktops: true\n' >"$RC"
reload_mwc
wayland everywhere size=200x100 dialog=everydlg dialog_after="$SMOKE_DIR/every.open"
switch_to 2
# A dialog it opens later is where it is, here too.
touch "$SMOKE_DIR/every.open"
wait_for "it opens a dialog" said everywhere 'dialog opened'
on "its dialog is in sight on desktop 2" everydlg 2 1
check "and has the keys" "[ \"\$(get everydlg kbfocus)\" = 1 ]"
urgent_is "nothing is marked" ""
do_input key alt+F4
check "the dialog is closed" "[ -z \"\$(get everydlg x)\" ]"
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
wayland latewin size=200x100 late_app=lateall late_after="$SMOKE_DIR/latewin"
touch "$SMOKE_DIR/latewin"
wait_for "it takes its name" said latewin "late app"
check "and is everywhere now" "[ \"\$(get lateall everywhere)\" = 1 ]"
switch_to 2
check "in sight on desktop 2 too" "[ \"\$(get lateall visible)\" = 1 ]"
switch_to 1
kill "$PID_latewin"
wait "$PID_latewin" 2>/dev/null
check "it is gone" "[ -z \"\$(get lateall x)\" ]"
# Shown again under its first name, the rules are those for that one: on this desktop
# only.
wayland relabel size=200x100 late_app=lateall late_after="$SMOKE_DIR/relabel" \
	rehide_after="$SMOKE_DIR/relabel.hide" reshow_after="$SMOKE_DIR/relabel.show"
touch "$SMOKE_DIR/relabel"
wait_for "it takes the name" said relabel "late app"
check "and is everywhere" "[ \"\$(get lateall everywhere)\" = 1 ]"
touch "$SMOKE_DIR/relabel.hide"
check "it is withdrawn" "[ -z \"\$(get lateall x)\" ]"
touch "$SMOKE_DIR/relabel.show"
check "shown again as relabel, it is on this desktop only" \
	"[ \"\$(get relabel everywhere)\" = 0 ] && [ \"\$(get relabel desktop)\" = 1 ]"
stop relabel
# Its dialog with it: from desktop 2, where both are out of sight, they come into it.
wayland fam size=200x100 late_app=lateall late_after="$SMOKE_DIR/fam" dialog=famdlg
check "its dialog is mapped" "[ -n \"\$(get famdlg x)\" ]"
switch_to 2
check "both out of sight" "[ \"\$(get fam visible)\" = 0 ] && [ \"\$(get famdlg visible)\" = 0 ]"
touch "$SMOKE_DIR/fam"
wait_for "it takes its name" said fam "late app"
check "in sight on desktop 2" "[ \"\$(get lateall visible)\" = 1 ]"
check "its dialog as well" "[ \"\$(get famdlg visible)\" = 1 ]"
switch_to 1
kill "$PID_fam"
wait "$PID_fam" 2>/dev/null
check "it is gone" "[ -z \"\$(get lateall x)\" ] && [ -z \"\$(get famdlg x)\" ]"
# A tab group is on all desktops as a whole: one of its tabs taking such a name late
# takes the others along, and each tab can be shown on any desktop.
wayland plain size=300x200
wayland tabbed size=300x200 late_app=lateall late_after="$SMOKE_DIR/tabbed"
do_input key super+g
check "a tab group" "[ \"\$(get plain tab)\" != -1 ] && [ \"\$(get tabbed tab)\" != -1 ]"
touch "$SMOKE_DIR/tabbed"
wait_for "one of them takes its name" said tabbed "late app"
check "the whole group is everywhere" \
	"[ \"\$(get lateall everywhere)\" = 1 ] && [ \"\$(get plain everywhere)\" = 1 ]"
switch_to 2
check "in sight on desktop 2" \
	"[ \$((\$(get plain visible) + \$(get lateall visible))) = 1 ]"
shown=$( [ "$(get plain visible)" = 1 ] && echo plain || echo lateall )
other=$( [ "$shown" = plain ] && echo lateall || echo plain )
do_input move $(($(get $shown x) + 30)) $(($(get $shown y) + $(get $shown th) + 30))
do_input click left
check "it has the focus" "[ \"\$(get $shown active)\" = 1 ]"
do_input key super+Tab
check "the other tab is shown here too" \
	"[ \"\$(get $other visible)\" = 1 ] && [ \"\$(get $shown visible)\" = 0 ]"
# Out of the group, the plain one is everywhere no more: it stays where it is in sight.
do_input key super+shift+g
check "out of the group" "[ \"\$(get plain tab)\" = -1 ] && [ \"\$(get lateall tab)\" = -1 ]"
check "the plain one is on desktop 2 only" \
	"[ \"\$(get plain everywhere)\" = 0 ] && [ \"\$(get plain desktop)\" = 2 ] && \
	 [ \"\$(get plain visible)\" = 1 ] && [ \"\$(get lateall everywhere)\" = 1 ]"
switch_to 1
on "and out of sight on desktop 1" plain 2 0
stop plain
kill "$PID_tabbed"
wait "$PID_tabbed" 2>/dev/null
check "it is gone" "[ -z \"\$(get lateall x)\" ]"
# The window with the rule joins a plain one, and leaves it again on desktop 2: the one
# left alone is on that desktop only. With three, the two left are.
for n in 1 2; do
	wayland p1 size=300x200
	[ $n = 2 ] && { wayland p2 size=300x200; do_input key super+g; }
	wayland lateall size=300x200
	do_input key super+g
	check "$n plain, the one with the rule joins" \
		"[ \"\$(get lateall tab)\" != -1 ] && [ \"\$(get p1 everywhere)\" = 1 ]"
	switch_to 2
	check "the group is in sight on desktop 2, its new tab shown" \
		"[ \"\$(get lateall visible)\" = 1 ] && [ \"\$(get lateall active)\" = 1 ]"
	do_input key super+shift+g
	check "the one with the rule leaves" "[ \"\$(get lateall tab)\" = -1 ]"
	check "the plain ones are on desktop 2 only, in sight" \
		"[ \"\$(get p1 everywhere)\" = 0 ] && [ \"\$(get p1 desktop)\" = 2 ] && \
		 { [ $n = 1 ] || { [ \"\$(get p2 everywhere)\" = 0 ] && [ \"\$(get p2 desktop)\" = 2 ]; }; } && \
		 [ \$((\$(get p1 visible) + 0\$(get p2 visible))) = 1 ]"
	switch_to 1
	stop p1
	[ $n = 2 ] && stop p2
	stop lateall
done
echo 'Mwm*outputManagement: true' >"$RC"
reload_mwc

# A reload takes a window off all desktops: its dialog goes where it goes, here, not
# to the desktop both were mapped on, which they still say while on all of them.
printf 'Mwm*outputManagement: true\nMwm*evr*allDesktops: true\n' >"$RC"
reload_mwc
switch_to 2
wayland evr size=300x200 dialog=evrkid
check "a window with its dialog, mapped on desktop 2" \
	"[ \"\$(get evrkid desktop)\" = 2 ] && [ \"\$(get evr desktop)\" = 2 ]"
switch_to 1
check "both in sight here, still of desktop 2" "[ \"\$(get evrkid visible)\" = 1 ] && \
	[ \"\$(get evr desktop)\" = 2 ] && [ \$(stack_pos evrkid) -lt \$(stack_pos evr) ]"
echo 'Mwm*outputManagement: true' >"$RC"
reload_mwc
check "after the reload both are on this desktop, in sight" \
	"[ \"\$(get evr desktop)\" = 1 ] && [ \"\$(get evrkid desktop)\" = 1 ] && \
	 [ \"\$(get evr visible)\" = 1 ] && [ \"\$(get evrkid visible)\" = 1 ]"
stop evr

# Icons of two windows on all desktops, mapped on different ones: each takes a slot of
# its own, on every desktop.
printf 'Mwm*outputManagement: true\nMwm*ev1*allDesktops: true\nMwm*ev2*allDesktops: true\n' >"$RC"
reload_mwc
switch_to 2
wayland ev1 size=200x100
switch_to 3
wayland ev2 size=200x100
do_input key alt+F9
check "ev2 is an icon" "[ \"\$(get ev2 min)\" = 1 ]"
do_input move $(($(get ev1 x) + 30)) $(($(get ev1 y) + $(get ev1 th) + 30))
do_input click left
check "ev1 has the focus" "[ \"\$(get ev1 active)\" = 1 ]"
do_input key alt+F9
check "ev1 is an icon" "[ \"\$(get ev1 min)\" = 1 ]"
switch_to 1
check "on desktop 1 both icons, in two slots" \
	"[ \"\$(get ev1 icon_shown)\" = 1 ] && [ \"\$(get ev2 icon_shown)\" = 1 ] && \
	 [ \"\$(get ev1 icon_x),\$(get ev1 icon_y)\" != \"\$(get ev2 icon_x),\$(get ev2 icon_y)\" ]"
stop ev1
stop ev2
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

# ---- asking for the focus, xdg-activation-v1 -------------------------------------------

# Only a token made while its window had the keyboard, for an input event the client
# got, takes the focus. Then it brings a window back from its icon and switches to its
# desktop; any other token marks the desktop of a window on another one.
mkfifo "$SMOKE_DIR/src.ctl" "$SMOKE_DIR/dst.ctl"
exec {srcfd}<>"$SMOKE_DIR/src.ctl" {dstfd}<>"$SMOKE_DIR/dst.ctl"
send() { local fd="${1}fd"; echo "$2" >&"${!fd}"; }
TOKEN=$SMOKE_DIR/token
token() { # app [noinput]: a token made by app's window, in $TOKEN
	rm -f "$TOKEN"
	send "$1" "token${2:+_$2} $TOKEN"
	wait_for "a token from $1" test -s "$TOKEN"
}
focus_src() { # near its left edge: dst, placed after it, covers the rest
	do_input move $(($(get src x) + $(get src bw) + 5)) $(($(get src y) + $(get src th) + 30))
	do_input click left
	check "src has the focus" "[ \"\$(get src active)\" = 1 ]"
}
# mwc's answer to dst's next request, once it is in the log: else a token wlroots threw
# out itself, which never reaches mwc, would look like one mwc refused.
answered() { # what
	local n
	n=$(grep -c "dst asks for the focus: $1" "$LOG")
	send dst "activate $TOKEN"
	wait_for "mwc answers: $1" eval \
		"[ \$(grep -c 'dst asks for the focus: $1' \"$LOG\") -gt $n ]"
}
stays_with() { # description app
	answered "not given"
	check "$1" "[ \"\$(get $2 active)\" = 1 ] && [ \"\$(get dst active)\" = 0 ]"
}
wayland src size=300x200 control="$SMOKE_DIR/src.ctl"
wayland dst size=300x200 control="$SMOKE_DIR/dst.ctl"
focus_src
token src
send dst "activate $TOKEN"
check "a token from the window with the focus: dst takes it" "[ \"\$(get dst active)\" = 1 ]"
wait_for "it is said" grep -q "dst asks for the focus: given" "$LOG"
focus_src
# The pointer over it, at its bottom right corner where src doesn't cover it: wlroots
# itself takes a token only from a surface with the keyboard or the pointer, and for an
# input event the client got. That much a window in the background has.
do_input move $(($(get dst x) + $(get dst bw) + 290)) $(($(get dst y) + $(get dst fh) - $(get dst bw) - 5))
token dst
stays_with "one made in the background doesn't" src
token src noinput
stays_with "nor one without an input event, as mwt's bell makes" src
focus_src
token src
wayland third size=200x100
check "a third window takes the keys" "[ \"\$(get third active)\" = 1 ]"
stays_with "nor one made before the keys went to another window" third
stop third
focus_src
token src
WAYLAND_DISPLAY=wayland-0 build/layerc layer=overlay keyboard=exclusive size=100x100 \
	>"$SMOKE_DIR/lock.events" 2>&1 &
PID_lock=$!
wait_for "a layer surface takes the keys exclusively" grep -q configured "$SMOKE_DIR/lock.events"
stays_with "nor once a screen locker took them" src
kill "$PID_lock"
wait "$PID_lock" 2>/dev/null
send dst minimize
check "dst is an icon" "[ \"\$(get dst min)\" = 1 ]"
focus_src
token src
send dst "activate $TOKEN"
check "a good token brings it back from its icon, with the focus" \
	"[ \"\$(get dst min)\" = 0 ] && [ \"\$(get dst active)\" = 1 ]"
to_desktop dst 3
focus_src
token src
send dst "activate $TOKEN"
check "and switches to its desktop" \
	"[ \"\$(global desktop current)\" = 3 ] && [ \"\$(get dst active)\" = 1 ]"
switch_to 1
focus_src
urgent_is "nothing is marked yet" ""
token src noinput
answered "its desktop is marked"
check "any other token doesn't switch" "[ \"\$(global desktop current)\" = 1 ]"
urgent_is "it marks its desktop" "3"
switch_to 3
stop dst
switch_to 1
stop src
exec {srcfd}>&- {dstfd}>&-

# ---- the toast ---------------------------------------------------------------------------

# Each switch, whoever asks for it, says "Desktop N" in the middle of the pointer's
# monitor, for a second, here three (MWC_TOAST_MS) for margins of a second. A switch
# while one shows replaces it and starts its time anew. None over a screen locker.
toast() { global toast "$1"; }
n=$(toast shown)
switch_to 1
sleep 0.3
check "no switch, no toast" "[ \"\$(toast shown)\" = $n ]"
switch_to 2
check "a switch shows a toast" "[ \"\$(toast text)\" = Desktop_2 ] && [ \"\$(toast shown)\" = $((n + 1)) ]"
TX=$(toast x) TY=$(toast y) TW=$(toast w) TH=$(toast h)
[ "$TX" = $(((1280 - TW) / 2)) ] && [ "$TY" = $(((720 - TH) / 2)) ] ||
	fail "the toast isn't in the middle: $TX,$TY ${TW}x$TH"
echo "ok: in the middle of the monitor"
pixel_is "a grey line round it" "$TX" "$TY" 808080
pixel_is "a dark box" $((TX + 2)) $((TY + 2)) 2b2b2b
sleep 1.5
check "it still shows before the next switch" "[ \"\$(toast text)\" = Desktop_2 ]"
switch_to 3
check "the next switch replaces it" "[ \"\$(toast text)\" = Desktop_3 ] && [ \"\$(toast shown)\" = $((n + 2)) ]"
sleep 2 # over three seconds after the first: gone by now, had its time not started anew
check "and starts its time anew" "[ \"\$(toast text)\" = Desktop_3 ]"
check "then it goes" '[ -z "$(toast text)" ]'
pixel_is "and the desktop shows again" $((TX + 2)) $((TY + 2)) 7189a3
WAYLAND_DISPLAY=wayland-0 build/layerc layer=overlay keyboard=exclusive size=100x100 \
	>"$SMOKE_DIR/lock.events" 2>&1 &
PID_lock=$!
wait_for "a screen locker takes the keys" grep -q configured "$SMOKE_DIR/lock.events"
switch_to 1
sleep 0.3
check "a switch under it shows none" "[ \"\$(toast shown)\" = $((n + 2)) ] && [ -z \"\$(toast text)\" ]"
kill "$PID_lock"
wait "$PID_lock" 2>/dev/null
switch_to 2
check "once it is gone, a switch shows one again" "[ \"\$(toast shown)\" = $((n + 3)) ]"
switch_to 1

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

kill -0 "$pid" || fail "mwc died"
stop_vinput
stop_mwc
echo "PASS: desktop"
