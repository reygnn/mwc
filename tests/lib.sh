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
