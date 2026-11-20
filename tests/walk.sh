#!/bin/bash
# Headless random walk: the operations of tests/ops.sh in a random order, for sequences
# the matrix doesn't build, with tests/invariants.awk after every step. WALK_STEPS
# steps (default 200), from WALK_SEED (default the time); the seed is in every report
# and repeats the walk. Output goes to build/walk.log, or WALK_LOG for walks side by side.
LOG=${WALK_LOG:-build/walk.log}
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/ops.sh" || { echo "FAIL: tests/ops.sh"; exit 1; }

for tool in build/vinput build/xdgc build/workspacec; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

STEPS=${WALK_STEPS:-200}
SEED=${WALK_SEED:-$(date +%s)}
RANDOM=$SEED
echo "walk: $STEPS steps, WALK_SEED=$SEED" | tee -a "$LOG"

export MWC_TEST_STATE=$SMOKE_DIR/state
mkdir -p "$XDG_CONFIG_HOME/mwc"
ops_rc >"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'exec sleep 300'
start_vinput
refresh

# Each as likely as its count here.
OPS=(spawn spawn minimize restore restore maximize fullscreen group group ungroup tab
	focus focus tiling close reload title app app size dialog undialog cfull cmin
	sticky desktop desktop)

for ((i = 1; i <= STEPS; i++)); do
	if [ ${#CLIENTS[@]} = 0 ]; then
		op=spawn
	else
		op=${OPS[RANDOM % ${#OPS[@]}]}
		[ $op = spawn ] && [ ${#CLIENTS[@]} -ge 5 ] && op=focus
	fi
	if [ $op = desktop ]; then
		op_desktop $((1 + RANDOM % 3))
	else
		"op_$op"
	fi
	inv "WALK_SEED=$SEED step $i ($op)"
done
echo "ok: $STEPS steps from WALK_SEED=$SEED, the invariants held after every one"

stop_vinput
stop_mwc
echo "PASS: walk"
