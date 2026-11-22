#!/bin/bash
# Headless mwmrc test: frame, title and desktop colours, the order of
# environment and file, reloading. Checks pixels in screenshots.
# Output goes to build/config.log. Exits non-zero on failure.
LOG=build/config.log
. "$(dirname "$0")/lib.sh"

require_tools foot grim
for tool in build/vinput build/pngtool; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
export MWC_TEST_STATE=$SMOKE_DIR/state
marker='touch "$SMOKE_DIR/$0"; exec sleep 120'

# The left border, away from the corner grooves and the bevels.
border_x() { refresh; echo $(($(get "$1" x) + 3)); }
border_y() { refresh; echo $(($(get "$1" y) + 100)); }

# border_is <description> <app> <rrggbb>
border_is() {
	pixel_is "$1" "$(border_x "$2")" "$(border_y "$2")" "$3"
}

# pixel_near <description> <x> <y> <rrggbb>: within 2 per channel, for colours
# that are computed and rounded.
pixel_near() {
	local what=$1 x=$2 y=$3 want=$((16#$4)) expected=$4 got i ok d shift
	for ((i = 0; i < 50; i++)); do
		WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/shot.png" 2>/dev/null || continue
		got=$(build/pngtool pixel "$SMOKE_DIR/shot.png" "$x" "$y")
		ok=1
		for shift in 0 8 16; do
			d=$(((((16#$got) >> shift) & 255) - ((want >> shift) & 255)))
			[ "$d" -ge -2 ] && [ "$d" -le 2 ] || ok=
		done
		[ -n "$ok" ] && { echo "ok: $what"; return 0; }
		sleep 0.1
	done
	fail "$what: pixel $x,$y is $got, expected about $expected"
}

border_near() {
	pixel_near "$1" "$(border_x "$2")" "$(border_y "$2")" "$3"
}

# title_has <description> <app> <rrggbb>: text drawn in that colour.
title_has() {
	local what=$1 app=$2 color=$3 n i x y w t
	refresh
	x=$(get "$app" x) y=$(get "$app" y) w=$(get "$app" w) t=$(global theme title)
	for ((i = 0; i < 50; i++)); do
		WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/shot.png" 2>/dev/null || continue
		# Between the buttons, so bevels are not counted.
		n=$(build/pngtool count "$SMOKE_DIR/shot.png" $((x + 6 + t + 3)) $((y + 8)) \
			$((w - 3 * t - 6)) $((t - 4)) "$color")
		[ "${n:-0}" -gt 0 ] && { echo "ok: $what"; return 0; }
		sleep 0.1
	done
	fail "$what: no pixel of colour $color in the title"
}

log_has() {
	grep -q -- "$2" "$LOG" || fail "$1: no \"$2\" in the log"
	echo "ok: $1"
}

# ---- colours from the file ---------------------------------------------------

cat >"$RC" <<'EOF'
! a comment
Mwm*activeBackground:           #102030
Mwm*background:                 gold
Mwm*rootBackground:             rgb:20/40/60

Mwm*foot*activeBackground:      firebrick
Mwm*foot*activeForeground:      #00ff00
Mwm*foot*background:            not-a-colour
Mwm*TERM*activeBackground:       #00aa00
Mwm*term*background:            #334455

this line has no colon
Other*activeBackground:         #ffffff
Mwm*fontList:                   Sans-12
Mwm*unknownResource:            whatever
EOF

start_mwc "grep SigBlk /proc/self/status > \"\$SMOKE_DIR/sigblk\"; \
	foot sh -c '$marker' foot-up & \
	while [ ! -e \"\$SMOKE_DIR/foot-up\" ]; do sleep 0.1; done; \
	exec foot --app-id=term -W 60x15 sh -c '$marker' term-up"
wait_for "foot connected (Wayland)" test -e "$SMOKE_DIR/foot-up"
wait_for "term connected" test -e "$SMOKE_DIR/term-up"
start_vinput
check "both windows are mapped" "[ -n \"\$(get foot x)\" ] && [ -n \"\$(get term x)\" ]"

wait_for "started programs have no signals blocked" \
	grep -q 'SigBlk:[[:space:]]*0*$' "$SMOKE_DIR/sigblk"

log_has "a line without a colon is reported" "mwmrc:12: missing ':'"
log_has "a wrong prefix is reported" "mwmrc:13: expected Mwm"
log_has "a font name without a size is reported" 'ignoring font "Sans-12"'
log_has "an unknown colour is reported" 'unknown colour "not-a-colour"'

# Move term next to foot, so both frames can be seen whichever has the focus.
B=$(global theme border)
T=$(global theme title)
tx=$(($(get term x) + ($(get term w) + 2 * B) / 2))
ty=$(($(get term y) + B + T / 2))
do_input move "$tx" "$ty"
do_input down left
do_input move $((tx + 730)) $((ty + 100))
do_input up left

pixel_is "rootBackground in rgb: form" 1270 700 204060
check "term has the focus" "[ \"\$(get term active)\" = 1 ]"
border_is "activeBackground by app_id, ignoring case" term 00aa00
# firebrick mixed with gold, because foot's own "background" is unusable
border_near "unfocused: a paler version of the client's colour" foot dc860f
# 00aa00 is a mid-tone at 26% of Motif's brightness, below its foregroundThreshold of 70%.
title_has "white title text on a mid-tone, as Motif picks it" term ffffff

do_input key alt+Tab
check "foot has the focus" "[ \"\$(get foot active)\" = 1 ]"
border_is "a colour name, by app_id" foot b22222
title_has "activeForeground" foot 00ff00
title_has "white title text on a dark background, unfocused" term ffffff
border_is "background by app_id" term 334455

# ---- the desktop icon wears the colours too ----------------------------------

do_input key alt+F9
check "foot is an icon" "[ \"\$(get foot min)\" = 1 ]"
refresh
pixel_near "the icon has the client's unfocused colour" \
	$(($(get foot icon_x) + 3)) $(($(get foot icon_y) + 30)) dc860f
do_input move $(($(get foot icon_x) + 30)) $(($(get foot icon_y) + 30))
do_input click left
do_input click left
check "foot is back" "[ \"\$(get foot min)\" = 0 ]"

# ---- reloading ---------------------------------------------------------------

# maximize first: the frame must stay inside the screen when the title grows
sleep 0.45
do_input key alt+F10
check "foot is maximized" "[ \"\$(get foot max)\" = 1 ]"

cat >"$RC" <<'EOF'
Mwm*activeBackground:   #aa0000
Mwm*background:         #00aa00
Mwm*rootBackground:     #445566
Mwm*fontList:           Sans Bold 30
EOF
reload_mwc
check "SIGHUP: the title bar grows with the font" "[ \"\$(global theme title)\" -gt 30 ]"
check "SIGHUP: a maximized window still fits the screen" \
	"[ \$((\$(get foot h) + 2 * B + \$(global theme title))) = 720 ] && [ \"\$(get foot max)\" = 1 ]"
border_is "SIGHUP: per-client entry gone, global colour" foot aa0000
do_input key alt+F10
check "foot is no longer maximized" "[ \"\$(get foot max)\" = 0 ]"
pixel_is "SIGHUP: new desktop colour" 1270 700 445566
border_is "SIGHUP: other window has the new colour" term 00aa00

build/pngtool stripes "$SMOKE_DIR/wall.png" 1280 720 ff0000 0000ff
cat >"$RC" <<EOF
Mwm*rootBackground:     #123456
Mwm*wallpaper:          $SMOKE_DIR/wall.png
EOF
do_input move 1270 700
do_input click right
check "the root menu is open" "[ \"\$(global menu open)\" = 1 ]"
do_input key r
check "Reload asks nothing" "[ \"\$(global menu open)\" = 0 ]"
pixel_is "Reload: wallpaper from the file" 1270 700 ff0000
check "the window is still there" "[ -n \"\$(get foot x)\" ] && [ -n \"\$(get term x)\" ]"

rm "$RC"
reload_mwc
pixel_is "without the file: back to the defaults" 1270 700 7189a3
check "the title bar is back to normal" "[ \"\$(global theme title)\" = $T ]"
log_has "a reload is logged" "reloading the configuration"
stop_vinput
stop_mwc

# ---- the environment wins ----------------------------------------------------

build/pngtool stripes "$SMOKE_DIR/other.png" 1280 720 00ff00 00ff00
cat >"$RC" <<EOF
Mwm*rootBackground:     #123456
Mwm*wallpaper:          $SMOKE_DIR/wall.png
EOF
export MWC_ROOT='#010203'
export MWC_WALLPAPER=$SMOKE_DIR/other.png
start_mwc "sleep 120"
pixel_is "MWC_WALLPAPER beats Mwm*wallpaper" 640 360 00ff00
stop_mwc
unset MWC_WALLPAPER
rm "$RC"
start_mwc "sleep 120"
pixel_is "MWC_ROOT without a file" 640 360 010203
stop_mwc
export MWC_ROOT=gold
start_mwc "sleep 120"
pixel_is "a colour name in the environment" 640 360 ffd700
stop_mwc
export MWC_ROOT=not-a-colour
start_mwc "sleep 120"
pixel_is "an unusable value falls back" 640 360 7189a3
log_has "an unusable value is reported" 'ignoring MWC_ROOT=not-a-colour'
stop_mwc
unset MWC_ROOT

# The file against the environment, for what a window shows as well.
cat >"$RC" <<'EOF'
Mwm*rootBackground:     #123456
Mwm*activeBackground:   #102030
Mwm*fontList:           Sans Bold 30
EOF
export MWC_ROOT='#010203' MWC_ACTIVE='#405060' MWC_FONT='Sans Bold 10'
rm -f "$SMOKE_DIR/foot-up"
start_mwc "exec foot sh -c '$marker' foot-up"
wait_for "foot connected" test -e "$SMOKE_DIR/foot-up"
check "foot is mapped" "[ -n \"\$(get foot x)\" ]"
pixel_is "MWC_ROOT beats Mwm*rootBackground" 1270 700 010203
border_is "MWC_ACTIVE beats Mwm*activeBackground" foot 405060
check "MWC_FONT beats Mwm*fontList" "[ \"\$(global theme title)\" = $T ]"
stop_mwc
unset MWC_ROOT MWC_ACTIVE MWC_FONT

# ---- programs in the root menu ------------------------------------------------------

ran() { test -e "$SMOKE_DIR/ran-$1"; }
root_menu_open() {
	do_input move 1279 719
	do_input click right
	check "the root menu is open" "[ \"\$(global menu open)\" = 1 ]"
}
cat >"$RC" <<EOF
Mwm*rootMenuItem:   Firefox: touch "$SMOKE_DIR/ran-first"
Mwm*rootMenuItem:   Dateien: touch "$SMOKE_DIR/ran-second:with-a-colon"
Mwm*rootMenuItem:   Fotos: touch "$SMOKE_DIR/ran-fotos"
Mwm*rootMenuItem:   Quant: touch "$SMOKE_DIR/ran-noletter"
Mwm*rootMenuItem:   no command here
Mwm*rootMenuItem:   : touch "$SMOKE_DIR/ran-nolabel"
Mwm*rootMenuItem:   Nothing:
Mwm*foot*rootMenuItem: Client: touch "$SMOKE_DIR/ran-client"
EOF
start_mwc "exec sleep 120"
start_vinput
log_has "a line without a command is reported" "mwmrc:5: expected Mwm\*rootMenuItem"
log_has "a line without a label" "mwmrc:6: expected Mwm\*rootMenuItem"
log_has "an empty command" "mwmrc:7: expected Mwm\*rootMenuItem"
root_menu_open
check "on top with the first free letters of each, in order, without those that are bad" \
	"[ \"\$(global menu sensitive | cut -c1-4)\" = FeoN ]"
do_input key f
wait_for "the key of the first item runs it" ran first
check "and the menu closes" "[ \"\$(global menu open)\" = 0 ]"
root_menu_open
do_input key e
wait_for "a letter that wasn't taken yet, and a command with a colon" ran second:with-a-colon
root_menu_open
do_input key o
wait_for "a first letter taken by the item above moves on" ran fotos
root_menu_open
do_input key Down
do_input key Down
do_input key Down
do_input key Down
do_input key Return
wait_for "with all of its letters taken, it can still be chosen" ran noletter
rm -f "$SMOKE_DIR/ran-first"
root_menu_open
do_input key Down
do_input key Return
wait_for "the arrow keys reach them too" ran first

# A reload takes up changes.
rm -f "$SMOKE_DIR"/ran-*
cat >"$RC" <<EOF
Mwm*rootMenuItem:   Zebra: touch "$SMOKE_DIR/ran-zebra"
EOF
reload_mwc
root_menu_open
check "a reload changes the items" "[ \"\$(global menu sensitive | cut -c1-2)\" = ZN ]"
WITH=$(global menu items)
do_input key z
wait_for "which run" ran zebra
# A reload while the menu is open frees the items it shows: it closes first, and its
# keys run nothing afterwards.
rm -f "$SMOKE_DIR"/ran-*
root_menu_open
rm "$RC"
reload_mwc
check "a reload closes the open menu" "[ \"\$(global menu open)\" = 0 ]"
do_input key z
sleep 0.3
[ -e "$SMOKE_DIR/ran-zebra" ] && fail "a key ran an item of the old menu"
kill -0 "$pid" || fail "mwc died"
echo "ok: and nothing of it runs"
root_menu_open
check "without any, the menu is as before" "[ \"\$(global menu sensitive | cut -c1)\" = N ]"
[ "$WITH" = $(($(global menu items) + 2)) ] || fail "there is no separator below the items"
echo "ok: with one item there were two more: the item and a separator"
do_input key Escape
stop_vinput
stop_mwc

# More than there is room for.
rm -f "$SMOKE_DIR"/ran-*
for i in $(seq 1 25); do
	echo "Mwm*rootMenuItem: Item$i: touch \"$SMOKE_DIR/ran-$i\""
done >"$RC"
start_mwc "exec sleep 120"
start_vinput
log_has "more than can be used is reported" "only the first 20"
# Arming an item moves its raised shadow there; the menu itself, with its text, is drawn
# once (Review 2026-11-18 #21). Left of the label, the top of an armed row is the shadow,
# its middle the menu's background.
root_menu_open
r=$(global menu renders)
mx=$(($(global menu x) + 7))
do_input key Down
check "the first item is armed" "[ \"\$(global menu armed)\" = 0 ]"
y1=$(global menu ay)
WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/shot.png"
bg=$(build/pngtool pixel "$SMOKE_DIR/shot.png" $mx $((y1 + 12)))
pixel_isnt "the first item has a shadow" $mx $y1 "$bg"
top=$(build/pngtool pixel "$SMOKE_DIR/shot.png" $mx $y1)
for i in $(seq 2 20); do do_input key Down; done
check "the twentieth is armed" "[ \"\$(global menu armed)\" = 19 ]"
y20=$(global menu ay)
pixel_is "the twentieth has it" $mx $y20 "$top"
pixel_is "the first has it no more" $mx $y1 "$bg"
[ "$(global menu renders)" = "$r" ] ||
	fail "arming 20 items drew the menu $(($(global menu renders) - r)) times more"
echo "ok: arming 20 items drew the menu no more"
do_input key Return
wait_for "the twentieth is there" ran 20
root_menu_open
for i in $(seq 1 21); do do_input key Down; done
check "and after it the menu goes on" "[ \"\$(global menu open)\" = 1 ]"
do_input key Return
sleep 0.5
ran 21 && fail "the twenty-first is there"
echo "ok: the twenty-first isn't"
stop_vinput
stop_mwc
rm "$RC"

# ---- later lines win, and a reload redoes everything ----------------------------------

build/pngtool stripes "$SMOKE_DIR/same.png" 1280 720 ff0000 ff0000
cat >"$RC" <<EOF
Mwm*rootBackground:         #111111
Mwm*rootBackground:         #222222
Mwm*activeBackground:       #333333
Mwm*activeBackground:       #440000
Mwm*foreground:             #ff00ff
Mwm*activeForeground:       #00ff00
Mwm*foot*activeBackground:  #005500
Mwm*foot*activeBackground:  #006600
Mwm*foot*background:        #aa0000
EOF
rm -f "$SMOKE_DIR/foot-up" "$SMOKE_DIR/term-up"
start_mwc "foot sh -c '$marker' foot-up & \
	while [ ! -e \"\$SMOKE_DIR/foot-up\" ]; do sleep 0.1; done; \
	exec foot --app-id=term -W 60x15 sh -c '$marker' term-up"
wait_for "foot connected" test -e "$SMOKE_DIR/foot-up"
wait_for "term connected" test -e "$SMOKE_DIR/term-up"
start_vinput
check "both windows are mapped" "[ -n \"\$(get foot x)\" ] && [ -n \"\$(get term x)\" ]"
check "term has the focus" "[ \"\$(get term active)\" = 1 ]"
pixel_is "the later of two global lines" 1270 700 222222
border_is "for frames as well" term 440000
title_has "global activeForeground" term 00ff00
title_has "global foreground, on a window that isn't focused" foot ff00ff
do_input key alt+Tab
check "foot has the focus" "[ \"\$(get foot active)\" = 1 ]"
border_is "the later of two lines for a client" foot 006600

do_input key alt+F9
check "foot is an icon" "[ \"\$(get foot min)\" = 1 ]"
refresh
pixel_near "the icon has its colour" $(($(get foot icon_x) + 3)) $(($(get foot icon_y) + 30)) aa0000
sed -i 's/Mwm\*foot\*background:.*/Mwm*foot*background: #0000aa/' "$RC"
echo "Mwm*wallpaper: $SMOKE_DIR/same.png" >>"$RC"
reload_mwc
pixel_near "a reload paints the icon anew" $(($(get foot icon_x) + 3)) $(($(get foot icon_y) + 30)) 0000aa
pixel_is "the wallpaper comes" 1270 700 ff0000
# Same name, same size, other picture: only rendering it again can show that.
build/pngtool stripes "$SMOKE_DIR/same.png" 1280 720 00ff00 00ff00
reload_mwc
pixel_is "a reload reads the wallpaper again" 1270 700 00ff00

# ---- iconPlacement ------------------------------------------------------------

do_input key alt+F9
check "term is an icon too" "[ \"\$(get term min)\" = 1 ]"
# icons_at <description> <x,y> <x,y>: foot and term in these two slots, either way
icons_at() {
	check "$1" "p=\"\$(get foot icon_x),\$(get foot icon_y) \$(get term icon_x),\$(get term icon_y)\"; \
		[ \"\$p\" = \"$2 $3\" ] || [ \"\$p\" = \"$3 $2\" ]"
}
icons_at "by default along the bottom from the left" 8,640 88,640
echo "Mwm*iconPlacement: top right" >>"$RC"
reload_mwc
icons_at "top right: down the right edge" 1200,8 1200,88
echo "Mwm*iconPlacement: right top tight" >>"$RC"
reload_mwc
icons_at "right top tight: along the top from the right, no gap" 1200,8 1128,8
# In the middle of an edge, as one block: two icons of 72 with 8 between are 152
# high, so on 720 they start at 284.
echo "Mwm*iconPlacement: center right" >>"$RC"
reload_mwc
icons_at "center right: both together in the middle of the right edge" 1200,284 1200,364
echo "Mwm*iconPlacement: right center" >>"$RC"
reload_mwc
icons_at "right center is the same" 1200,284 1200,364
# One goes: the other closes up to the middle. It comes back: both as before.
do_input move $(($(get foot icon_x) + 30)) $(($(get foot icon_y) + 30))
do_input click left
do_input click left
check "foot is back" "[ \"\$(get foot min)\" = 0 ]"
check "the other icon is in the middle now" "[ \"\$(get term icon_x),\$(get term icon_y)\" = 1200,324 ]"
do_input key alt+F9
check "foot is an icon again" "[ \"\$(get foot min)\" = 1 ]"
check "first again, as it became an icon first" \
	"[ \"\$(get foot icon_y)\" = 284 ] && [ \"\$(get term icon_y)\" = 364 ]"
echo "Mwm*iconPlacement: center bottom tight" >>"$RC"
reload_mwc
icons_at "center bottom tight: along the bottom in the middle, no gap" 568,640 640,640
echo "Mwm*iconPlacement: center center" >>"$RC"
reload_mwc
wait_for "center twice is reported" grep -q 'not "center center"' "$LOG"
echo "Mwm*iconPlacement: left right" >>"$RC"
reload_mwc
wait_for "a wrong iconPlacement is reported" grep -q 'iconPlacement: expected a placement' "$LOG"
icons_at "and the default is used" 8,640 88,640
echo "Mwm*iconPlacement: top right" >>"$RC"
reload_mwc
icons_at "top right again" 1200,8 1200,88
do_input key super+t
do_input move $(($(get term icon_x) + 30)) $(($(get term icon_y) + 30))
do_input click left
do_input click left
check "term is back and tiled" "[ \"\$(get term min)\" = 0 ] && [ \"\$(get term tiled)\" = 1 ]"
check "the layout ends left of the icon column" \
	"[ \$((\$(get term x) + \$(get term w) + 2 * $B)) -le \$(get foot icon_x) ]"
check "and keeps the full height" "[ \$((\$(get term y) + 300)) -lt 640 ] && \
	[ \$((\$(get term y) + \$(get term h))) -gt 640 ]"
do_input key super+t
stop_vinput
stop_mwc

echo "PASS: config"
