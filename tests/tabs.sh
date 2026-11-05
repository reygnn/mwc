#!/bin/bash
# Headless test of tab groups: forming them, switching, what only the shown window takes
# part in, windows leaving, and how groups get along with the window states.
# Output goes to build/tabs.log. Exits non-zero on failure.
LOG=build/tabs.log
. "$(dirname "$0")/lib.sh"

require_tools Xwayland grim
for tool in build/vinput build/xclient build/xdgc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state

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
	check "$name is mapped" "[ -n \"\$(get $name x)\" ] && [ \"\$(get $name active)\" = 1 ]"
}
wayland() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$name" "$@" >"$SMOKE_DIR/$name.log" 2>&1 &
	eval "PID_$name=$!"
	check "$name is mapped" "[ -n \"\$(get $name x)\" ] && [ \"\$(get $name active)\" = 1 ]"
}
stop() {
	local var="PID_$1"
	kill "${!var}" 2>/dev/null
	wait "${!var}" 2>/dev/null
	check "$1 is gone" "[ -z \"\$(get $1 x)\" ]"
}
shown() { # description app, and it has the focus
	check "$1" "[ \"\$(get $2 tab_hidden)\" = 0 ] && [ \"\$(get $2 active)\" = 1 ] && \
		[ \"\$(get $2 kbfocus)\" = 1 ]"
}
hidden() {
	check "$1" "[ \"\$(get $2 tab_hidden)\" = 1 ] && [ \"\$(get $2 active)\" = 0 ] && \
		[ \"\$(get $2 visible)\" = 0 ]"
}
alone() { check "$1" "[ \"\$(get $2 tab)\" = -1 ] && [ \"\$(get $2 tab_hidden)\" = 0 ]"; }
said() { grep -q "^$2" "$SMOKE_DIR/$1.log"; }

begin

# ---- forming a group ---------------------------------------------------------------------

wayland a size=400x300
refresh
AX=$(get a x) AY=$(get a y)
x11 b size=300x200
do_input key super+g
shown "Super+G: the window stays shown" b
hidden "and the other is behind it" a
check "in the place of the other, at its size" \
	"[ \"\$(get b x)\" = $AX ] && [ \"\$(get b y)\" = $AY ] && \
	[ \"\$(get b w)\" = 400 ] && [ \"\$(get b h)\" = 300 ]"
check "in tab order" "[ \"\$(get a tab)\" = 0 ] && [ \"\$(get b tab)\" = 1 ]"

do_input key super+Tab
shown "Super+Tab: the next tab" a
hidden "and the other goes" b
check "nothing moved" "[ \"\$(get a x)\" = $AX ] && [ \"\$(get a y)\" = $AY ] && [ \"\$(get a w)\" = 400 ]"
do_input key super+shift+Tab
shown "Super+Shift+Tab: back again" b

x11 c size=200x100
refresh
[ "$(get c tab)" = -1 ] || fail "a new window joins by itself"
do_input key alt+Tab
shown "Alt+Tab counts the group once" b
hidden "and doesn't reach the hidden tab" a
do_input key alt+Tab
shown "and on to the other window" c
do_input key super+g
shown "a third one" c
check "at the end" "[ \"\$(get c tab)\" = 2 ]"
do_input key super+1
shown "Super+1: the first tab" a
do_input key super+3
shown "Super+3: the third" c
do_input key super+9
shown "Super+9: nothing there, nothing happens" c

# ---- sizes, moving, states -----------------------------------------------------------------

do_input key alt+F8
do_input key Right
do_input key Right
do_input key Return
check "the shown one has a new size" "[ \"\$(get c w)\" != 400 ]"
refresh
W=$(get c w) H=$(get c h)
check "and the hidden ones have it too, before they are shown" \
	"[ \"\$(get a w)\" = $W ] && [ \"\$(get b w)\" = $W ] && [ \"\$(get a h)\" = $H ]"

do_input key alt+F10
check "maximized" "[ \"\$(get c max)\" = 1 ] && [ \$((\$(get c w) + 2 * B)) = 1280 ]"
do_input key super+1
shown "another tab" a
check "takes it over" "[ \"\$(get a max)\" = 1 ] && [ \$((\$(get a w) + 2 * B)) = 1280 ] && \
	[ \"\$(get c max)\" = 0 ]"
do_input key alt+F10
check "and un-maximized, it goes where the group was" \
	"[ \"\$(get a max)\" = 0 ] && [ \"\$(get a w)\" = $W ] && [ \"\$(get a x)\" = $AX ]"
do_input key super+Left
check "snapped" "[ \"\$(get a snap)\" != 0 ] && [ \"\$(get a x)\" = 0 ]"
do_input key super+3
check "the state moves over too" "[ \"\$(get c snap)\" != 0 ] && [ \"\$(get c x)\" = 0 ] && \
	[ \"\$(get a snap)\" = 0 ]"
do_input key alt+F5
check "restored" "[ \"\$(get c snap)\" = 0 ] && [ \"\$(get c x)\" = $AX ]"

do_input key alt+F11
check "fullscreen" "[ \"\$(get c full)\" = 1 ]"
do_input key super+Tab
check "no switching while it fills the screen" "[ \"\$(get c tab_hidden)\" = 0 ] && [ \"\$(get c full)\" = 1 ]"
do_input key alt+F11
check "and back" "[ \"\$(get c full)\" = 0 ]"

do_input key alt+F9
check "minimized: one icon" "[ \"\$(get c min)\" = 1 ] && [ \"\$(get a min)\" = 0 ] && [ \"\$(get a tab_hidden)\" = 1 ]"
refresh
do_input move $(($(get c icon_x) + 30)) $(($(get c icon_y) + 30))
do_input click left
do_input click left
shown "restored, as it was" c
hidden "the others still behind it" a

# ---- leaving -------------------------------------------------------------------------------

do_input key super+shift+g
alone "Super+Shift+G: on its own" c
shown "with the focus" c
check "somewhere else" "[ \"\$(get c tab_hidden)\" = 0 ]"
check "the group goes on" "[ \"\$(get b tab)\" != -1 ]"
stop c
do_input key alt+Tab
stop b
alone "the last one left is alone" a
shown "and has the focus" a
stop a
finish

begin
wayland a size=400x300
x11 b size=300x200
do_input key super+g
x11 c size=200x100
do_input key super+g
shown "three, the last one shown" c
x11 d size=200x100
do_input key super+g
do_input key super+2
shown "the second of four" b
do_input key alt+F4
shown "the shown one closes, and the one to its right takes over" c
refresh
[ -z "$(get b x)" ] || fail "the closed window is still there"
do_input key super+1
shown "the first one" a
do_input key alt+F4
shown "the first closes: the one now first" c
do_input key alt+F4
shown "one more" d
alone "and it is alone" d
stop d
x11 b size=300x200
x11 c size=200x100
do_input key super+g
do_input key super+2
shown "the last of two" c
do_input key alt+F4
shown "at the end: the one to its left" b
alone "and it is alone" b
stop b

wayland a size=400x300
x11 b size=300x200
do_input key super+g
stop a
shown "a hidden one dies: nothing shows of it" b
alone "and the group is over" b
stop b

wayland nb size=300x200
x11 b size=300x200
do_input key super+g
do_input key super+Tab
shown "the client on top" nb
stop nb
shown "a shown one dies" b
alone "and the other carries on" b
stop b

# ---- each to its own size again -----------------------------------------------------------

size_is() { check "$1" "[ \"\$(get $2 w)\" = $3 ] && [ \"\$(get $2 h)\" = $4 ]"; }
x11 own1 size=400x300
x11 own2 size=300x200
do_input key super+g
size_is "in the group, the size of the group" own2 400 300
do_input key super+shift+g
alone "out of the group" own2
size_is "taken out, its own size again" own2 300 200
size_is "the other keeps its own" own1 400 300
stop own1
stop own2

x11 keep1 size=400x300
x11 keep2 size=300x200
do_input key super+g
do_input key super+Tab
shown "the other tab" keep1
do_input key super+shift+g
alone "that one taken out" keep1
size_is "it has its size" keep1 400 300
alone "and the group is over" keep2
size_is "the one left over has its own size again as well" keep2 300 200
stop keep1
stop keep2

x11 cl1 size=400x300
x11 cl2 size=300x200
do_input key super+g
do_input key super+Tab
shown "the first tab" cl1
do_input key alt+F4
alone "the first one closes" cl2
size_is "the one left over has its own size" cl2 300 200
stop cl2

wayland w1 size=400x300
wayland w2 size=250x150
do_input key super+g
size_is "a Wayland client in a group" w2 400 300
do_input key super+shift+g
size_is "and out of it, the size it was" w2 250 150
stop w1
stop w2

# Whatever state it was in before: it is the floating size that counts.
x11 mx1 size=300x200
do_input key alt+F10
check "maximized" "[ \"\$(get mx1 max)\" = 1 ]"
x11 mx2 size=250x150
do_input key super+g
check "a group that fills the screen" "[ \"\$(get mx2 max)\" = 1 ]"
do_input key super+shift+g
size_is "taken out of it, its own size" mx2 250 150
check "the other is still maximized" "[ \"\$(get mx1 max)\" = 1 ]"
do_input key alt+Tab
shown "the other one" mx1
do_input key alt+F10
size_is "and un-maximized, its own size" mx1 300 200
stop mx1
stop mx2

x11 mj1 size=400x300
x11 mj2 size=250x150
do_input key alt+F10
check "maximized before it joins" "[ \"\$(get mj2 max)\" = 1 ]"
do_input key super+g
size_is "in the group of a floating one" mj2 400 300
do_input key super+shift+g
size_is "and out again, the size it had before maximizing" mj2 250 150
stop mj1
stop mj2

# The last one left in a state: what it goes back to is its own size, not that of the other.
x11 ms1 size=400x300
do_input key alt+F10
x11 ms2 size=250x150
do_input key super+g
do_input key super+Tab
shown "the first of a maximized group" ms1
do_input key super+shift+g
alone "taken out" ms1
alone "the group is over" ms2
check "the other is still maximized" "[ \"\$(get ms2 max)\" = 1 ]"
do_input key alt+Tab
shown "the other one" ms2
do_input key alt+F10
size_is "and un-maximized, its own size" ms2 250 150
stop ms1
stop ms2

# A window larger than its group is placed by the size it is about to have, so it fits.
for i in 1 2 3 4 5; do x11 "pad$i" size=100x60; done
x11 tiny size=200x100
x11 large size=1200x640
do_input key super+g
size_is "in a small group" large 200 100
do_input key super+shift+g
size_is "out of it, large again" large 1200 640
check "and on the screen" "[ \$((\$(get large x) + 1200 + 2 * B)) -le 1280 ] && [ \$((\$(get large y) + 640 + 2 * B + T)) -le 720 ]"
stop large
stop tiny
for i in 1 2 3 4 5; do stop "pad$i"; done

# ---- who can't be in a group ---------------------------------------------------------------

wayland a size=400x300
x11 fixed size=200x100 min=200x100 max=200x100
do_input key super+g
alone "a window of a fixed size can't join" fixed
alone "and nothing happens to the other" a
stop fixed
wayland nof csd=1 size=200x100
do_input key super+g
alone "a window drawing its own frame can't join" nof
stop nof
DISPLAY=$X_DISPLAY build/xclient class=dlg dialog=kid >"$SMOKE_DIR/dlg.log" 2>&1 &
PID_dlg=$!
check "a dialog with the focus" "[ \"\$(get kid active)\" = 1 ]"
do_input key super+g
alone "a dialog can't join" kid
stop dlg
stop a

# ---- requests from behind ------------------------------------------------------------------

x11 asks ask_later=fullscreen size=300x200
wayland front size=400x300
do_input key super+g
shown "the other tab in front" front
wait_for "the hidden tab asks for fullscreen" said asks 'asked'
sleep 0.3
check "it stays behind" "[ \"\$(get asks tab_hidden)\" = 1 ] && [ \"\$(get asks full)\" = 0 ] && [ \"\$(get front full)\" = 0 ]"
stop asks
stop front

x11 pa dialog=dk dialog_ms=2500 size=300x200
wayland qa size=400x300
do_input key super+g
shown "a tab in front of the other" qa
check "a dialog of the hidden one brings it to the front" "[ \"\$(get pa tab_hidden)\" = 0 ] && [ \"\$(get qa tab_hidden)\" = 1 ]"
check "and has the focus" "[ \"\$(get dk active)\" = 1 ] && [ \"\$(get dk hidden)\" = 0 ]"
do_input key alt+Tab
do_input key super+Tab
check "the dialog goes with its tab" "[ \"\$(get dk hidden)\" = 1 ] && [ \"\$(get qa tab_hidden)\" = 0 ]"
do_input key super+Tab
check "and comes back with it" "[ \"\$(get dk hidden)\" = 0 ]"
stop pa
stop qa

# ---- clicks only reach what is shown -------------------------------------------------------

x11 under move_press=1 pos=100,100 size=400x300
x11 over move_press=1 size=300x200
do_input key super+g
shown "one X tab over another" over
: >"$SMOKE_DIR/under.log"
do_input move 300 250
do_input click left
wait_for "the shown one gets the click" said over 'press main'
sleep 0.3
said under 'press' && fail "a hidden X window caught a click"
echo "ok: the hidden X window didn't"
stop under
stop over

# ---- the layout ----------------------------------------------------------------------------

do_input key super+t
check "tiling" "[ \"\$(global tiling on)\" = 1 ]"
wayland s1 size=300x200
x11 s2 size=300x200
x11 s3 size=300x200
check "three cells" "[ \"\$(get s1 tiled)\" = 1 ] && [ \"\$(get s2 tiled)\" = 1 ] && [ \"\$(get s3 tile_main)\" = 1 ]"
refresh
do_input key super+g
shown "joined in the layout" s3
check "two cells now" "[ \"\$(get s2 tab_hidden)\" = 1 ] && [ \"\$(get s2 tiled)\" = 0 ] && [ \"\$(get s3 tiled)\" = 1 ]"
check "the other one takes all of the stack" "[ \"\$(get s1 tiled)\" = 1 ] && \
	[ \$((\$(get s1 y) + \$(get s1 h) + 2 * B + T + 8)) = 720 ]"
refresh
X=$(get s3 x) Y=$(get s3 y) W=$(get s3 w) M=$(get s3 tile_main)
do_input key super+Tab
shown "the other tab" s2
check "in the same cell" "[ \"\$(get s2 x)\" = $X ] && [ \"\$(get s2 y)\" = $Y ] && [ \"\$(get s2 w)\" = $W ] && \
	[ \"\$(get s2 tile_main)\" = $M ] && [ \"\$(get s3 tiled)\" = 0 ]"
do_input key super+shift+g
alone "out of it" s2
check "a new window leads the layout" "[ \"\$(get s2 tile_main)\" = 1 ] && [ \"\$(get s2 tiled)\" = 1 ]"
alone "the group is over" s3
check "and each has a cell" "[ \"\$(get s3 tiled)\" = 1 ] && [ \"\$(get s1 tiled)\" = 1 ]"
do_input key super+t
stop s1
stop s2
stop s3

# Its place in the layout goes with the shown tab: the group took the place of the older
# window, below the other one in the stack, though the newer window is what shows.
x11 o1 size=300x200
x11 o0 size=300x200
do_input key alt+Tab
shown "the older one" o1
x11 o2 size=300x200
do_input key super+g
x11 o3 size=250x150
do_input key super+t
do_input key alt+Tab
do_input key alt+Tab
shown "the group" o2
check "in the stack below the other" "[ \"\$(get o3 tile_main)\" = 1 ] && [ \"\$(get o2 y)\" -gt \"\$(get o0 y)\" ]"
do_input key super+Tab
shown "its other tab" o1
check "stays below it" "[ \"\$(get o1 tiled)\" = 1 ] && [ \"\$(get o1 y)\" -gt \"\$(get o0 y)\" ]"
do_input key super+t
stop o1
stop o2
stop o0
stop o3

# What a group goes back to, and its place in the layout, go with the shown tab.
x11 g1 pos=200,150 size=300x200
x11 g2 size=300x200
do_input key super+g
refresh
GX=$(get g2 x) GY=$(get g2 y)
x11 g3 size=250x150
do_input key super+t
check "two in the layout" "[ \"\$(get g2 tiled)\" = 1 ] && [ \"\$(get g3 tile_main)\" = 1 ]"
do_input key alt+Tab
shown "the group in the stack" g2
do_input key super+Tab
shown "its other tab" g1
check "in the stack as well, not leading" "[ \"\$(get g1 tile_main)\" = 0 ] && [ \"\$(get g3 tile_main)\" = 1 ]"
do_input key super+t
check "out of the layout it goes back to where the group was" \
	"[ \"\$(get g1 tiled)\" = 0 ] && [ \"\$(get g1 x)\" = $GX ] && [ \"\$(get g1 y)\" = $GY ]"
stop g1
stop g2
stop g3

# ---- a reload ------------------------------------------------------------------------------

wayland a size=400x300
x11 b size=300x200
do_input key super+g
kill -HUP "$pid"
sleep 0.5
shown "a group stays through a reload" b
hidden "both of it" a
stop a
stop b
finish

# ---- tabs in the title bar -------------------------------------------------------------

RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
cat >"$RC" <<'MWMRC'
Mwm*activeBackground:   #102030
Mwm*ta*background:      #aa0000
Mwm*tb*background:      #00aa00
Mwm*tc*background:      #0000aa
MWMRC
begin
x11 ta pos=100,100 size=600x200
x11 tb size=300x200
do_input key super+g
shown "two tabs" tb
refresh
TX=$((100 + B + T)) TY=$((100 + B)) TW=$(((600 - 3 * T) / 2))
pixel_is "the other tab is in its own unfocused colour" $((TX + 8)) $((TY + 4)) aa0000
pixel_is "the shown tab in the frame's" $((TX + TW + 8)) $((TY + 4)) 102030
shot() { sleep 0.3; WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/shot.png"; }
do_input key super+Tab
shown "the other one" ta
pixel_is "and now the colours change places" $((TX + TW + 8)) $((TY + 4)) 00aa00
pixel_is "the shown one in the frame's" $((TX + 8)) $((TY + 4)) 102030
do_input key super+Tab

do_input move $((TX + 40)) $((TY + T / 2))
do_input click left
shown "a click on a tab shows it" ta
sleep 0.5
do_input move $((TX + TW + 40)) $((TY + T / 2))
do_input down left
do_input up left
shown "a click on the other" tb
sleep 0.5
do_input down left
do_input move $((TX + TW + 90)) $((TY + T / 2 + 40))
check "dragging a tab moves the group" "[ \"\$(global cursor mode)\" != 0 ]"
do_input up left
check "to where it was dropped" "[ \"\$(get tb x)\" = 150 ] && [ \"\$(get tb y)\" = 140 ]"
shown "without a switch" tb
sleep 0.5
TY=$((140 + B)) TX=$((150 + B + T))
do_input move $((TX + 40)) $((TY + T / 2))
do_input down left
do_input move $((TX + 90)) $((TY + T / 2 + 40))
do_input up left
shown "also from the tab not shown" tb
check "which moves it as well" "[ \"\$(get tb x)\" = 200 ] && [ \"\$(get tb y)\" = 180 ]"
TY=$((180 + B)) TX=$((200 + B + T))
sleep 0.5

do_input move $((TX + 40)) $((TY + T / 2))
do_input wheel 1
shown "the wheel: the next tab" ta
do_input wheel -1
shown "and back" tb
do_input wheel 1
do_input wheel 1
shown "around the end" tb

do_input move $((TX + 40)) $((TY + T / 2))
sleep 0.5
do_input click left
do_input click left
check "a double click on a tab behind shows and maximizes it" "[ \"\$(get ta max)\" = 1 ] && [ \"\$(get ta tab_hidden)\" = 0 ]"
do_input key alt+F10
stop ta
stop tb

# With the focus elsewhere, tabs of the same colour differ only by their bevels. The
# top edge of a raised one is light, that of a sunken one dark.
x11 sa pos=100,100 size=600x200
x11 sb size=300x200
do_input key super+g
x11 elsewhere pos=800,400 size=200x100
TX=$((100 + B + T)) TY=$((100 + B))
shot
LEFT=$(build/pngtool pixel "$SMOKE_DIR/shot.png" $((TX + TW / 2)) "$TY")
RIGHT=$(build/pngtool pixel "$SMOKE_DIR/shot.png" $((TX + TW + TW / 2)) "$TY")
[ "$LEFT" != "$RIGHT" ] || fail "the shown tab looks like the other one"
do_input key alt+Tab
do_input key super+Tab
do_input key alt+Tab
shot
[ "$(build/pngtool pixel "$SMOKE_DIR/shot.png" $((TX + TW / 2)) "$TY")" = "$RIGHT" ] &&
	[ "$(build/pngtool pixel "$SMOKE_DIR/shot.png" $((TX + TW + TW / 2)) "$TY")" = "$LEFT" ] ||
	fail "the bevels don't change places with the shown tab"
echo "ok: the shown tab is sunken, and the others raised"
stop sa
stop sb
stop elsewhere

# A title that changes behind the shown tab shows.
x11 tn pos=100,100 size=600x200 title_later=WWWWWWWWWWWWWWWW
x11 tb size=300x200
do_input key super+g
shown "with a hidden tab" tb
refresh
TY=$((100 + B)) TX=$((100 + B + T))
shot
BEFORE=$(build/pngtool count "$SMOKE_DIR/shot.png" $((TX + 3)) $((TY + 3)) $((TW - 6)) $((T - 6)) "$(build/pngtool pixel "$SMOKE_DIR/shot.png" $((TX + 3)) $((TY + 3)))")
wait_for "its title changes" said tn 'renamed'
shot
AFTER=$(build/pngtool count "$SMOKE_DIR/shot.png" $((TX + 3)) $((TY + 3)) $((TW - 6)) $((T - 6)) "$(build/pngtool pixel "$SMOKE_DIR/shot.png" $((TX + 3)) $((TY + 3)))")
[ "$BEFORE" != "$AFTER" ] || fail "the tab of a hidden window keeps its old title"
echo "ok: the tab of a hidden window shows its new title"
stop tn
stop tb

# More tabs than fit.
x11 n1 pos=100,100 size=300x200
for i in 2 3 4 5; do
	x11 "n$i" size=300x200
	do_input key super+g
done
shown "five tabs in a narrow window" n5
check "the last ones are drawn" "[ \"\$(get n5 tab_first)\" -gt 0 ]"
do_input key super+1
shown "the first" n1
check "and the strip moves along" "[ \"\$(get n1 tab_first)\" = 0 ]"
TY=$((100 + B)) TX=$((100 + B + T + T / 2))
do_input move $((TX + (300 - 3 * T - 2 * (T / 2)) / 2 + 10)) $((TY + T / 2))
sleep 0.5
do_input click left
shown "a click reaches the second" n2
do_input key super+5
shown "the last again" n5
check "and clicking where the first was now reaches another" "[ \"\$(get n5 tab_first)\" -gt 0 ]"
do_input move $((TX + 10)) $((TY + T / 2))
sleep 0.5
do_input click left
check "not the first" "[ \"\$(get n1 tab_hidden)\" = 1 ]"
for i in 1 2 3 4 5; do stop "n$i"; done

# A reload repaints the tabs of all members.
x11 ta pos=100,100 size=600x200
x11 tb size=300x200
do_input key super+g
shown "two tabs" tb
TX=$((100 + B + T)) TY=$((100 + B))
pixel_is "the other tab's colour" $((TX + 8)) $((TY + 4)) aa0000
sed -i 's/Mwm\*ta\*background:.*/Mwm*ta*background: #aa00aa/' "$RC"
kill -HUP "$pid"
pixel_is "the other tab's new colour" $((TX + 8)) $((TY + 4)) aa00aa
stop ta
stop tb
finish
rm "$RC"

# ---- taking one window along ---------------------------------------------------------

begin
preview_is() { # description x y
	check "$1" "[ \"\$(global preview shown)\" = 1 ] && [ \"\$(global preview x)\" = $2 ] && \
		[ \"\$(global preview y)\" = $3 ]"
}
x11 t1 pos=100,100 size=300x200
x11 t2 pos=600,300 size=300x200
sleep 0.5
do_input move 750 $((300 + B + T / 2))
do_input down middle
do_input move 300 200
check "a drag with the middle button" "[ \"\$(global cursor mode)\" != 0 ]"
preview_is "the window it would join is marked" 100 100
check "the window stays where it is meanwhile" "[ \"\$(get t2 x)\" = 600 ] && [ \"\$(get t2 tab)\" = -1 ]"
do_input up middle
shown "dropped on it, anywhere: joined, and shown" t2
check "in its place" "[ \"\$(get t2 x)\" = 100 ] && [ \"\$(get t2 y)\" = 100 ] && [ \"\$(get t1 tab_hidden)\" = 1 ]"
check "the mark is gone" "[ \"\$(global preview shown)\" = 0 ]"

TX=$((100 + B + T)) TY=$((100 + B))
sleep 0.5
do_input move $((TX + 20)) $((TY + T / 2))
do_input down middle
do_input move $((TX + 60)) $((TY + 100))
check "over its own group" "[ \"\$(global preview shown)\" = 0 ]"
do_input up middle
check "dropped within its own group, nothing happens" "[ \"\$(get t1 tab)\" = 0 ] && [ \"\$(get t2 tab)\" = 1 ] && [ \"\$(get t2 tab_hidden)\" = 0 ]"

sleep 0.5
do_input move $((TX + 20)) $((TY + T / 2))
do_input down middle
do_input move $((TX + 620)) $((TY + 400))
do_input up middle
alone "a hidden tab dragged onto the desktop is on its own" t1
alone "the group is over" t2
check "the first went where it was dropped" "[ \"\$(get t1 x)\" = 700 ] && [ \"\$(get t1 y)\" = $((100 + 400 - T / 2)) ]"
check "the other one stayed" "[ \"\$(get t2 x)\" = 100 ] && [ \"\$(get t2 y)\" = 100 ]"
shown "the taken one has the focus" t1

# The outline shows the size it will have on its own.
x11 ob1 pos=500,20 size=500x150
x11 ob2 size=200x100
do_input key super+g
shown "a small window in a larger group" ob2
sleep 0.5
OTX=$((500 + B + T)) OTY=$((20 + B)) OTW=$(((500 - 3 * T) / 2))
do_input move $((OTX + OTW + 10)) $((OTY + T / 2))
do_input down middle
do_input move 1100 300
check "its outline has its own size" "[ \"\$(global outline w)\" = $((200 + 2 * B)) ] && [ \"\$(global outline h)\" = $((100 + 2 * B + T)) ]"
do_input up middle
check "and that is what it gets" "[ \"\$(get ob2 w)\" = 200 ] && [ \"\$(get ob2 h)\" = 100 ]"
alone "on its own" ob2
stop ob1
stop ob2
sleep 0.5

# Ctrl+left does the same, for touchpads.
sleep 0.5
do_input move 800 $((100 + 400 - T / 2 + B + T / 2))
do_input hold ctrl
do_input down left
do_input release ctrl
do_input move 200 200
preview_is "Ctrl+left drag" 100 100
do_input up left
shown "joins as well" t1
sleep 0.5
do_input move $((TX + 20)) $((TY + T / 2))
do_input hold ctrl
do_input down left
do_input release ctrl
do_input up left
shown "Ctrl+click on a tab shows it, as a click would" t2
sleep 0.5

# Escape, and a middle click.
do_input move $((TX + 20)) $((TY + T / 2))
do_input down middle
do_input move 900 500
do_input key Escape
check "Escape ends it" "[ \"\$(global cursor mode)\" = 0 ] && [ \"\$(global preview shown)\" = 0 ]"
do_input up middle
check "and nothing happened" "[ \"\$(get t2 tab)\" != -1 ] && [ \"\$(get t2 x)\" = 100 ]"
x11 t3 pos=500,50 size=200x100
do_input key alt+Tab
shown "the group on top" t2
do_input move $((TX + 20)) $((TY + T / 2))
sleep 0.5
do_input click middle
shown "a middle click lowers, and the other one is on top" t3
check "without touching the group" "[ \"\$(get t2 tab)\" != -1 ]"
stop t3

# A window that can't join: it goes there, over the other.
x11 fix pos=700,400 size=200x100 min=200x100 max=200x100
do_input key alt+Tab
sleep 0.5
do_input move $((TX + 20)) $((TY + T / 2))
do_input down middle
do_input move 800 450
check "no mark over a window that can't take it" "[ \"\$(global preview shown)\" = 0 ]"
do_input up middle
alone "so it is on its own" t2
alone "and so is the other" t1
stop fix

# The hidden window being taken along goes away.
stop t2
do_input key alt+Tab
x11 t4 size=200x100
do_input key super+g
shown "a group again" t4
hidden "with one behind" t1
sleep 0.5
refresh
TX=$(($(get t4 x) + B + T)) TY=$(($(get t4 y) + B))
do_input move $((TX + 5)) $((TY + T / 2))
do_input down middle
do_input move 900 600
stop t1
check "its drag is over" "[ \"\$(global cursor mode)\" = 0 ]"
do_input up middle
alone "the other is left on its own" t4
stop t4

# ---- the window menu -------------------------------------------------------------------

x11 m1 size=300x200
x11 m2 size=300x200
do_input key alt+space
do_input key t
shown "Tab With Previous Window" m2
do_input key alt+space
do_input key e
shown "Next Tab" m1
do_input key alt+space
do_input key p
shown "Previous Tab" m2
do_input key alt+space
do_input key d
alone "Detach Tab" m2
alone "both alone" m1
stop m1
stop m2

# ---- groups and fullscreen ------------------------------------------------------------

# fullgroup <main> <other>: a group at a known place, the other shown and fullscreen.
fullgroup() {
	wayland "$1" size=400x300
	refresh
	AX=$(get "$1" x) AY=$(get "$1" y)
	x11 "$2" size=300x200
	do_input key super+g
	shown "$2 is shown" "$2"
	do_input key alt+F11
	check "$2 is fullscreen" "[ \"\$(get $2 full)\" = 1 ] && [ \"\$(get $2 w)\" = 1280 ]"
}
place_is() { # description app x y w h
	check "$1" "[ \"\$(get $2 x)\" = $3 ] && [ \"\$(get $2 y)\" = $4 ] && \
		[ \"\$(get $2 w)\" = $5 ] && [ \"\$(get $2 h)\" = $6 ]"
}

fullgroup f1 f2
stop f1
alone "the hidden tab left, the group is over" f2
check "the last one stays fullscreen, at the size of the output" \
	"[ \"\$(get f2 full)\" = 1 ] && [ \"\$(get f2 w)\" = 1280 ] && [ \"\$(get f2 h)\" = 720 ]"
do_input key alt+F11
check "and goes back to its own size" \
	"[ \"\$(get f2 full)\" = 0 ] && [ \"\$(get f2 w)\" = 300 ] && [ \"\$(get f2 h)\" = 200 ]"
stop f2

fullgroup f3 f4
stop f4
alone "the fullscreen tab left, the group is over" f3
check "the other doesn't inherit fullscreen" "[ \"\$(get f3 full)\" = 0 ] && [ \"\$(get f3 frame)\" = 1 ]"
place_is "and is at the group's place and size" f3 "$AX" "$AY" 400 300
stop f3

wayland f5 size=400x300
wayland f6 size=300x200
do_input key super+g
shown "f6 is shown" f6
wayland f7 size=300x200
do_input key super+g
shown "f7 is shown" f7
do_input key alt+F11
check "f7 is fullscreen" "[ \"\$(get f7 full)\" = 1 ]"
stop f7
check "the group goes on, without fullscreen" \
	"[ \"\$(get f6 tab)\" != -1 ] && [ \"\$(get f6 full)\" = 0 ] && [ \"\$(get f6 w)\" = 400 ]"
stop f5
stop f6

fullgroup f8 f9
do_input key super+shift+g
check "taken out, it leaves fullscreen" "[ \"\$(get f9 full)\" = 0 ] && [ \"\$(get f9 frame)\" = 1 ]"
alone "and is alone" f9
alone "as is the other" f8
place_is "at its own size" f9 "$(get f9 x)" "$(get f9 y)" 300 200
place_is "the other at the group's place and size" f8 "$AX" "$AY" 400 300
check "and it can be seen" "[ \"\$(get f8 visible)\" = 1 ]"
stop f8
stop f9

do_input key super+t
check "tiling is on" "[ \"\$(global tiling on)\" = 1 ]"
fullgroup f10 f11
stop f11
alone "from the layout: the group is over" f10
check "and the other is back in the layout, not fullscreen" \
	"[ \"\$(get f10 full)\" = 0 ] && [ \"\$(get f10 tiled)\" = 1 ]"
stop f10
do_input key super+t
check "tiling is off" "[ \"\$(global tiling on)\" = 0 ]"
finish

echo "PASS: tabs"
