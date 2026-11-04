# Common parts of the headless tests. Source it, then call start_mwc.
# Each test logs to build/<name>.log; the log is where to look after a failure.
set -u

MWC=./mwc
TIMEOUT=15

mkdir -p build
[ -x "$MWC" ] || { echo "FAIL: $MWC not built"; exit 1; }

# Keep this path short: Wayland socket paths are limited to 108 bytes.
base=${XDG_RUNTIME_DIR:-/tmp}
SMOKE_DIR=$(mktemp -d "$base/mwc.XXXXXX") || exit 1
export SMOKE_DIR

# Don't let the developer's own settings change what the tests see.
export XDG_CONFIG_HOME=$SMOKE_DIR/config
unset MWC_ACTIVE MWC_INACTIVE MWC_ROOT MWC_FONT MWC_WALLPAPER MWC_TERMINAL

pid=
cleanup() {
	if [ -n "$pid" ]; then
		kill "$pid" 2>/dev/null
		wait "$pid" 2>/dev/null
	fi
	rm -rf "$SMOKE_DIR"
}
trap cleanup EXIT

fail() {
	echo "FAIL: $1 (see $LOG)"
	exit 1
}

require_tools() {
	local tool
	for tool in "$@"; do
		command -v "$tool" >/dev/null || { echo "FAIL: $tool not installed"; exit 1; }
	done
}

# wait_for <description> <command...>: poll until true or mwc died.
wait_for() {
	local what=$1 i
	shift
	for ((i = 0; i < TIMEOUT * 5; i++)); do
		"$@" && { echo "ok: $what"; return 0; }
		kill -0 "$pid" 2>/dev/null || fail "mwc died while waiting for $what"
		sleep 0.2
	done
	fail "timed out waiting for $what"
}

# start_mwc <startup command>: mwc on a 1280x720 headless output, with the
# software renderer so no GPU is needed.
start_mwc() {
	export XDG_RUNTIME_DIR=$SMOKE_DIR
	export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
	unset WAYLAND_DISPLAY DISPLAY
	"$MWC" -s "$1" >"$LOG" 2>&1 &
	pid=$!
	wait_for "Wayland socket" test -S "$SMOKE_DIR/wayland-0"
}

# stop_mwc: SIGTERM must end it cleanly, so a crash on shutdown fails the test.
stop_mwc() {
	local status
	kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	status=$?
	pid=
	[ "$status" = 0 ] || fail "mwc exited with status $status"
	# Killed compositors leave their socket behind, which would make the
	# next start_mwc see it before the new one is listening.
	rm -f "$SMOKE_DIR"/wayland-*
}

# pixel_is <description> <x> <y> <rrggbb>: poll, frames take a moment to appear.
pixel_is() {
	local what=$1 x=$2 y=$3 want=$4 shot=$SMOKE_DIR/shot.png got i
	for ((i = 0; i < 50; i++)); do
		if WAYLAND_DISPLAY=wayland-0 grim "$shot" 2>/dev/null; then
			got=$(build/pngtool pixel "$shot" "$x" "$y")
			[ "$got" = "$want" ] && { echo "ok: $what"; return 0; }
		fi
		sleep 0.1
	done
	fail "$what: pixel $x,$y is ${got:-unreadable}, expected $want"
}

# ---- simulated input and window state (needs MWC_TEST_STATE) -----------------

# start_vinput [width height]: the size of the whole layout, default one output.
start_vinput() {
	local w=${1:-1280} h=${2:-720}
	coproc VINPUT { WAYLAND_DISPLAY=wayland-0 build/vinput "$w" "$h" 2>&1; }
}

stop_vinput() {
	eval "exec ${VINPUT[1]}>&-"
	wait "$VINPUT_PID" 2>/dev/null
}

# do_input <command>: send one command to vinput and wait for its answer.
do_input() {
	local answer
	echo "$*" >&"${VINPUT[1]}" || fail "vinput is gone (sending: $*)"
	read -r answer <&"${VINPUT[0]}" || fail "vinput is gone (sending: $*)"
	[ "$answer" = ok ] || fail "vinput said '$answer' to: $*"
}

refresh() {
	rm -f "$MWC_TEST_STATE"
	kill -USR1 "$pid" 2>/dev/null || fail "mwc died"
	local i
	for ((i = 0; i < 100; i++)); do
		[ -e "$MWC_TEST_STATE" ] && return 0
		sleep 0.02
	done
	fail "mwc wrote no state"
}

# get <app> <field>: a value from the last refreshed state.
get() {
	awk -v app="app=$1" -v key="$2=" '$1 == "view" && $2 == app {
		for (i = 3; i <= NF; i++)
			if (index($i, key) == 1) { print substr($i, length(key) + 1); exit }
	}' "$MWC_TEST_STATE"
}

# global <kind> <field>: a value from the "menu" or "cursor" line.
global() {
	awk -v kind="$1" -v key="$2=" '$1 == kind {
		for (i = 2; i <= NF; i++)
			if (index($i, key) == 1) { print substr($i, length(key) + 1); exit }
	}' "$MWC_TEST_STATE"
}

# check <description> <shell condition>: poll, because clients resize
# asynchronously. The condition sees freshly refreshed state.
check() {
	local what=$1 cond=$2 i
	for ((i = 0; i < 100; i++)); do
		refresh
		if eval "$cond"; then
			echo "ok: $what"
			return 0
		fi
		sleep 0.05
	done
	echo "--- state"
	cat "$MWC_TEST_STATE"
	fail "$what: never true: $cond"
}
