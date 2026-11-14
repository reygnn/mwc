# mwc - Motif-style Wayland compositor

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = wlroots-0.19 wayland-server xkbcommon pixman-1 cairo pangocairo xcb xcb-icccm

ifeq ($(filter clean uninstall,$(MAKECMDGOALS)),)
MISSING := $(foreach p,$(PKGS) wayland-protocols wayland-scanner,\
	$(if $(shell $(PKG_CONFIG) --exists $(p) && echo ok),,$(p)))
ifneq ($(strip $(MISSING)),)
$(error Missing build dependencies: $(strip $(MISSING)) -- see README.md for the apt install line)
endif
endif

WAYLAND_PROTOCOLS = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER = $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)

# CFLAGS and LDLIBS stay the user's, also on the command line: `make CFLAGS=-O0` must
# not lose the include paths.
CFLAGS ?= -O2 -g
MWC_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror=implicit \
	-DWLR_USE_UNSTABLE -Ibuild $(shell $(PKG_CONFIG) --cflags $(PKGS))
MWC_LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS))

SRC = src/main.c src/view.c src/xwayland.c src/frame.c src/menu.c \
	src/input.c src/draw.c src/wallpaper.c src/config.c src/layer.c \
	src/hints.c src/outline.c src/snap.c src/tile.c src/tabs.c src/testhook.c \
	src/shmalloc.c src/fifo.c src/desktop.c src/workspace.c src/iconbox.c \
	src/dock.c
# Protocols mwc implements itself, not through wlroots: their glue code too.
PROTO_CODE = build/fifo-v1-protocol.c build/ext-workspace-v1-protocol.c
OBJ = $(SRC:src/%.c=build/%.o) $(PROTO_CODE:.c=.o)
PROTO = build/xdg-shell-protocol.h build/wlr-layer-shell-unstable-v1-protocol.h \
	build/wlr-output-power-management-unstable-v1-protocol.h \
	build/ext-image-copy-capture-v1-protocol.h build/fifo-v1-protocol.h \
	build/ext-workspace-v1-protocol.h

all: mwc

build:
	mkdir -p build

build/xdg-shell-protocol.h: | build
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

# wlroots' own header for it includes this one. In wayland-protocols from 1.37 on.
build/ext-image-copy-capture-v1-protocol.h: | build
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml $@

# wp_fifo_v1, in wayland-protocols from 1.38 on; wlroots 0.19 has no implementation.
build/fifo-v1-protocol.h: | build
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/staging/fifo/fifo-v1.xml $@

build/fifo-v1-protocol.c: | build
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/staging/fifo/fifo-v1.xml $@

# ext-workspace-v1, in wayland-protocols from 1.40 on; none in wlroots 0.19 either.
build/ext-workspace-v1-protocol.h: | build
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/staging/ext-workspace/ext-workspace-v1.xml $@

build/ext-workspace-v1-protocol.c: | build
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/staging/ext-workspace/ext-workspace-v1.xml $@

build/%-protocol.o: build/%-protocol.c
	$(CC) $(MWC_CFLAGS) $(CFLAGS) -c -o $@ $<

build/wlr-layer-shell-unstable-v1-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml | build
	$(WAYLAND_SCANNER) server-header $< $@

build/wlr-output-power-management-unstable-v1-protocol.h: protocols/wlr-output-power-management-unstable-v1.xml | build
	$(WAYLAND_SCANNER) server-header $< $@

build/%.o: src/%.c src/mwc.h $(PROTO) | build
	$(CC) $(MWC_CFLAGS) $(CFLAGS) -c -o $@ $<

# The icon box's logo, built in: a C array of the PNG's bytes.
build/logo.h: data/logo.png | build
	{ echo 'static const unsigned char logo_png[] = {'; \
	  od -An -v -tx1 $< | sed 's/ *\([0-9a-f][0-9a-f]\)/0x\1,/g'; \
	  echo '};'; } >$@

build/iconbox.o: build/logo.h

build/main.o: src/version.h

mwc: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(MWC_LIBS) $(LDLIBS)

# make check: the whole test suite against a build with AddressSanitizer and
# UndefinedBehaviorSanitizer, in build/asan beside the normal one. A finding aborts
# mwc, which the test then reports; its report is in that test's log. Leaks are not
# checked: wlroots and Pango keep memory until exit. Warnings are errors here, not in
# the normal build, where a newer compiler's new warning would only stop people.
SAN_FLAGS = -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	-fno-sanitize-recover=undefined -Werror
SAN_OBJ = $(SRC:src/%.c=build/asan/%.o) $(PROTO_CODE:build/%.c=build/asan/%.o)

build/asan: | build
	mkdir -p build/asan

build/asan/%.o: src/%.c src/mwc.h $(PROTO) | build/asan
	$(CC) $(MWC_CFLAGS) $(SAN_FLAGS) -c -o $@ $<

build/asan/%-protocol.o: build/%-protocol.c | build/asan
	$(CC) $(MWC_CFLAGS) $(SAN_FLAGS) -c -o $@ $<

build/asan/iconbox.o: build/logo.h

build/asan/main.o: src/version.h

build/mwc-asan: $(SAN_OBJ)
	$(CC) $(SAN_FLAGS) -o $@ $(SAN_OBJ) $(MWC_LIBS)

check: build/mwc-asan
	MWC=build/mwc-asan MWC_SANITIZED=1 ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
		UBSAN_OPTIONS=print_stacktrace=1 $(MAKE) test
	@if command -v $(FUZZ_CC) >/dev/null; then $(MAKE) fuzz-replay; \
	else echo "make check: no $(FUZZ_CC), the fuzz inputs in fuzz/ were not replayed"; fi

# make analyze: GCC's static analyzer over every source file, findings as errors:
# NULL dereferences, use after free, double free, leaks on a path. Clean today, so a
# new finding stops it. At -O0: optimizing first lets GCC drop the very paths that
# dereference NULL. clang's analyzer is left out: wayland's list macros make it
# report what isn't there.
analyze: $(PROTO) build/logo.h
	@for f in $(SRC); do \
		echo "analyze $$f"; \
		$(CC) $(MWC_CFLAGS) -O0 -fanalyzer -Werror -c $$f -o /dev/null || exit 1; \
	done

# make lint: C lines longer than LINT_MAX columns, tabs to every eighth column as the
# kernel's checkpatch counts them. Shell scripts are left out: their checks are long
# conditions that read better whole.
LINT_MAX = 100
LINT_FILES = src/*.c src/*.h tests/*.c fuzz/*.c

lint:
	@awk -v max=$(LINT_MAX) '{ \
		n = 0; \
		for (i = 1; i <= length($$0); i++) \
			n = substr($$0, i, 1) == "\t" ? n + 8 - n % 8 : n + 1; \
		if (n > max) { print FILENAME ":" FNR ": " n " columns"; bad = 1 } \
	} END { exit bad }' $(LINT_FILES)

# make valgrind: the tests against mwc in valgrind's memcheck (tests/valgrind-mwc),
# for what the sanitizers don't see: reads of uninitialized memory, and errors inside
# wlroots, Pango and cairo, which aren't built with them. Many times slower, so not
# part of make test or make check, and without the scripts that go by timing.
# Left out, as they go by timing a slowed-down mwc can't keep: layer, move, fifo, rdp,
# and state, whose clients act on timers of their own.
VALGRIND_TESTS = smoke input-test wallpaper-test config-test window-test snap-test \
	outputs-test tile-test crash-test pointer-test misc-test session-test \
	tabs-test desktop-test iconbox-test dock-test

valgrind: mwc
	MWC=tests/valgrind-mwc MWC_VALGRIND=1 MWC_TIMEOUT=90 $(MAKE) $(VALGRIND_TESTS)

# Fuzzing the mwmrc parser with libFuzzer, which comes with clang (see distrobox.ini).
# make fuzz runs for FUZZ_TIME seconds, from the inputs kept in fuzz/corpus/mwmrc and
# fuzz/findings; what it finds new goes to build/fuzz-corpus, a crash to build/ as
# crash-*. A crash, once fixed, is kept in fuzz/findings, which fuzz-replay and make
# check run again.
FUZZ_CC = clang
FUZZ_TIME ?= 60
FUZZ_FLAGS = -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined

build/fuzz-mwmrc: fuzz/mwmrc.c src/config.c src/mwc.h $(PROTO) | build
	$(FUZZ_CC) $(MWC_CFLAGS) $(FUZZ_FLAGS) -Werror -o $@ fuzz/mwmrc.c \
		$(shell $(PKG_CONFIG) --libs pango)

fuzz: build/fuzz-mwmrc
	mkdir -p build/fuzz-corpus
	build/fuzz-mwmrc -max_total_time=$(FUZZ_TIME) -max_len=4096 \
		-artifact_prefix=build/ build/fuzz-corpus fuzz/corpus/mwmrc fuzz/findings

fuzz-replay: build/fuzz-mwmrc
	build/fuzz-mwmrc -runs=0 fuzz/corpus/mwmrc fuzz/findings

install: mwc
	install -Dm755 mwc $(DESTDIR)$(PREFIX)/bin/mwc
	install -d $(DESTDIR)/usr/share/wayland-sessions
	sed 's|^Exec=mwc$$|Exec=$(PREFIX)/bin/mwc|' data/mwc.desktop \
		>$(DESTDIR)/usr/share/wayland-sessions/mwc.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mwc \
		$(DESTDIR)/usr/share/wayland-sessions/mwc.desktop

smoke: mwc
	tests/smoke.sh

# Simulated mouse and keyboard for the input test.
VINPUT_PROTOS = wlr-virtual-pointer-unstable-v1 virtual-keyboard-unstable-v1
VINPUT_HEADERS = $(VINPUT_PROTOS:%=build/%-client.h)
VINPUT_CODE = $(VINPUT_PROTOS:%=build/%-code.c)
VINPUT_PKGS = wayland-client xkbcommon

build/%-client.h: protocols/%.xml | build
	$(WAYLAND_SCANNER) client-header $< $@

build/%-code.c: protocols/%.xml | build
	$(WAYLAND_SCANNER) private-code $< $@

build/vinput: tests/vinput.c $(VINPUT_HEADERS) $(VINPUT_CODE)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
		$(shell $(PKG_CONFIG) --cflags $(VINPUT_PKGS)) \
		-o $@ tests/vinput.c $(VINPUT_CODE) \
		$(shell $(PKG_CONFIG) --libs $(VINPUT_PKGS))

input-test: mwc build/vinput
	tests/input.sh

# Writes test images and reads pixels from screenshots.
build/pngtool: tests/pngtool.c | build
	$(CC) -O2 -g -std=c11 -Wall -Wextra \
		$(shell $(PKG_CONFIG) --cflags cairo) \
		-o $@ tests/pngtool.c $(shell $(PKG_CONFIG) --libs cairo)

wallpaper-test: mwc build/vinput build/pngtool
	tests/wallpaper.sh

# A layer-shell client: a panel or wallpaper tool to test against.
LAYERC_GEN = build/wlr-layer-shell-unstable-v1-client.h \
	build/wlr-layer-shell-unstable-v1-code.c \
	build/xdg-shell-client.h build/xdg-shell-code.c

build/xdg-shell-client.h: | build
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

build/xdg-shell-code.c: | build
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

build/viewporter-client.h: | build
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml $@

build/viewporter-code.c: | build
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml $@

build/layerc: tests/layerc.c $(LAYERC_GEN)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
		$(shell $(PKG_CONFIG) --cflags wayland-client) -o $@ tests/layerc.c \
		build/wlr-layer-shell-unstable-v1-code.c build/xdg-shell-code.c \
		$(shell $(PKG_CONFIG) --libs wayland-client)

# Clients that can ask for any window property.
build/xclient: tests/xclient.c | build
	$(CC) -O2 -g -std=c11 -Wall -Wextra -o $@ tests/xclient.c \
		$(shell $(PKG_CONFIG) --cflags --libs x11)

build/xdg-decoration-unstable-v1-client.h: | build
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

build/xdg-decoration-unstable-v1-code.c: | build
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

XDGC_GEN = build/xdg-decoration-unstable-v1-client.h \
	build/xdg-decoration-unstable-v1-code.c build/xdg-shell-client.h \
	build/xdg-shell-code.c build/server-decoration-client.h \
	build/server-decoration-code.c build/viewporter-client.h \
	build/viewporter-code.c

build/xdgc: tests/xdgc.c $(XDGC_GEN)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
		$(shell $(PKG_CONFIG) --cflags wayland-client) -o $@ tests/xdgc.c \
		build/xdg-decoration-unstable-v1-code.c build/xdg-shell-code.c \
		build/server-decoration-code.c build/viewporter-code.c \
		$(shell $(PKG_CONFIG) --libs wayland-client)

build/badclient: tests/badclient.c build/xdg-shell-client.h build/xdg-shell-code.c
	$(CC) -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
		$(shell $(PKG_CONFIG) --cflags wayland-client) -o $@ tests/badclient.c \
		build/xdg-shell-code.c $(shell $(PKG_CONFIG) --libs wayland-client)

# Switches outputs off and on.
POWERC_GEN = build/wlr-output-power-management-unstable-v1-client.h \
	build/wlr-output-power-management-unstable-v1-code.c

build/powerc: tests/powerc.c $(POWERC_GEN)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
		$(shell $(PKG_CONFIG) --cflags wayland-client) -o $@ tests/powerc.c \
		build/wlr-output-power-management-unstable-v1-code.c \
		$(shell $(PKG_CONFIG) --libs wayland-client)

# Sets outputs as wlr-randr does, and captures them as a VNC or RDP server does.
OUTPUTC_GEN = build/wlr-output-management-unstable-v1-client.h \
	build/wlr-output-management-unstable-v1-code.c

build/outputc: tests/outputc.c $(OUTPUTC_GEN)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
		$(shell $(PKG_CONFIG) --cflags wayland-client) -o $@ tests/outputc.c \
		build/wlr-output-management-unstable-v1-code.c \
		$(shell $(PKG_CONFIG) --libs wayland-client)

CAPTURE_PROTOS = ext-image-copy-capture ext-image-capture-source ext-foreign-toplevel-list
CAPTUREC_GEN = $(CAPTURE_PROTOS:%=build/%-v1-client.h) $(CAPTURE_PROTOS:%=build/%-v1-code.c)

build/ext-%-v1-client.h: | build
	$(WAYLAND_SCANNER) client-header $(WAYLAND_PROTOCOLS)/staging/ext-$*/ext-$*-v1.xml $@

build/ext-%-v1-code.c: | build
	$(WAYLAND_SCANNER) private-code $(WAYLAND_PROTOCOLS)/staging/ext-$*/ext-$*-v1.xml $@

build/capturec: tests/capturec.c $(CAPTUREC_GEN)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
		$(shell $(PKG_CONFIG) --cflags wayland-client cairo) -o $@ tests/capturec.c \
		$(CAPTURE_PROTOS:%=build/%-v1-code.c) \
		$(shell $(PKG_CONFIG) --libs wayland-client cairo)

# Idle: watching for it as a locker does, keeping it away as a video player does.
IDLEC_GEN = build/ext-idle-notify-v1-client.h build/ext-idle-notify-v1-code.c \
	build/idle-inhibit-unstable-v1-client.h build/idle-inhibit-unstable-v1-code.c \
	build/xdg-shell-client.h build/xdg-shell-code.c

build/idle-inhibit-unstable-v1-client.h: | build
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml $@

build/idle-inhibit-unstable-v1-code.c: | build
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml $@

build/idlec: tests/idlec.c $(IDLEC_GEN)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
		$(shell $(PKG_CONFIG) --cflags wayland-client) -o $@ tests/idlec.c \
		build/ext-idle-notify-v1-code.c build/idle-inhibit-unstable-v1-code.c \
		build/xdg-shell-code.c $(shell $(PKG_CONFIG) --libs wayland-client)

# A window that waits for the refresh with wp_fifo_v1, as SDL 3 does.
FIFOC_GEN = build/fifo-v1-client.h build/fifo-v1-client-code.c \
	build/xdg-shell-client.h build/xdg-shell-code.c

build/fifo-v1-client.h: | build
	$(WAYLAND_SCANNER) client-header $(WAYLAND_PROTOCOLS)/staging/fifo/fifo-v1.xml $@

build/fifo-v1-client-code.c: | build
	$(WAYLAND_SCANNER) private-code $(WAYLAND_PROTOCOLS)/staging/fifo/fifo-v1.xml $@

build/fifoc: tests/fifoc.c $(FIFOC_GEN)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
		$(shell $(PKG_CONFIG) --cflags wayland-client) -o $@ tests/fifoc.c \
		build/fifo-v1-client-code.c build/xdg-shell-code.c \
		$(shell $(PKG_CONFIG) --libs wayland-client)

fifo-test: mwc build/fifoc
	tests/fifo.sh

# Lists and switches the desktops, as a pager does.
build/workspacec: tests/workspacec.c build/ext-workspace-v1-client.h build/ext-workspace-v1-code.c
	$(CC) -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
		$(shell $(PKG_CONFIG) --cflags wayland-client) -o $@ tests/workspacec.c \
		build/ext-workspace-v1-code.c $(shell $(PKG_CONFIG) --libs wayland-client)

desktop-test: mwc build/vinput build/xclient build/xdgc build/workspacec build/outputc
	tests/desktop.sh

# What an RDP server such as wayrdp does: devices that go mid-press, fresh captures,
# sizes per connection.
rdp-test: mwc build/vinput build/xdgc build/outputc build/capturec build/powerc \
		build/pngtool
	tests/rdp.sh

tabs-test: mwc build/vinput build/pngtool build/xclient build/xdgc
	tests/tabs.sh

session-test: mwc build/vinput build/pngtool build/xclient build/powerc build/outputc \
		build/capturec build/idlec
	tests/session.sh

misc-test: mwc build/vinput build/pngtool build/xclient build/xdgc build/badclient
	tests/misc.sh

state-test: mwc build/vinput build/pngtool build/xclient build/xdgc build/badclient build/layerc
	tests/state.sh

pointer-test: mwc build/vinput build/pngtool build/xclient build/badclient
	tests/pointer.sh

crash-test: mwc build/vinput build/xclient build/xdgc build/badclient build/layerc
	tests/crash.sh

outputs-test: mwc build/vinput build/pngtool build/xclient build/layerc
	tests/outputs.sh

tile-test: mwc build/vinput build/pngtool build/xclient build/xdgc build/outputc
	tests/tile.sh

snap-test: mwc build/vinput build/pngtool build/xclient build/xdgc
	tests/snap.sh

move-test: mwc build/vinput build/pngtool build/xclient build/xdgc
	tests/move.sh

window-test: mwc build/vinput build/pngtool build/xclient build/xdgc
	tests/window.sh

iconbox-test: mwc build/vinput build/pngtool build/xdgc build/workspacec build/layerc
	tests/iconbox.sh

dock-test: mwc build/vinput build/xclient build/xdgc build/pngtool build/workspacec
	tests/dock.sh

layer-test: mwc build/vinput build/pngtool build/layerc build/xdgc
	tests/layer.sh

config-test: mwc build/vinput build/pngtool
	tests/config.sh

test: smoke input-test wallpaper-test config-test layer-test window-test \
	move-test snap-test outputs-test tile-test crash-test pointer-test state-test misc-test session-test tabs-test fifo-test rdp-test desktop-test iconbox-test dock-test

clean:
	rm -rf build mwc

.SECONDARY: $(VINPUT_CODE) $(LAYERC_GEN) $(XDGC_GEN) $(POWERC_GEN) $(OUTPUTC_GEN) \
	$(CAPTUREC_GEN) $(FIFOC_GEN) $(IDLEC_GEN) build/ext-workspace-v1-client.h \
	build/ext-workspace-v1-code.c
.PHONY: lint analyze valgrind check fuzz fuzz-replay dock-test iconbox-test desktop-test rdp-test fifo-test tabs-test all install uninstall smoke input-test wallpaper-test config-test layer-test window-test move-test snap-test outputs-test tile-test crash-test pointer-test state-test misc-test session-test test clean
