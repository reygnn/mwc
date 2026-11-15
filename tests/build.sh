#!/bin/bash
# The Makefile: the user's CPPFLAGS reach every compile line, generated protocol
# files follow their XML, the installed session file is readable by all.
# Dry runs of make, and an install into a scratch DESTDIR.
# Output goes to build/build.log. Exits non-zero on failure.
LOG=build/build.log
. "$(dirname "$0")/lib.sh"
exec 3>&1 >"$LOG" 2>&1

say() { echo "$1" | tee /dev/fd/3; }
bad() { echo "FAIL: $1 (see $LOG)" >&3; exit 1; }

# ---- CPPFLAGS ----------------------------------------------------------------------

for target in build/draw.o build/asan/draw.o build/fifo-v1-protocol.o \
	build/asan/fifo-v1-protocol.o build/fuzz-mwmrc analyze; do
	make -n -B CPPFLAGS=-DMWC_BUILD_TEST "$target" | tee -a "$LOG" |
		grep -q -- -DMWC_BUILD_TEST || bad "CPPFLAGS don't reach $target"
done
say "ok: CPPFLAGS reach every compile line"

# ---- generated files follow their XML ----------------------------------------------

WP=$(pkg-config --variable=pkgdatadir wayland-protocols)
while read -r target xml; do
	make "$target" || bad "can't make $target"
	make -n -W "$WP/$xml" "$target" | grep -q wayland-scanner ||
		bad "$target isn't made again after $xml changes"
done <<EOF
build/xdg-shell-protocol.h stable/xdg-shell/xdg-shell.xml
build/ext-image-copy-capture-v1-protocol.h staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
build/fifo-v1-protocol.c staging/fifo/fifo-v1.xml
build/ext-workspace-v1-protocol.h staging/ext-workspace/ext-workspace-v1.xml
build/xdg-shell-code.c stable/xdg-shell/xdg-shell.xml
build/viewporter-client.h stable/viewporter/viewporter.xml
build/xdg-decoration-unstable-v1-code.c unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
build/idle-inhibit-unstable-v1-client.h unstable/idle-inhibit/idle-inhibit-unstable-v1.xml
build/fifo-v1-client.h staging/fifo/fifo-v1.xml
build/ext-image-capture-source-v1-client.h staging/ext-image-capture-source/ext-image-capture-source-v1.xml
build/ext-workspace-v1-code.c staging/ext-workspace/ext-workspace-v1.xml
EOF
say "ok: generated protocol files are made again when their XML changes"

# ---- install ----------------------------------------------------------------------

DEST=$SMOKE_DIR/dest
(umask 077 && make install DESTDIR="$DEST" PREFIX=/usr) || bad "make install failed"
mode=$(stat -c %a "$DEST/usr/share/wayland-sessions/mwc.desktop")
[ "$mode" = 644 ] || bad "mwc.desktop is installed with mode $mode under umask 077"
grep -qx 'Exec=/usr/bin/mwc' "$DEST/usr/share/wayland-sessions/mwc.desktop" ||
	bad "mwc.desktop doesn't run /usr/bin/mwc"
say "ok: mwc.desktop is installed with mode 644"

say "PASS"
