#!/bin/bash
# Headless test of combinations: each starting point (floating, tabs, tiling, shading,
# tiling with tabs; each also on all desktops) takes each operation once, and after
# every step tests/invariants.awk checks that what mwc says of its windows fits
# together; each case runs twice and leaves the scene as it found it. One mwc, a reset
# between the cases. Output goes to build/matrix.log, the operations to
# build/matrix-ops.log.
LOG=build/matrix.log
OPS_LOG=build/matrix-ops.log
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/ops.sh" || { echo "FAIL: tests/ops.sh"; exit 1; }

for tool in build/vinput build/xdgc build/workspacec; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

# The checker first: each rule reports a state that breaks it, and sound ones pass.
# view <app> [field=value ...]: a window line, the fields a sound floating window has
# unless given; its id is its letter's code, root its own id unless given.
id() { printf '%d' "'$1"; }
view() {
	local app=$1 line="active=0 kbfocus=0 min=0 full=0 hidden=0 tiled=0 tile_main=0 \
shaded=0 tab=-1 tab_hidden=0 visible=1 desktop=1 everywhere=0 group=0 sticky=0 icon_shown=0 \
icon_x=0 icon_y=0 cell=0,0,0,0 id=$(id "$1") root=$(id "$1")"
	shift
	local f
	for f in "$@"; do
		line=$(sed "s/\\b${f%%=*}=[^ ]*/$f/" <<<"$line")
	done
	echo "view app=$app $line"
}
tail_lines() { printf '%s\n' "output ux=0 uy=0 uw=1280 uh=720 on=1" \
	"iconbox on=${ICONBOX:-0} focused=${ICON:-0}" \
	"tiling on=${TILING:-0} area=0,0,1280,720 usable=0,0,1280,720 gap=8" \
	"desktop current=1" \
	"focus any=1 layer=${LAYER:-0}" "seat keyboard=${KB:-a}"; }
# sound <view lines...>, breaks <expected message> <view lines...>
sound() {
	local out
	out=$( { printf '%s\n' "$@"; tail_lines; } | awk -f tests/invariants.awk) ||
		fail "the checker rejects a sound state: $out"
}
breaks() {
	local want=$1 out
	shift
	if out=$( { printf '%s\n' "$@"; tail_lines; } | awk -f tests/invariants.awk); then
		fail "the checker passed a state where $want"
	fi
	grep -q "$want" <<<"$out" || fail "the checker missed '$want': $out"
}
sound "$(view a active=1 kbfocus=1)" "$(view b)"
LAYER=1 KB=other sound "$(view a active=1)"
ICON=1 KB=none sound "$(view a)"
sound "$(view a min=1 visible=0 icon_shown=1)" "$(view b group=3 tab=0)" \
	"$(view c group=3 tab=1 tab_hidden=1 visible=0)" "$(view d root=$(id b) hidden=1 visible=0)"
TILING=1 sound "$(view a tiled=1 tile_main=1 cell=644,8,628,704)" \
	"$(view b tiled=1 cell=8,8,628,348)" "$(view c tiled=1 cell=8,364,628,348)"
TILING=1 sound "$(view a tiled=1 tile_main=1 cell=8,8,1264,704)"
breaks "2 windows are active" "$(view a active=1 kbfocus=1)" "$(view b active=1)"
breaks "b has the keyboard but isn't active" "$(view a active=1)" "$(view b kbfocus=1)"
breaks "a is active but the keyboard is with" "$(view a active=1)"
LAYER=1 KB=other breaks "a layer surface has the keys and a" "$(view a active=1 kbfocus=1)"
LAYER=1 breaks "the seat's keyboard is with a" "$(view a active=1)"
KB=other breaks "no layer has the keys" "$(view a active=1 kbfocus=1)"
ICON=1 KB=none breaks "the icon box has the focus and a is active" "$(view a active=1)"
breaks "a visible=1, but min=1" "$(view a min=1 icon_shown=1)"
breaks "a visible=0, but min=0" "$(view a visible=0)"
breaks "a visible=1, but .* desktop=2/1" "$(view a desktop=2)"
breaks "a is active but out of sight" "$(view a active=1 kbfocus=1 min=1 visible=0 icon_shown=1)"
breaks "a icon_shown=0, but min=1" "$(view a min=1 visible=0)"
breaks "a icon_shown=1, but min=0" "$(view a icon_shown=1)"
breaks "b's and a's icons are both at 8,640" "$(view a min=1 visible=0 icon_shown=1 icon_x=8 \
	icon_y=640)" "$(view b min=1 visible=0 icon_shown=1 icon_x=8 icon_y=640)"
breaks "a is fullscreen and tiled" "$(view a full=1 tiled=1 cell=0,0,640,720)"
breaks "a is shaded without a cell" "$(view a shaded=1 tiled=1)"
TILING=1 breaks "a is shaded and the main window" "$(view a shaded=1 tiled=1 tile_main=1 \
	cell=8,8,1264,704)"
breaks "a is tiled here but out of sight" "$(view a tiled=1 min=1 visible=0 icon_shown=1 \
	cell=8,8,1264,704)"
breaks "a is hidden with its window but has none" "$(view a hidden=1 visible=0)"
breaks "d is in sight, its window s an icon" "$(view s min=1 visible=0 icon_shown=1)" \
	"$(view d root=$(id s))"
breaks "d is on desktop 2, its window s" "$(view s)" "$(view d root=$(id s) desktop=2 visible=0)"
breaks "group 3 shows 2 tabs" "$(view a group=3 tab=0)" "$(view b group=3 tab=1)"
breaks "group 3 has 1 window" "$(view a group=3 tab=0)"
breaks "group 3 of 2 has 2 tab 0" "$(view a group=3 tab=0)" \
	"$(view b group=3 tab=0 tab_hidden=1 visible=0)"
breaks "a is in no group but tab=1" "$(view a tab=1)"
breaks "group 3: b is on desktop" "$(view a group=3 tab=0)" \
	"$(view b group=3 tab=1 tab_hidden=1 visible=0 everywhere=1)"
breaks "group 3: b .* sticky=1" "$(view a group=3 tab=0)" \
	"$(view b group=3 tab=1 tab_hidden=1 visible=0 sticky=1)"
TILING=1 breaks "a's and b's cells overlap" "$(view a tiled=1 tile_main=1 cell=600,8,672,704)" \
	"$(view b tiled=1 cell=8,8,700,704)"
TILING=1 breaks "a's cell .* is outside" "$(view a tiled=1 tile_main=1 cell=0,0,1300,720)"
TILING=1 breaks "the cells cover 8,8 to 608,712" "$(view a tiled=1 tile_main=1 cell=8,8,600,704)"
TILING=1 breaks "is tiled in an empty cell" "$(view a tiled=1 tile_main=1 cell=8,8,0,0)"
TILING=1 breaks "0 main windows in a layout of 1" "$(view a tiled=1 cell=8,8,1264,704)"
TILING=1 breaks "the main window, isn't on the right" "$(view a tiled=1 tile_main=1 \
	cell=8,8,628,704)" "$(view b tiled=1 cell=644,8,628,704)"
TILING=1 breaks "c doesn't follow the one above it" \
	"$(view a tiled=1 tile_main=1 cell=644,8,628,704)" "$(view b tiled=1 cell=8,8,628,348)" \
	"$(view c tiled=1 cell=8,400,628,312)"
TILING=1 breaks "the stack ends at 600" "$(view a tiled=1 tile_main=1 cell=644,8,628,704)" \
	"$(view b tiled=1 cell=8,8,628,592)"
TILING=1 breaks "b is out of the stack's column" \
	"$(view a tiled=1 tile_main=1 cell=644,8,628,704)" "$(view b tiled=1 cell=8,8,600,704)"
echo "ok: the checker reports each kind of broken state"

export MWC_TEST_STATE=$SMOKE_DIR/state
mkdir -p "$XDG_CONFIG_HOME/mwc"
ops_rc >"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'exec sleep 300'
start_vinput
refresh

# The starting points. Each ends with the focus on the window an operation acts on.
: >"$OPS_LOG"
in_group() { check "$1 is in a group" "[ \"\$(get $1 group)\" -gt 0 ] 2>/dev/null"; }
tiled() { check "$1 is tiled" "[ \"\$(get $1 tiled)\" = 1 ]"; }
setup_floating() { op_spawn; }
setup_tabs() { op_spawn; op_spawn; op_group; in_group c2; }
setup_tiled() { op_tiling; op_spawn; op_spawn; op_spawn; tiled c1; tiled c2; tiled c3; }
setup_shaded() {
	setup_tiled
	op_focus # a window in the stack: minimizing it there rolls it up
	op_minimize
	check "one of them is rolled up" "grep -q ' shaded=1 ' \"\$MWC_TEST_STATE\""
	op_focus
}
setup_tiled_tabs() { setup_tiled; op_group; in_group c3; tiled c3; }

# The operations, each once per starting point.
OPS=(minimize "minimize restore" maximize fullscreen group ungroup tab "desktop 2"
	close reload title app size dialog cfull cmin)

# Each case twice: what it added to the scene must have gone with its windows. The
# first run creates what mwc makes once and keeps (a menu, the outline), so the
# second must end with the same nodes.
cases=0
for setup in floating tabs tiled shaded tiled_tabs; do
	for sticky in 0 1; do
		for op in "${OPS[@]}"; do
			label="$setup$([ $sticky = 1 ] && echo /sticky): $op"
			R=$cases # title and size: the same every run
			nodes=()
			for run in 1 2; do
				"setup_$setup"
				inv "$label: set up"
				if [ $sticky = 1 ]; then
					op_sticky
					check "a window is sticky" \
						"grep -q ' sticky=1 ' \"\$MWC_TEST_STATE\""
					inv "$label: sticky"
				fi
				for step in $op; do
					if [ "$step" = 2 ]; then continue; fi
					if [ "$step" = desktop ]; then op_desktop 2; else "op_$step"; fi
					inv "$label"
				done
				ops_reset
				inv "$label: reset"
				nodes+=("$(grep '^scene ' "$MWC_TEST_STATE")")
			done
			[ "${nodes[0]}" = "${nodes[1]}" ] ||
				fail "$label: nodes left in the scene: ${nodes[0]}, again ${nodes[1]}"
			cases=$((cases + 1))
		done
	done
done
echo "ok: $cases cases twice, the invariants held after every step, the scene kept no node"

stop_vinput
stop_mwc
echo "PASS: matrix"
