#!/bin/bash
# Headless test of combinations: each starting point (floating, tabs, tiling, shading,
# tiling with tabs; each also on all desktops) takes each operation once, and after
# every step tests/invariants.awk checks that what mwc says of its windows fits
# together. One mwc, a reset between the cases. Output goes to build/matrix.log.
LOG=build/matrix.log
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/ops.sh" || { echo "FAIL: tests/ops.sh"; exit 1; }

for tool in build/vinput build/xdgc build/workspacec; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

# The checker first: each rule reports a state that breaks it, and a sound one passes.
# view <app> [field=value ...]: a window line, the fields a sound floating window has
# unless given.
view() {
	local app=$1 line="active=0 kbfocus=0 min=0 full=0 hidden=0 tiled=0 shaded=0 tab=-1 \
tab_hidden=0 visible=1 desktop=1 everywhere=0 group=0 sticky=0 cell=0,0,0,0"
	shift
	local f
	for f in "$@"; do
		line=$(sed "s/\b${f%%=*}=[^ ]*/$f/" <<<"$line")
	done
	echo "view app=$app $line root=${ROOT:-$app}"
}
tail_lines() { printf '%s\n' "output ux=0 uy=0 uw=1280 uh=720 on=1" \
	"iconbox focused=${ICON:-0}" "tiling on=${TILING:-0} area=0,0,1280,720 gap=8" \
	"desktop current=1" \
	"focus any=1 layer=${LAYER:-0}" "seat keyboard=${KB:-a}"; }
# breaks <expected message> <view lines...>: the checker reports it.
breaks() {
	local want=$1 out
	shift
	if out=$( { printf '%s\n' "$@"; tail_lines; } | awk -f tests/invariants.awk); then
		fail "the checker passed a state where $want"
	fi
	grep -q "$want" <<<"$out" || fail "the checker missed '$want': $out"
}
{ view a active=1 kbfocus=1; view b; tail_lines; } | awk -f tests/invariants.awk >/dev/null ||
	fail "the checker rejects a sound state"
breaks "2 windows are active" "$(view a active=1 kbfocus=1)" "$(view b active=1)"
breaks "b has the keyboard but isn't active" "$(view a active=1)" "$(view b kbfocus=1)"
breaks "a is active but the keyboard is with" "$(view a active=1)"
ICON=1 breaks "the icon box has the focus" "$(view a kbfocus=1 active=1)"
breaks "a visible=1, but min=1" "$(view a min=1)"
breaks "a visible=0, but min=0" "$(view a visible=0)"
breaks "a visible=1, but .* desktop=2/1" "$(view a desktop=2)"
breaks "a is active but out of sight" "$(view a active=1 kbfocus=1 min=1 visible=0)"
breaks "a is fullscreen and tiled" "$(view a full=1 tiled=1 cell=0,0,640,720)"
breaks "a is shaded without a cell" "$(view a shaded=1 tiled=1)"
breaks "group 3 shows 2 tabs" "$(view a group=3)" "$(view b group=3)"
breaks "group 3 shows 0 tabs" "$(view a group=3 tab_hidden=1 visible=0)"
breaks "group 3: b is on desktop" "$(view a group=3)" \
	"$(view b group=3 tab_hidden=1 visible=0 everywhere=1)"
breaks "d belongs to sticky s" "$(view s sticky=1 everywhere=1)" "$(ROOT=s view d)"
TILING=1 breaks "a's and b's cells overlap" "$(view a tiled=1 cell=0,0,700,720)" \
	"$(view b tiled=1 cell=600,0,680,720)"
TILING=1 breaks "a's cell .* is outside" "$(view a tiled=1 cell=0,0,1300,720)"
TILING=1 breaks "the cells cover 8,8 to 608,712" "$(view a tiled=1 cell=8,8,600,704)"
{ view a tiled=1 cell=8,8,1264,704; TILING=1 tail_lines; } | awk -f tests/invariants.awk \
	>/dev/null || fail "the checker rejects a window filling the layout"
echo "ok: the checker reports each kind of broken state"

export MWC_TEST_STATE=$SMOKE_DIR/state
mkdir -p "$XDG_CONFIG_HOME/mwc"
ops_rc >"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'exec sleep 300'
start_vinput
refresh

# The starting points. Each ends with the focus on the window an operation acts on.
setup_floating() { op_spawn; }
setup_tabs() { op_spawn; op_spawn; op_group; }
setup_tiled() { op_tiling; op_spawn; op_spawn; op_spawn; }
setup_shaded() {
	setup_tiled
	op_focus # a window in the stack: minimizing it there rolls it up
	op_minimize
	check "one of them is rolled up" "grep -q ' shaded=1 ' \"\$MWC_TEST_STATE\""
	op_focus
}
setup_tiled_tabs() { op_tiling; op_spawn; op_spawn; op_spawn; op_group; }

# The operations, each once per starting point.
OPS=(minimize "minimize restore" maximize fullscreen group ungroup tab "desktop 2"
	close reload title app size dialog cfull cmin)

cases=0
for setup in floating tabs tiled shaded tiled_tabs; do
	for sticky in 0 1; do
		for op in "${OPS[@]}"; do
			label="$setup${sticky:+/sticky=$sticky}: $op"
			"setup_$setup"
			inv "$label: set up"
			if [ $sticky = 1 ]; then
				op_sticky
				inv "$label: sticky"
			fi
			for step in $op; do
				if [ "$step" = 2 ]; then continue; fi
				if [ "$step" = desktop ]; then op_desktop 2; else "op_$step"; fi
				inv "$label"
			done
			ops_reset
			inv "$label: reset"
			cases=$((cases + 1))
		done
	done
done
echo "ok: $cases cases, the invariants held after every step"

stop_vinput
stop_mwc
echo "PASS: matrix"
