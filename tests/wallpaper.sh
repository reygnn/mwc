#!/bin/bash
# Headless wallpaper test: renders generated PNGs through MWC_WALLPAPER and
# checks pixels in screenshots. Output goes to build/wallpaper.log.
LOG=build/wallpaper.log
. "$(dirname "$0")/lib.sh"

require_tools grim
for tool in build/vinput build/pngtool; do
	[ -x "$tool" ] || { echo "FAIL: $tool not built"; exit 1; }
done

SHOT=$SMOKE_DIR/shot.png
RED=ff0000
BLUE=0000ff
ROOT=5b7792

# pixel_is <description> <x> <y> <rrggbb>: poll, the first frame takes a moment.
pixel_is() {
	local what=$1 x=$2 y=$3 want=$4 got i
	for ((i = 0; i < 50; i++)); do
		if WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null; then
			got=$(build/pngtool pixel "$SHOT" "$x" "$y")
			[ "$got" = "$want" ] && { echo "ok: $what"; return 0; }
		fi
		sleep 0.1
	done
	fail "$what: pixel $x,$y is ${got:-unreadable}, expected $want"
}

# Same 16:9 as the 1280x720 output: the image fits exactly, scaled down.
build/pngtool stripes "$SMOKE_DIR/fit.png" 2560 1440 "$RED" "$BLUE"
# Far wider than the output: only the middle stays visible.
build/pngtool stripes "$SMOKE_DIR/wide.png" 3440 720 "$RED" "$BLUE"

# ---- exact fit, plus input on top of the wallpaper --------------------------

export MWC_TEST_STATE=$SMOKE_DIR/state
export MWC_WALLPAPER=$SMOKE_DIR/fit.png
start_mwc "sleep 120"
pixel_is "fitting image: left quarter" 100 360 "$RED"
pixel_is "fitting image: middle" 640 360 "$BLUE"
pixel_is "fitting image: right quarter" 1180 360 "$RED"
pixel_is "fitting image: reaches the top left corner" 0 0 "$RED"
pixel_is "fitting image: reaches the bottom right corner" 1279 719 "$RED"

start_vinput
do_input move 640 360
do_input click right
check "btn3 on the wallpaper opens the root menu" "[ \"\$(global menu open)\" = 1 ]"
do_input key Escape
stop_vinput
stop_mwc
unset MWC_TEST_STATE

# ---- wider than the output --------------------------------------------------

# 3440x720 keeps its scale of 1 and is centred, so 1080 px stick out on each
# side. The red quarters are 860 px wide: all of the visible part is blue.
export MWC_WALLPAPER=$SMOKE_DIR/wide.png
start_mwc "sleep 120"
pixel_is "wide image is cut off evenly: left edge" 0 360 "$BLUE"
pixel_is "wide image is cut off evenly: right edge" 1279 360 "$BLUE"
pixel_is "wide image fills the height: top" 640 0 "$BLUE"
pixel_is "wide image fills the height: bottom" 640 719 "$BLUE"
stop_mwc

# ---- a literal ~ --------------------------------------------------------------

export HOME=$SMOKE_DIR
export MWC_WALLPAPER='~/fit.png'
start_mwc "sleep 120"
pixel_is "a quoted ~/ is expanded" 640 360 "$BLUE"
stop_mwc

# ---- unusable files ---------------------------------------------------------

export MWC_WALLPAPER=$SMOKE_DIR/missing.png
start_mwc "sleep 120"
pixel_is "missing file: plain desktop colour" 640 360 "$ROOT"
grep -q 'cannot load wallpaper' "$LOG" || fail "missing file: no message in the log"
echo "ok: missing file: message in the log"
stop_mwc

echo "this is not a png" >"$SMOKE_DIR/broken.png"
export MWC_WALLPAPER=$SMOKE_DIR/broken.png
start_mwc "sleep 120"
pixel_is "broken file: plain desktop colour" 640 360 "$ROOT"
stop_mwc

unset MWC_WALLPAPER
start_mwc "sleep 120"
pixel_is "no wallpaper: plain desktop colour" 640 360 "$ROOT"

echo "PASS: wallpaper"
