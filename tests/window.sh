#!/bin/bash
# Headless test of window behaviour: size hints, dialogs, places from mwmrc and
# the frame, with a client that can ask for each of them (tests/xdgc.c).
# Output goes to build/window.log. Exits non-zero on failure.
LOG=build/window.log
. "$(dirname "$0")/lib.sh"

require_tools grim
for tool in build/vinput build/pngtool build/xdgc build/layerc; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
start_mwc 'exec sleep 300'
start_vinput
refresh
B=$(global theme border)
T=$(global theme title)
THIN=$(global theme thin)

PIDS=()
wayland() {
	local name=$1
	shift
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$name" "$@" >"$SMOKE_DIR/$name.events" 2>&1 &
	eval "PID_$name=$!"
}
stop() {
	local var="PID_$1"
	kill "${!var}" 2>/dev/null
	wait "${!var}" 2>/dev/null
	check "$1 is gone" "[ -z \"\$(get $1 x)\" ]"
}
mapped() { check "$1 is mapped" "[ -n \"\$(get $1 x)\" ]"; }
saw() { wait_for "$1" grep -q "$3" "$SMOKE_DIR/$2.events"; }
settle() { sleep 0.45; }

# The left and top of the frame, and the size of its parts.
border() { get "$1" bw; }
title() { get "$1" th; }
outer_w() { echo $(($(get "$1" w) + 2 * $(border "$1"))); }
outer_h() { echo $(($(get "$1" h) + 2 * $(border "$1") + $(title "$1"))); }

drag() { # from_x from_y to_x to_y [button]
	local button=${5:-left}
	do_input move "$1" "$2"
	do_input down "$button"
	do_input move "$3" "$4"
	do_input up "$button"
}

# Drag the bottom-right corner by dx dy.
resize_by() { # app dx dy
	refresh
	local x=$(($(get "$1" x) + $(outer_w "$1") - 2))
	local y=$(($(get "$1" y) + $(outer_h "$1") - 2))
	drag "$x" "$y" $((x + $2)) $((y + $3))
}

sensitive_is() { # description window expected, leaving out the entries for tabs and desktops
	do_input key alt+space
	check "$1" "[ \"\$(global menu sensitive | tr -d TePD1234O)\" = $3 ]"
	do_input key Escape
}

# ---- size hints ---------------------------------------------------------------

wayland wlimit size=300x200 min=200x150 max=400x300
mapped wlimit
resize_by wlimit 500 500
check "the maximum size holds" "[ \"\$(get wlimit w)\" = 400 ] && [ \"\$(get wlimit h)\" = 300 ]"
resize_by wlimit -500 -500
check "the minimum size holds" "[ \"\$(get wlimit w)\" = 200 ] && [ \"\$(get wlimit h)\" = 150 ]"
do_input key alt+F10
check "maximizing stops at the maximum size" \
	"[ \"\$(get wlimit max)\" = 1 ] && [ \"\$(get wlimit w)\" = 400 ] && [ \"\$(get wlimit h)\" = 300 ]"
do_input key alt+F10
check "and goes back" "[ \"\$(get wlimit max)\" = 0 ] && [ \"\$(get wlimit w)\" = 200 ]"
stop wlimit

# A minimum size of INT32_MAX, which wlroots lets through: what mwc asks for stays at
# its own limit, 2^20, and the frame around it adds up.
wayland whuge size=300x200 min=2147483647x2147483647 own_size=1 states=1
mapped whuge
do_input key alt+F10
wait_for "maximized" grep -q '^configure maximized' "$SMOKE_DIR/whuge.events"
check "a huge minimum size, capped" \
	"grep '^configure maximized' \"\$SMOKE_DIR/whuge.events\" | tail -1 | \
	grep -qx 'configure maximized 1048576x1048576'"
do_input key alt+F10
check "and back" "[ \"\$(get whuge max)\" = 0 ]"
refresh
X=$(get whuge x) Y=$(get whuge y)
do_input key super+t
check "tiling leaves it out, where it was" \
	"[ \"\$(global tiling on)\" = 1 ] && [ \"\$(get whuge tiled)\" = 0 ] && \
	[ \"\$(get whuge x)\" = $X ] && [ \"\$(get whuge y)\" = $Y ]"
do_input key super+t
stop whuge

# ---- a window that cannot be resized ------------------------------------------

wayland fixed size=300x200 min=300x200 max=300x200
mapped fixed
check "a fixed size means a thin border" "[ \"\$(border fixed)\" = $THIN ] && [ \"\$(title fixed)\" = $T ]"
sensitive_is "and no Size or Maximize in the menu" fixed MnLC
do_input key alt+F10
do_input key alt+F8
sleep 0.2
check "Alt+F10 and Alt+F8 do nothing" \
	"[ \"\$(get fixed max)\" = 0 ] && [ \"\$(global cursor mode)\" = 0 ]"
refresh
resize_by fixed 100 100
check "its border doesn't resize" "[ \"\$(get fixed w)\" = 300 ] && [ \"\$(get fixed h)\" = 200 ]"
stop fixed

# ---- dialogs ------------------------------------------------------------------

wayland parent size=600x400 dialog=child
check "dialog and main window are mapped" "[ -n \"\$(get parent x)\" ] && [ -n \"\$(get child x)\" ]"
check "the dialog gets the focus" "[ \"\$(get child kbfocus)\" = 1 ]"
check "the dialog is centred over its window" \
	"[ \$((\$(get child x) + \$(outer_w child) / 2)) = \$((\$(get parent x) + \$(outer_w parent) / 2)) ] && \
	[ \$((\$(get child y) + \$(outer_h child) / 2)) = \$((\$(get parent y) + \$(outer_h parent) / 2)) ]"
sensitive_is "a dialog has no Minimize or Maximize" child MSLC

do_input move $(($(get parent x) + 100)) $(($(get parent y) + 12))
settle
do_input click left
check "a click on the main window focuses it, the dialog stays above" \
	"[ \"\$(get parent kbfocus)\" = 1 ] && [ \"\$(stack_pos child)\" = 0 ] && [ \"\$(stack_pos parent)\" = 1 ]"

wayland other size=300x200
mapped other
check "another window goes to the front" "[ \"\$(stack_pos other)\" = 0 ]"
# Where the cascade put it, it leaves the main window's title bar there to click.
px=$(($(get parent x) + 100)) py=$(($(get parent y) + 12))
[ "$py" -lt "$(get other y)" ] || [ "$px" -lt "$(get other x)" ] ||
	fail "the other window covers the main window's title bar at $px,$py"
do_input move $px $py
settle
do_input click left
check "raising the main window brings its dialog along" \
	"[ \"\$(stack_pos child)\" = 0 ] && [ \"\$(stack_pos parent)\" = 1 ] && [ \"\$(stack_pos other)\" = 2 ]"

do_input key alt+F9
check "minimizing the main window takes the dialog along, without an icon" \
	"[ \"\$(get parent min)\" = 1 ] && [ \"\$(get child min)\" = 0 ] && [ \"\$(get child hidden)\" = 1 ]"
check "the focus goes to the other window" "[ \"\$(get other kbfocus)\" = 1 ]"
do_input key alt+Tab
sleep 0.1
check "Alt+Tab skips the hidden dialog" "[ \"\$(get other active)\" = 1 ] && [ \"\$(get child active)\" = 0 ]"
do_input move $(($(get parent icon_x) + 30)) $(($(get parent icon_y) + 30))
settle
do_input click left
do_input click left
check "restoring it brings the dialog back, above it" \
	"[ \"\$(get parent min)\" = 0 ] && [ \"\$(get child hidden)\" = 0 ] && \
	[ \"\$(stack_pos child)\" = 0 ] && [ \"\$(stack_pos parent)\" = 1 ]"
stop other

do_input move $(($(get child x) + 70)) $(($(get child y) + 12))
settle
do_input click left
check "the dialog has the focus" "[ \"\$(get child kbfocus)\" = 1 ]"
do_input key alt+F4
check "closing the dialog gives the focus to its window" \
	"[ -z \"\$(get child x)\" ] && [ \"\$(get parent kbfocus)\" = 1 ]"
stop parent

# Closing a minimized window must not leave its dialog hidden for good.
wayland orphanmain size=400x300 dialog=orphan
check "main window and dialog are mapped" "[ -n \"\$(get orphanmain x)\" ] && [ -n \"\$(get orphan x)\" ]"
bring_main() { for _ in 1 2 3; do refresh; [ "$(get orphanmain active)" = 1 ] && return; do_input key alt+Tab; done; }
bring_main
do_input key alt+F9
check "the dialog is hidden" "[ \"\$(get orphan hidden)\" = 1 ]"
do_input move $(($(get orphanmain icon_x) + 30)) $(($(get orphanmain icon_y) + 30))
settle
do_input click right
do_input key c
check "closing the minimized window brings the dialog back" \
	"[ -z \"\$(get orphanmain x)\" ] && [ \"\$(get orphan hidden)\" = 0 ] && [ \"\$(get orphan kbfocus)\" = 1 ]"
do_input key alt+F4
check "the dialog closes" "[ -z \"\$(get orphan x)\" ]"
wait "$PID_orphanmain" 2>/dev/null

# A dialog of a dialog, hidden with their window: the dialog between going away doesn't
# bring it back while the window is an icon.
mkfifo "$SMOKE_DIR/nest.ctl"
exec {nestfd}<>"$SMOKE_DIR/nest.ctl"
wayland nest dialog=nestmid subdialog=nestkid control="$SMOKE_DIR/nest.ctl"
check "a window, its dialog and the dialog's dialog" "[ -n \"\$(get nestkid x)\" ]"
do_input move $(($(get nest x) + 30)) $(($(get nest y) + $(get nest th) + 30))
do_input click left
check "the window has the focus" "[ \"\$(get nest active)\" = 1 ]"
do_input key alt+F9
check "an icon, both dialogs hidden with it" "[ \"\$(get nest min)\" = 1 ] && \
	[ \"\$(get nestmid hidden)\" = 1 ] && [ \"\$(get nestkid hidden)\" = 1 ]"
echo undialog >&$nestfd
saw "the dialog between goes" nest '^did 1'
check "the dialog between is gone" "[ -z \"\$(get nestmid x)\" ]"
check "the other stays hidden while its window is an icon" \
	"[ \"\$(get nestkid hidden)\" = 1 ] && [ \"\$(get nestkid visible)\" = 0 ]"
do_input move $(($(get nest icon_x) + 30)) $(($(get nest icon_y) + 30))
do_input dclick left
check "and comes back with it" "[ \"\$(get nest min)\" = 0 ] && [ \"\$(get nestkid visible)\" = 1 ]"
stop nest
exec {nestfd}>&-

# A dialog appearing while its window is an icon waits for it.
wayland kidp dialog=kid dialog_after="$SMOKE_DIR/kidp.open"
mapped kidp
do_input key alt+F9
check "the parent is an icon" "[ \"\$(get kidp min)\" = 1 ]"
touch "$SMOKE_DIR/kidp.open"
saw "its dialog opens" kidp '^dialog opened'
check "a dialog appearing now stays hidden" \
	"[ -n \"\$(get kid x)\" ] && [ \"\$(get kid hidden)\" = 1 ] && [ \"\$(get kid active)\" = 0 ]"
do_input move $(($(get kidp icon_x) + 30)) $(($(get kidp icon_y) + 30))
settle
do_input click left
do_input click left
check "and comes back with it" "[ \"\$(get kidp min)\" = 0 ] && [ \"\$(get kid hidden)\" = 0 ]"
stop kidp

# ---- placement ----------------------------------------------------------------

RC=$XDG_CONFIG_HOME/mwc/mwmrc
mkdir -p "$(dirname "$RC")"
cat >"$RC" <<'EOF'
Mwm*gcorner*geometry:   -0-0
Mwm*gleft*geometry:     +20+30
Mwm*gbig*geometry:      -0-0
Mwm*gdialog*geometry:   +0+0
Mwm*gsized*geometry:    100x100-0-0
Mwm*ghalf*geometry:     -0
Mwm*gtab*geometry:      -0-0
Mwm*gmaxed*geometry:    -0-0
Mwm*gpointer*geometry:  pointer
Mwm*gpedge*geometry:    Pointer
Mwm*gwgrow*geometry:    -0-0
Mwm*gwleft*geometry:    +20+30
Mwm*gwmoved*geometry:   -0-0
Mwm*gwsized*geometry:   -0-0
Mwm*gwframe*geometry:   -0-0
Mwm*gwfleft*geometry:   +20+30
Mwm*gwfont*geometry:    -0-0
Mwm*gplain*geometry:    -0-0
EOF
reload_mwc

right() { echo $(($(get "$1" x) + $(outer_w "$1"))); }
bottom() { echo $(($(get "$1" y) + $(outer_h "$1"))); }

wayland gcorner
mapped gcorner
check "-0-0 puts a window into the corner" \
	"[ \"\$(right gcorner)\" = 1280 ] && [ \"\$(bottom gcorner)\" = 720 ]"
drag $(($(get gcorner x) + 100)) $(($(get gcorner y) + 12)) 500 300
check "it can be moved away" "[ \"\$(right gcorner)\" != 1280 ]"
moved_x=$(get gcorner x)
reload_mwc
check "a reload leaves it there" "[ \"\$(get gcorner x)\" = $moved_x ]"
stop gcorner

# Hidden and shown again, it goes back to its entry's place.
wayland gplain rehide_after="$SMOKE_DIR/gplain.hide" reshow_after="$SMOKE_DIR/gplain.show"
mapped gplain
check "it starts in its corner" "[ \"\$(right gplain)\" = 1280 ] && [ \"\$(bottom gplain)\" = 720 ]"
drag $(($(get gplain x) + 100)) $(($(get gplain y) + 12)) 500 300
check "it can be moved" "[ \"\$(right gplain)\" != 1280 ]"
touch "$SMOKE_DIR/gplain.hide"
check "it is hidden" "[ -z \"\$(get gplain x)\" ]"
touch "$SMOKE_DIR/gplain.show"
check "shown again, it is in its corner again" \
	"[ -n \"\$(get gplain x)\" ] && [ \"\$(right gplain)\" = 1280 ] && \
	 [ \"\$(bottom gplain)\" = 720 ]"
stop gplain

# Hidden as an icon or snapped, it comes back as a plain window, with its title drawn.
for how in alt+F9 super+Left; do
	rm -f "$SMOKE_DIR/remap.hide" "$SMOKE_DIR/remap.show"
	wayland remap rehide_after="$SMOKE_DIR/remap.hide" reshow_after="$SMOKE_DIR/remap.show"
	mapped remap
	do_input key $how
	check "$how: an icon or snapped" \
		"[ \"\$(get remap min)\" = 1 ] || [ \"\$(get remap snap)\" != 0 ]"
	touch "$SMOKE_DIR/remap.hide"
	check "$how: hidden" "[ -z \"\$(get remap x)\" ]"
	touch "$SMOKE_DIR/remap.show"
	check "$how: shown again, neither" \
		"[ \"\$(get remap min)\" = 0 ] && [ \"\$(get remap snap)\" = 0 ] && \
		 [ \"\$(get remap visible)\" = 1 ] && [ \"\$(get remap tdrawn)\" = 5 ]"
	stop remap
done

wayland gleft
mapped gleft
check "+20+30 counts from the left and top" \
	"[ \"\$(get gleft x)\" = 20 ] && [ \"\$(get gleft y)\" = 30 ]"
stop gleft

wayland gbig size=2000x100
mapped gbig
check "too wide for the screen: reachable, and still at the bottom" \
	"[ \"\$(get gbig x)\" = 0 ] && [ \"\$(bottom gbig)\" = 720 ]"
stop gbig

do_input move 400 300
settle
wayland gpointer
mapped gpointer
cx=$(global cursor x) cy=$(global cursor y)
check "pointer puts the middle of the title bar under the pointer" \
	"[ \$cx -ge 399 ] && [ \$cx -le 401 ] && \
	 [ \$((\$(get gpointer x) + \$(outer_w gpointer) / 2 - cx)) -ge -\$(get gpointer th) ] && \
	 [ \$((\$(get gpointer x) + \$(outer_w gpointer) / 2 - cx)) -le \$(get gpointer th) ] && \
	 [ \$((\$(get gpointer y) + \$(get gpointer bw) + \$(get gpointer th) / 2)) = \$cy ]"
stop gpointer

do_input move 1270 710
settle
wayland gpedge
mapped gpedge
check "at the bottom right it stays on the screen" \
	"[ \"\$(right gpedge)\" = 1280 ] && [ \"\$(bottom gpedge)\" = 720 ]"
stop gpedge

# Growing by itself, a window keeps the edges its entry counts from.
wayland gwgrow grow=400x300 states=1
mapped gwgrow
wait_for "gwgrow has grown" grep -q grown "$SMOKE_DIR/gwgrow.events"
check "a window placed at -0-0 keeps the corner when it grows" \
	"[ \"\$(outer_w gwgrow)\" -gt 400 ] && [ \"\$(right gwgrow)\" = 1280 ] && [ \"\$(bottom gwgrow)\" = 720 ]"
grown_w=$(outer_w gwgrow)
wayland gfocus
mapped gfocus
check "another window takes the focus" "[ \"\$(get gwgrow active)\" = 0 ]"
stop gfocus
check "gwgrow has the focus again" "[ \"\$(get gwgrow active)\" = 1 ]"
settle
check "and keeps the size it chose through the configures that come with that" \
	"[ \"\$(outer_w gwgrow)\" = $grown_w ] && [ \"\$(right gwgrow)\" = 1280 ]"
do_input key alt+F10
check "gwgrow is maximized" "[ \"\$(get gwgrow max)\" = 1 ]"
do_input key alt+F10
check "restored, mwc's own size change moves nothing: still in the corner" \
	"[ \"\$(get gwgrow max)\" = 0 ] && [ \"\$(right gwgrow)\" = 1280 ] && [ \"\$(bottom gwgrow)\" = 720 ]"
stop gwgrow

# Once mwc has asked for a size, its later configures carry one: the size it chose
# itself must be what they say, or an obedient client shrinks back.
wayland gwsized grow=400x300 grow_ms=3000 states=1
mapped gwsized
do_input key alt+F10
check "gwsized is maximized" "[ \"\$(get gwsized max)\" = 1 ]"
do_input key alt+F10
check "and restored" "[ \"\$(get gwsized max)\" = 0 ]"
wait_for "gwsized has grown" grep -q grown "$SMOKE_DIR/gwsized.events"
settle
check "it keeps the size it chose" "[ \"\$(outer_w gwsized)\" -gt 400 ] && [ \"\$(right gwsized)\" = 1280 ]"
grown_w=$(outer_w gwsized)
wayland gfocus
mapped gfocus
stop gfocus
check "gwsized has the focus again" "[ \"\$(get gwsized active)\" = 1 ]"
settle
check "it keeps the size it chose after one mwc asked for" \
	"[ \"\$(outer_w gwsized)\" = $grown_w ] && [ \"\$(right gwsized)\" = 1280 ]"
stop gwsized

wayland gwleft grow=400x300
mapped gwleft
wait_for "gwleft has grown" grep -q grown "$SMOKE_DIR/gwleft.events"
check "one placed at +20+30 keeps its top-left corner" \
	"[ \"\$(outer_w gwleft)\" -gt 400 ] && [ \"\$(get gwleft x)\" = 20 ] && [ \"\$(get gwleft y)\" = 30 ]"
stop gwleft

# A frame asked for only once the window is shown, as GTK3 does.
wayland gwframe late_frame=kde
mapped gwframe
check "it appears without one" "[ \"\$(title gwframe)\" = 0 ]"
check "gwframe has its frame" "[ \"\$(title gwframe)\" -gt 0 ]"
settle
check "a frame that comes late keeps a window placed at -0-0 in the corner" \
	"[ \"\$(right gwframe)\" = 1280 ] && [ \"\$(bottom gwframe)\" = 720 ]"
stop gwframe

wayland gwfleft late_frame=kde
mapped gwfleft
check "gwfleft has its frame" "[ \"\$(title gwfleft)\" -gt 0 ]"
settle
check "and one at +20+30 at its top-left corner" \
	"[ \"\$(get gwfleft x)\" = 20 ] && [ \"\$(get gwfleft y)\" = 30 ]"
stop gwfleft

# A reload with a larger font: the title bar grows, the corner stays.
wayland gwfont
mapped gwfont
check "it is in the corner" \
	"[ \"\$(right gwfont)\" = 1280 ] && [ \"\$(bottom gwfont)\" = 720 ]"
old_title=$(title gwfont)
echo 'Mwm*fontList:  Sans Bold 30' >>"$RC"
kill -HUP "$pid"
check "the title bar has grown" "[ \"\$(title gwfont)\" -gt $old_title ]"
check "a reload with a taller title bar keeps it in its corner" \
	"[ \"\$(right gwfont)\" = 1280 ] && [ \"\$(bottom gwfont)\" = 720 ]"
sed -i '/^Mwm\*fontList:/d' "$RC"
kill -HUP "$pid"
check "the font is back" "[ \"\$(title gwfont)\" = $old_title ]"
check "and it is still in the corner" "[ \"\$(bottom gwfont)\" = 720 ]"
stop gwfont

wayland gwmoved grow=400x300 grow_ms=3000
mapped gwmoved
drag $(($(get gwmoved x) + 100)) $(($(get gwmoved y) + 12)) 500 300
check "it can be moved away" "[ \"\$(right gwmoved)\" != 1280 ]"
settle
moved_x=$(get gwmoved x)
moved_y=$(get gwmoved y)
wait_for "gwmoved has grown" grep -q grown "$SMOKE_DIR/gwmoved.events"
check "moved by hand, it grows from where it is now" \
	"[ \"\$(outer_w gwmoved)\" -gt 400 ] && [ \"\$(get gwmoved x)\" = $moved_x ] && [ \"\$(get gwmoved y)\" = $moved_y ]"
stop gwmoved

wayland gmain dialog=gdialog
mapped gmain
mapped gdialog
check "a dialog goes with its window, whatever its entry says" \
	"[ \"\$(get gdialog x)\" != 0 ] && [ \"\$(get gdialog y)\" != 0 ]"
stop gmain

wayland gcorner
mapped gcorner
do_input key super+t
check "tiling takes it over" "[ \"\$(get gcorner tiled)\" = 1 ]"
do_input key super+t
check "and gives it back to its place" \
	"[ \"\$(get gcorner tiled)\" = 0 ] && [ \"\$(right gcorner)\" = 1280 ] && [ \"\$(bottom gcorner)\" = 720 ]"
stop gcorner

wayland gother
mapped gother
wayland gtab
mapped gtab
do_input key super+g
check "gtab joins a group" "[ \"\$(get gtab tab)\" != -1 ]"
do_input key super+shift+g
check "taken out again" "[ \"\$(get gtab tab)\" = -1 ] && [ \"\$(get gtab tab_hidden)\" = 0 ]"
check "it cascades, the entry is for appearing" "[ \"\$(right gtab)\" != 1280 ]"
stop gtab
stop gother

wayland gmaxed pre=max
mapped gmaxed
check "gmaxed starts maximized" "[ \"\$(get gmaxed max)\" = 1 ]"
settle
do_input key alt+F10
check "and goes back to the top left, as documented" \
	"[ \"\$(get gmaxed max)\" = 0 ] && [ \"\$(get gmaxed x)\" = 32 ] && [ \"\$(get gmaxed y)\" = 32 ]"
stop gmaxed

wayland glate late_app=gcorner
check "its app_id arrives" "[ -n \"\$(get gcorner x)\" ]"
check "too late for the entry, as documented" "[ \"\$(right gcorner)\" != 1280 ]"
kill "$PID_glate"
wait "$PID_glate" 2>/dev/null
check "glate is gone" "[ -z \"\$(get gcorner x)\" ]"

WAYLAND_DISPLAY=wayland-0 build/layerc layer=top anchor=bottom,left,right size=0x40 \
	exclusive=40 >"$SMOKE_DIR/gpanel.events" 2>&1 &
PID_gpanel=$!
check "a panel reserves the bottom" "[ \"\$(global output uh)\" = 680 ]"
wayland gcorner
mapped gcorner
check "the corner is the usable area's" \
	"[ \"\$(right gcorner)\" = 1280 ] && [ \"\$(bottom gcorner)\" = 680 ]"
stop gcorner
kill "$PID_gpanel"
wait "$PID_gpanel" 2>/dev/null
check "the panel is gone" "[ \"\$(global output uh)\" = 720 ]"

wayland gsized
wayland ghalf
mapped gsized
mapped ghalf
# Loaded four times by now (twice for the font), and the window asking adds nothing.
check "a size in the value is reported once per load" \
	"[ \"\$(grep -c 'mwmrc:5: expected a position' \"$LOG\")\" = 4 ]"
check "half a position too" "grep -q 'mwmrc:6: expected a position' \"$LOG\""
check "and such a window cascades as usual" "[ \"\$(right gsized)\" != 1280 ]"
stop gsized
stop ghalf
rm "$RC"

# A dialog that is being moved goes away with its window: the move ends with it.
wayland dlgmain size=400x300 dialog=dlgkid minimize_after="$SMOKE_DIR/dlgmain.min"
check "a window with a dialog" "[ -n \"\$(get dlgkid x)\" ] && [ -n \"\$(get dlgmain x)\" ]"
kx=$(($(get dlgkid x) + $(get dlgkid bw) + 40)) ky=$(($(get dlgkid y) + $(get dlgkid bw) + $(get dlgkid th) + 30))
do_input hold alt
do_input move $kx $ky
do_input down left
do_input move $((kx + 60)) $((ky + 40))
check "the dialog is being moved" "[ \"\$(global cursor mode)\" = 1 ]"
touch "$SMOKE_DIR/dlgmain.min"
saw "the window minimizes itself" dlgmain '^minimize asked'
check "and the dialog goes with it" "[ \"\$(get dlgmain min)\" = 1 ] && [ \"\$(get dlgkid hidden)\" = 1 ]"
check "the move ends" "[ \"\$(global cursor mode)\" = 0 ] && [ \"\$(global outline shown)\" = 0 ]"
do_input move $((kx + 200)) $((ky + 100))
do_input up left
do_input release alt
kx2=$(get dlgkid x) ky2=$(get dlgkid y)
do_input move 900 600
# Once mwc has the motion, a dialog still grabbed would have followed it.
check "the pointer is at 900,600" "[ \"\$(global cursor x)\" = 900 ] && [ \"\$(global cursor y)\" = 600 ]"
[ "$(get dlgkid x)" = "$kx2" ] && [ "$(get dlgkid y)" = "$ky2" ] ||
	fail "the hidden dialog still follows the pointer"
echo "ok: the hidden dialog stays where it is"
stop dlgmain

# The corner grooves go on from the lines where the buttons meet, as in mwm: dark on
# B + T - 1, light on B + T, along the top and down the sides.
wayland grooves size=300x200
check "it has the focus" "[ \"\$(get grooves active)\" = 1 ]"
sleep 0.5
refresh
GX=$(get grooves x) GY=$(get grooves y) GB=$(get grooves bw) GT=$(get grooves th)
GW=$(($(get grooves w) + 2 * GB)) GH=$(($(get grooves h) + 2 * GB + GT))
K=$((GB + GT))
WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/grooves.png" || fail "a screenshot"
px() { build/pngtool pixel "$SMOKE_DIR/grooves.png" "$1" "$2"; }
BORDER_Y=$((GY + GB / 2)) TITLE_Y=$((GY + GB + GT / 2))
line_at() { # description x: a groove in the border and a button edge in the title there
	local x=$2
	[ "$(px $((x - 1)) $BORDER_Y)" != "$(px $x $BORDER_Y)" ] ||
		fail "$1: no groove in the border at $x"
	[ "$(px $((x - 1)) $BORDER_Y)" = "$(px $((x - 1)) $TITLE_Y)" ] &&
		[ "$(px $x $BORDER_Y)" = "$(px $x $TITLE_Y)" ] ||
		fail "$1: the groove at $x isn't on the button edge: border $(px $((x - 1)) $BORDER_Y) $(px $x $BORDER_Y), title $(px $((x - 1)) $TITLE_Y) $(px $x $TITLE_Y)"
	echo "ok: $1"
}
line_at "top left: the groove goes on from the menu button's edge" $((GX + K))
line_at "top right: and from the maximize button's" $((GX + GW - K))
SIDE_X=$((GX + GW - GB / 2)) CLIENT_X=$((GX + GW - GB - 10))
[ "$(px $SIDE_X $((GY + K - 1)))" = "$(px $CLIENT_X $((GY + K - 1)))" ] &&
	[ "$(px $SIDE_X $((GY + K - 1)))" != "$(px $SIDE_X $((GY + K)))" ] ||
	fail "the side groove isn't on the title bar's lower edge"
echo "ok: down the side it goes on from the title bar's lower edge"
stop grooves

# A narrow window: the corners take at most a third of the frame, as in mwm, so a
# piece of the side is left between them.
wayland narrow size=40x150
check "narrow is mapped" "[ -n \"\$(get narrow x)\" ]"
sleep 0.5
refresh
NX=$(get narrow x) NY=$(get narrow y) NB=$(get narrow bw)
NW=$(($(get narrow w) + 2 * NB))
NK=$((NW / 3))
WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/narrow.png" || fail "a screenshot"
npx() { build/pngtool pixel "$SMOKE_DIR/narrow.png" "$1" "$2"; }
NY0=$((NY + NB / 2))
[ "$(npx $((NX + NK - 1)) $NY0)" != "$(npx $((NX + NK)) $NY0)" ] &&
	[ "$(npx $((NX + NW - NK - 1)) $NY0)" != "$(npx $((NX + NW - NK)) $NY0)" ] ||
	fail "the corners of a frame $NW wide don't end at a third, $NK"
[ "$(npx $((NX + NW / 2)) $NY0)" = "$(npx $((NX + NW / 2 + 1)) $NY0)" ] ||
	fail "no side left between the corners"
echo "ok: a narrow window's corners take a third each, with side between them"
stop narrow

# Behind the client lies the frame's colour: it shows through what the client leaves
# transparent and takes the clicks outside its input region.
wayland clear size=300x200 clear=1
check "clear has the focus" "[ \"\$(get clear active)\" = 1 ]"
wayland other size=100x80
check "other takes it" "[ \"\$(get other active)\" = 1 ] && [ \"\$(get clear active)\" = 0 ]"
drag $(($(get other x) + 50)) $(($(get other y) + $(get other bw) + 4)) 1000 500
sleep 0.5
refresh
QX=$(get clear x) QY=$(get clear y) QB=$(get clear bw) QT=$(get clear th)
MX=$((QX + QB + $(get clear w) / 2)) MY=$((QY + QB + QT + $(get clear h) / 2))
WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/clear.png" || fail "a screenshot"
qpx() { build/pngtool pixel "$SMOKE_DIR/clear.png" "$1" "$2"; }
[ "$(qpx $MX $MY)" = "$(qpx $((QX + QB / 2)) $MY)" ] ||
	fail "a transparent client shows $(qpx $MX $MY), not its frame's $(qpx $((QX + QB / 2)) $MY)"
echo "ok: a transparent client shows its frame's colour"
do_input move $MX $MY
do_input click left
check "a click where it takes none brings it to the front" \
	"[ \"\$(get clear active)\" = 1 ] && [ \"\$(global menu open)\" = 0 ]"
stop other
stop clear

# ---- fullscreen that doesn't cover the output ---------------------------------------------

# A program keeping its own size, as a game with a fixed resolution: in the middle of
# the output, with black around it, not the desktop (xdg-shell).
wayland fixedres size=400x300 own_size=1 pre=full
mapped fixedres
check "it is fullscreen" "[ \"\$(get fixedres full)\" = 1 ] && [ \"\$(get fixedres w)\" = 400 ]"
pixel_is "black around it" 5 5 000000
pixel_is "black beside it" 300 360 000000
pixel_is "the window itself in the middle" 640 360 808080
stop fixedres
# Its popups are placed by where it is drawn: one at its right edge slides back onto the
# output. 400 wide in the middle of 1280 is drawn from 440; a popup 600 wide at 350
# would end at 1390, so it slides 110 to the left (Review 2026-11-18 #23).
wayland fixedpop size=400x300 own_size=1 pre=full popup=350,100,600,100 \
	popup_after="$SMOKE_DIR/popup"
mapped fixedpop
check "it is fullscreen" "[ \"\$(get fixedpop full)\" = 1 ] && [ \"\$(get fixedpop w)\" = 400 ]"
touch "$SMOKE_DIR/popup"
saw "its popup slides back onto the output" fixedpop '^popup at 240 100$'
stop fixedpop

# ---- a burst of titles ---------------------------------------------------------------------

# A program that sets its title as fast as it can (a progress counter, a terminal
# forwarding title escapes): the title bar is drawn at most once a refresh, with the last
# one, not once per title (Review 2026-11-18 #18).
wayland titled size=600x200 titles=2000
mapped titled
saw "it sends 2000 titles" titled '^titles sent'
check "the last one counts" "[ \"\$(get titled tlen)\" = 4 ]"
check "and is drawn" "[ \"\$(get titled tdrawn)\" = 4 ]"
n=$(get titled tdraws)
[ "$n" -le 60 ] || fail "2000 titles drew the title bar $n times"
echo "ok: 2000 titles drew the title bar $n times"
stop titled

# ---- the window menu a client asks for -------------------------------------------------

# show_window_menu says where in surface coordinates, which include a shadow drawn
# around the window geometry (GTK): the menu opens there, not 20 px off.
wayland shadowed size=300x200 shadow=20 menu_at=50,40
mapped shadowed
refresh
SX=$(get shadowed x) SY=$(get shadowed y)
# The window geometry fills the frame (wlroots' scene tree offsets the surface; mwc did
# it once more), and the shadow around it is cut off, not drawn over the frame.
IX=$((SX + $(border shadowed))) IY=$((SY + $(border shadowed) + $(title shadowed)))
pixel_is "the window geometry starts in the frame's corner" $IX $IY 808080
pixel_is "and ends in the opposite one" $((IX + 299)) $((IY + 199)) 808080
pixel_isnt "the frame right of it is no shadow" $((IX + 303)) $((IY + 100)) 402020
pixel_isnt "nor is the title bar above it" $((IX + 150)) $((IY - 5)) 402020
do_input move $((SX + 150)) $((SY + $(border shadowed) + $(title shadowed) + 100))
do_input click left
saw "it asks for its window menu" shadowed '^menu asked'
check "the menu is open" "[ \"\$(global menu open)\" = 1 ]"
# Unless that is too low for it: the window's cascade slot depends on the tests above.
my=$((IY + 20)) bottom=$(($(global output uy) + $(global output uh) - $(global menu h)))
[ "$my" -le "$bottom" ] || my=$bottom
check "the menu opens at that point of its surface" \
	"[ \"\$(global menu x)\" = \$((IX + 30)) ] && [ \"\$(global menu y)\" = $my ]"
do_input key Escape
stop shadowed

# A subsurface past the window geometry of a window without a shadow, as GTK3's
# tooltips: drawn whole, past the frame too.
wayland subbed size=300x200 geometry_at=0,0 sub=280,180,60,60
mapped subbed
saw "it adds a subsurface" subbed '^sub added'
refresh
SBX=$(($(get subbed x) + $(border subbed))) SBY=$(($(get subbed y) + $(border subbed) + $(title subbed)))
pixel_is "the subsurface is drawn past the frame" $((SBX + 330)) $((SBY + 230)) 2040a0
stop subbed

# ---- what a window is offered ----------------------------------------------------------------

# wm_capabilities as mwc means them: a dialog gets no maximize or minimize to show.
wayland capable size=300x200 caps=1 dialog=capdlg
mapped capable
wait_for "the main window is offered both" grep -q 'caps capable max=1 min=1' "$SMOKE_DIR/capable.events"
wait_for "its dialog neither" grep -q 'caps capdlg max=0 min=0' "$SMOKE_DIR/capable.events"
stop capable

stop_vinput
stop_mwc
echo "PASS: window"
