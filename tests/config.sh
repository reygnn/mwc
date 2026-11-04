#!/bin/bash
# Headless mwmrc test: frame, title and desktop colours, the order of
# environment and file, reloading. Checks pixels in screenshots.
# Output goes to build/config.log. Exits non-zero on failure.
LOG=build/config.log
. "$(dirname "$0")/lib.sh"

require_tools foot xterm Xwayland grim
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
Mwm*XTERM*activeBackground:     #00aa00
Mwm*myterm*background:          #334455

this line has no colon
Other*activeBackground:         #ffffff
Mwm*fontList:                   Sans-12
Mwm*unknownResource:            whatever
EOF

start_mwc "grep SigBlk /proc/self/status > \"\$SMOKE_DIR/sigblk\"; \
	foot sh -c '$marker' foot-up & \
	while [ ! -e \"\$SMOKE_DIR/foot-up\" ]; do sleep 0.1; done; \
	exec xterm -name myterm -e sh -c '$marker' xterm-up"
wait_for "foot connected (Wayland)" test -e "$SMOKE_DIR/foot-up"
wait_for "xterm connected (XWayland)" test -e "$SMOKE_DIR/xterm-up"
start_vinput
check "both windows are mapped" "[ -n \"\$(get foot x)\" ] && [ -n \"\$(get XTerm x)\" ]"

wait_for "started programs have no signals blocked" \
	grep -q 'SigBlk:[[:space:]]*0*$' "$SMOKE_DIR/sigblk"

log_has "a line without a colon is reported" "mwmrc:12: missing ':'"
log_has "a wrong prefix is reported" "mwmrc:13: expected Mwm"
log_has "a font name without a size is reported" 'ignoring font "Sans-12"'
log_has "an unknown colour is reported" 'unknown colour "not-a-colour"'

# Move xterm next to foot, so both frames can be seen whichever has the focus.
B=$(global theme border)
T=$(global theme title)
tx=$(($(get XTerm x) + ($(get XTerm w) + 2 * B) / 2))
ty=$(($(get XTerm y) + B + T / 2))
do_input move "$tx" "$ty"
do_input down left
do_input move $((tx + 730)) $((ty + 100))
do_input up left

pixel_is "rootBackground in rgb: form" 1270 700 204060
check "xterm has the focus" "[ \"\$(get XTerm active)\" = 1 ]"
border_is "activeBackground by WM_CLASS class, ignoring case" XTerm 00aa00
# firebrick mixed with gold, because foot's own "background" is unusable
border_near "unfocused: a paler version of the client's colour" foot dc860f
title_has "white title text on a dark background" XTerm ffffff

do_input key alt+Tab
check "foot has the focus" "[ \"\$(get foot active)\" = 1 ]"
border_is "a colour name, by app_id" foot b22222
title_has "activeForeground" foot 00ff00
border_is "background by WM_CLASS instance" XTerm 334455

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
kill -HUP "$pid"
check "SIGHUP: the title bar grows with the font" "[ \"\$(global theme title)\" -gt 30 ]"
check "SIGHUP: a maximized window still fits the screen" \
	"[ \$((\$(get foot h) + 2 * B + \$(global theme title))) = 720 ] && [ \"\$(get foot max)\" = 1 ]"
border_is "SIGHUP: per-client entry gone, global colour" foot aa0000
do_input key alt+F10
check "foot is no longer maximized" "[ \"\$(get foot max)\" = 0 ]"
pixel_is "SIGHUP: new desktop colour" 1270 700 445566
border_is "SIGHUP: other window has the new colour" XTerm 00aa00

build/pngtool stripes "$SMOKE_DIR/wall.png" 1280 720 ff0000 0000ff
cat >"$RC" <<EOF
Mwm*rootBackground:     #123456
Mwm*wallpaper:          $SMOKE_DIR/wall.png
EOF
do_input move 1270 700
do_input click right
check "the root menu is open" "[ \"\$(global menu open)\" = 1 ]"
do_input key r
check "Restart... asks first" "[ \"\$(global menu open)\" = 1 ] && [ \"\$(global menu title)\" = yes ]"
do_input key c
pixel_is "Restart... then Cancel keeps the old settings" 1270 700 445566
do_input click right
do_input key r
do_input key o
pixel_is "Restart... then OK: wallpaper from the file" 1270 700 ff0000
check "the window is still there" "[ -n \"\$(get foot x)\" ] && [ -n \"\$(get XTerm x)\" ]"

rm "$RC"
kill -HUP "$pid"
pixel_is "without the file: back to the defaults" 1270 700 5b7792
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
pixel_is "an unusable value falls back" 640 360 5b7792
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
rm -f "$SMOKE_DIR/foot-up" "$SMOKE_DIR/xterm-up"
start_mwc "foot sh -c '$marker' foot-up & \
	while [ ! -e \"\$SMOKE_DIR/foot-up\" ]; do sleep 0.1; done; \
	exec xterm -e sh -c '$marker' xterm-up"
wait_for "foot connected" test -e "$SMOKE_DIR/foot-up"
wait_for "xterm connected" test -e "$SMOKE_DIR/xterm-up"
start_vinput
check "both windows are mapped" "[ -n \"\$(get foot x)\" ] && [ -n \"\$(get XTerm x)\" ]"
check "xterm has the focus" "[ \"\$(get XTerm active)\" = 1 ]"
pixel_is "the later of two global lines" 1270 700 222222
border_is "for frames as well" XTerm 440000
title_has "global activeForeground" XTerm 00ff00
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
kill -HUP "$pid"
pixel_near "a reload paints the icon anew" $(($(get foot icon_x) + 3)) $(($(get foot icon_y) + 30)) 0000aa
pixel_is "the wallpaper comes" 1270 700 ff0000
# Same name, same size, other picture: only rendering it again can show that.
build/pngtool stripes "$SMOKE_DIR/same.png" 1280 720 00ff00 00ff00
kill -HUP "$pid"
pixel_is "a reload reads the wallpaper again" 1270 700 00ff00
stop_vinput
stop_mwc

echo "PASS: config"
