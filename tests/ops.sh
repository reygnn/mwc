# What a user and clients do to windows, for matrix.sh and walk.sh: each operation
# acts on the window with the focus, or on a client, and returns once mwc has handled
# it. After each, inv checks tests/invariants.awk against mwc's state.
#
# Clients are xdgc windows c1, c2, ..., each driven through control=FIFO. Their app_id
# switches between cN and cN-all, which mwmrc puts on all desktops; a dialog is cN-d.
# The operations go to OPS_LOG, as mwc writes its own log over what is appended there.

declare -a CLIENTS=()
declare -A CTL=() DID=() APP=() PIDS=()
SEQ=0
SEQ_MAX=500 # cN-all rules in mwmrc
TRACE=() # the operations of the current case or walk, for a report
R=0 # a number for title and size, which the caller sets: a walk stays the same
OPS_LOG=${OPS_LOG:-/dev/null}

ops_rc() {
	local i
	echo 'Mwm*outputManagement: true'
	for ((i = 1; i <= SEQ_MAX; i++)); do
		echo "Mwm*c$i-all*allDesktops: true"
	done
}

# inv <label>: the invariants hold now.
inv() {
	local out
	refresh
	if ! out=$(awk -f tests/invariants.awk "$MWC_TEST_STATE"); then
		echo "--- after: ${TRACE[*]}"
		echo "$out"
		echo "--- state"
		cat "$MWC_TEST_STATE"
		fail "$1: invariants broken"
	fi
}

trace() { TRACE+=("$*"); echo "op: $*" >>"$OPS_LOG"; }

# The client a window belongs to: cN for cN, cN-all and cN-d.
owner() {
	local a=${1%-d}
	echo "${a%-all}"
}

focused_app() { awk '$1 == "view" && / active=1 / { sub("app=", "", $2); print $2; exit }' \
	"$MWC_TEST_STATE"; }

# Running, not a zombie bash has yet to reap. The state follows the name in brackets,
# which may hold spaces.
alive() {
	local p=${PIDS[$1]-} s
	[ -n "$p" ] && s=$(<"/proc/$p/stat") 2>/dev/null || return 1
	s=${s##*) }
	[ "${s%% *}" != Z ]
}

# Clients that ended, as when their last window was closed: forgotten.
prune() {
	local c keep=()
	for c in "${CLIENTS[@]}"; do
		if alive "$c"; then
			keep+=("$c")
		else
			wait "${PIDS[$c]}" 2>/dev/null
			eval "exec ${CTL[$c]}>&-"
			rm -f "$SMOKE_DIR/$c.ctl"
			unset "CTL[$c]" "DID[$c]" "APP[$c]" "PIDS[$c]"
		fi
	done
	CLIENTS=("${keep[@]}")
}

op_spawn() {
	local c=c$((++SEQ))
	[ $SEQ -le $SEQ_MAX ] || fail "more than $SEQ_MAX clients: no cN-all rule for $c"
	trace spawn "$c"
	mkfifo "$SMOKE_DIR/$c.ctl"
	WAYLAND_DISPLAY=wayland-0 build/xdgc app="$c" size=300x200 control="$SMOKE_DIR/$c.ctl" \
		>"$SMOKE_DIR/$c.log" 2>&1 &
	PIDS[$c]=$!
	exec {fd}<>"$SMOKE_DIR/$c.ctl"
	CTL[$c]=$fd
	DID[$c]=0
	APP[$c]=$c
	CLIENTS+=("$c")
	check "$c is mapped" "[ -n \"\$(get $c x)\" ]"
}

# cmd <client> <command>: sent, and handled by mwc.
cmd() {
	local c=$1
	shift
	alive "$c" || return 0
	echo "$*" >&"${CTL[$c]}"
	DID[$c]=$((DID[$c] + 1))
	wait_for "$c: $*" eval "[ \$(grep -c '^did' \"$SMOKE_DIR/$c.log\") -ge ${DID[$c]} ] || ! alive $c"
}

# TARGET: the client of the window with the focus, or the first one; empty for none.
# A variable, not an output: a failure inside $(...) would end only the subshell.
target() {
	local a
	refresh
	a=$(focused_app)
	TARGET=
	if [ -n "$a" ]; then
		TARGET=$(owner "$a")
	elif [ ${#CLIENTS[@]} -gt 0 ]; then
		TARGET=${CLIENTS[0]}
	fi
	[ -n "$TARGET" ] && alive "$TARGET"
}

# Whether the client's main window is there.
has_main() { [ -n "$(get "${APP[$1]}" x)" ]; }

op_key() { trace key "$1"; do_input key "$1"; }
op_minimize() { op_key alt+F9; }
op_maximize() { op_key alt+F10; }
op_fullscreen() { op_key alt+F11; }
op_group() { op_key super+g; }
op_ungroup() { op_key super+shift+g; }
op_tab() { op_key super+Tab; }
op_focus() { op_key alt+Tab; }
op_tiling() { op_key super+t; }

op_close() {
	local a
	refresh
	a=$(focused_app)
	op_key alt+F4
	[ -n "$a" ] && check "$a is closed" "[ -z \"\$(get $a x)\" ]"
	prune
}

# Whether a window in sight covers the point x y, as a fullscreen one does the icons.
covered() {
	awk -v px="$1" -v py="$2" '$1 == "view" && / visible=1 / {
		for (i = 2; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] }
		if (px >= f["x"] && px < f["x"] + f["w"] + 2 * f["bw"] &&
				py >= f["y"] && py < f["y"] + f["fh"]) { found = 1; exit }
	} END { exit !found }' "$MWC_TEST_STATE"
}

# An icon brought back, one no window covers; or the window with the focus restored,
# as in the layout, where Alt+F9 rolls up or moves into the stack.
op_restore() {
	local c a x y
	refresh
	for c in "${CLIENTS[@]}"; do
		for a in "${APP[$c]}" "$c-d"; do
			[ "$(get "$a" min)" = 1 ] && [ "$(get "$a" icon_shown)" = 1 ] || continue
			x=$(($(get "$a" icon_x) + 20)) y=$(($(get "$a" icon_y) + 20))
			if ! covered $x $y; then
				trace restore "$a"
				do_input move $x $y
				do_input dclick left
				do_input move 5 5
				check "$a is back" "[ \"\$(get $a min)\" = 0 ]"
				return
			fi
		done
	done
	op_key alt+F5
}

op_desktop() { trace desktop "$1"; WAYLAND_DISPLAY=wayland-0 build/workspacec activate "$1" \
	>/dev/null || fail "desktop $1"; }

op_reload() { trace reload; reload_mwc; }

# On All Desktops or Only This Desktop from the window menu, where it is offered.
op_sticky() {
	local a was
	trace sticky
	refresh
	a=$(focused_app)
	[ -n "$a" ] || return 0
	a=${APP[$(owner "$a")]-} # the flag is its window's, also for a dialog
	[ -n "$a" ] && [ -n "$(get "$a" x)" ] || return 0
	was=$(get "$a" sticky)
	do_input key alt+space
	refresh
	if [ "$(global menu open)" = 1 ] && [[ $(global menu sensitive) == *O* ]]; then
		do_input key o
		check "$a sticky=$((1 - was))" "[ \"\$(get $a sticky)\" = $((1 - was)) ]"
	else
		do_input key Escape
	fi
}

op_title() { target || return 0; trace title "$TARGET"; cmd "$TARGET" "title t$R"; }
op_app() {
	target || return 0
	local c=$TARGET
	has_main "$c" || return 0
	if [ "${APP[$c]}" = "$c" ]; then APP[$c]=$c-all; else APP[$c]=$c; fi
	trace app "$c" "${APP[$c]}"
	cmd "$c" "app ${APP[$c]}"
}
op_size() {
	target || return 0
	local s=$((150 + R % 400))x$((100 + R / 7 % 300))
	trace size "$TARGET" "$s"
	cmd "$TARGET" "size $s"
}
op_dialog() {
	target || return 0
	local c=$TARGET
	trace dialog "$c"
	cmd "$c" "dialog $c-d"
	has_main "$c" && check "$c-d is mapped" "[ -n \"\$(get $c-d x)\" ]"
}
op_undialog() { target || return 0; trace undialog "$TARGET"; cmd "$TARGET" undialog; }
op_cfull() {
	target || return 0
	trace client-fullscreen "$TARGET"
	if [ "$(get "${APP[$TARGET]}" full)" = 1 ]; then
		cmd "$TARGET" unfullscreen
	else
		cmd "$TARGET" fullscreen
	fi
}
op_cmin() { target || return 0; trace client-minimize "$TARGET"; cmd "$TARGET" minimize; }

# Back to nothing: no clients, no layout, desktop 1, the numbering from the start.
ops_reset() {
	local c
	for c in "${CLIENTS[@]}"; do
		kill "${PIDS[$c]}" 2>/dev/null
	done
	for c in "${CLIENTS[@]}"; do
		wait "${PIDS[$c]}" 2>/dev/null
		eval "exec ${CTL[$c]}>&-"
		rm -f "$SMOKE_DIR/$c.ctl"
	done
	CLIENTS=()
	CTL=() DID=() APP=() PIDS=()
	SEQ=0
	check "no window left" "! grep -q '^view' \"\$MWC_TEST_STATE\""
	refresh
	[ "$(global menu open)" = 1 ] && do_input key Escape
	[ "$(global tiling on)" = 1 ] && do_input key super+t
	[ "$(global desktop current)" = 1 ] || op_desktop 1
	TRACE=()
}
