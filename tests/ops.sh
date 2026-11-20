# What a user and clients do to windows, for matrix.sh and walk.sh: each operation
# acts on the window with the focus, or on a client, and returns once mwc has handled
# it. After each, inv checks tests/invariants.awk against mwc's state.
#
# Clients are xdgc windows c1, c2, ..., each driven through control=FIFO. Their app_id
# switches between cN and cN-all, which mwmrc puts on all desktops.

declare -a CLIENTS=()
declare -A CTL=() DID=() APP=() PIDS=()
SEQ=0
TRACE=() # the operations of the current case or walk, for a report

# mwmrc for the operations: cN-all on all desktops, for N up to 200.
ops_rc() {
	local i
	echo 'Mwm*outputManagement: true'
	for ((i = 1; i <= 200; i++)); do
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

trace() { TRACE+=("$*"); echo "op: $*" >>"$LOG"; }

# The client a window belongs to: cN for cN, cN-all and cN-d.
owner() {
	local a=${1%-all}
	echo "${a%-d}"
}

focused_app() { awk '$1 == "view" && / active=1 / { sub("app=", "", $2); print $2; exit }' \
	"$MWC_TEST_STATE"; }

alive() { [ -n "${PIDS[$1]-}" ] && kill -0 "${PIDS[$1]}" 2>/dev/null; }

# A client's windows are gone from the state when it is: forget it then.
prune() {
	local c keep=()
	for c in "${CLIENTS[@]}"; do
		if alive "$c"; then
			keep+=("$c")
		else
			wait "${PIDS[$c]}" 2>/dev/null
			eval "exec ${CTL[$c]}>&-"
			unset "CTL[$c]" "DID[$c]" "APP[$c]" "PIDS[$c]"
		fi
	done
	CLIENTS=("${keep[@]}")
}

op_spawn() {
	local c=c$((++SEQ))
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

# The client of the window with the focus, or the first one.
target() {
	local a
	refresh
	a=$(focused_app)
	if [ -n "$a" ]; then
		owner "$a"
	elif [ ${#CLIENTS[@]} -gt 0 ]; then
		echo "${CLIENTS[0]}"
	fi
}

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

op_restore() {
	local c a
	refresh
	for c in "${CLIENTS[@]}"; do
		for a in "${APP[$c]}" "$c-d"; do
			if [ "$(get "$a" min)" = 1 ] && [ "$(get "$a" icon_shown)" = 1 ]; then
				trace restore "$a"
				do_input move $(($(get "$a" icon_x) + 20)) $(($(get "$a" icon_y) + 20))
				do_input click left
				do_input click left
				do_input move 5 5
				return
			fi
		done
	done
	trace restore nothing
}

op_desktop() { trace desktop "$1"; WAYLAND_DISPLAY=wayland-0 build/workspacec activate "$1" \
	>/dev/null || fail "desktop $1"; }

op_reload() { trace reload; reload_mwc; }

op_sticky() {
	trace sticky
	refresh
	[ -n "$(focused_app)" ] || return 0
	do_input key alt+space
	refresh
	if [ "$(global menu open)" = 1 ] && [[ $(global menu sensitive) == *O* ]]; then
		do_input key o
	else
		do_input key Escape
	fi
}

op_title() { local c; c=$(target); [ -n "$c" ] || return 0; trace title "$c"
	cmd "$c" "title t$RANDOM"; }
op_app() {
	local c
	c=$(target)
	[ -n "$c" ] || return 0
	if [ "${APP[$c]}" = "$c" ]; then APP[$c]=$c-all; else APP[$c]=$c; fi
	trace app "$c" "${APP[$c]}"
	cmd "$c" "app ${APP[$c]}"
}
op_size() { local c; c=$(target); [ -n "$c" ] || return 0
	local s=$((150 + RANDOM % 400))x$((100 + RANDOM % 300)); trace size "$c" "$s"
	cmd "$c" "size $s"; }
op_dialog() { local c; c=$(target); [ -n "$c" ] || return 0; trace dialog "$c"
	cmd "$c" "dialog $c-d"; }
op_undialog() { local c; c=$(target); [ -n "$c" ] || return 0; trace undialog "$c"
	cmd "$c" undialog; }
op_cfull() { local c; c=$(target); [ -n "$c" ] || return 0; trace client-fullscreen "$c"
	if [ "$(get "${APP[$c]}" full)" = 1 ]; then cmd "$c" unfullscreen; else cmd "$c" fullscreen; fi; }
op_cmin() { local c; c=$(target); [ -n "$c" ] || return 0; trace client-minimize "$c"
	cmd "$c" minimize; }

# Back to nothing: no clients, no layout, desktop 1.
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
	check "no window left" "! grep -q '^view' \"\$MWC_TEST_STATE\""
	do_input key Escape
	refresh
	[ "$(global tiling on)" = 1 ] && do_input key super+t
	[ "$(global desktop current)" = 1 ] || op_desktop 1
	TRACE=()
}
