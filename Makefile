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
	src/hints.c src/outline.c src/snap.c src/tile.c src/tabs.c src/testhook.c
OBJ = $(SRC:src/%.c=build/%.o)
PROTO = build/xdg-shell-protocol.h build/wlr-layer-shell-unstable-v1-protocol.h \
	build/wlr-output-power-management-unstable-v1-protocol.h

all: mwc

build:
	mkdir -p build

build/xdg-shell-protocol.h: | build
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

build/wlr-layer-shell-unstable-v1-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml | build
	$(WAYLAND_SCANNER) server-header $< $@

build/wlr-output-power-management-unstable-v1-protocol.h: protocols/wlr-output-power-management-unstable-v1.xml | build
	$(WAYLAND_SCANNER) server-header $< $@

build/%.o: src/%.c src/mwc.h $(PROTO) | build
	$(CC) $(MWC_CFLAGS) $(CFLAGS) -c -o $@ $<

mwc: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(MWC_LIBS) $(LDLIBS)

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
	build/server-decoration-code.c

build/xdgc: tests/xdgc.c $(XDGC_GEN)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
		$(shell $(PKG_CONFIG) --cflags wayland-client) -o $@ tests/xdgc.c \
		build/xdg-decoration-unstable-v1-code.c build/xdg-shell-code.c \
		build/server-decoration-code.c \
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

tabs-test: mwc build/vinput build/pngtool build/xclient build/xdgc
	tests/tabs.sh

session-test: mwc build/vinput build/pngtool build/xclient build/powerc
	tests/session.sh

misc-test: mwc build/vinput build/pngtool build/xclient build/xdgc
	tests/misc.sh

state-test: mwc build/vinput build/pngtool build/xclient build/xdgc build/badclient build/layerc
	tests/state.sh

pointer-test: mwc build/vinput build/pngtool build/xclient build/badclient
	tests/pointer.sh

crash-test: mwc build/vinput build/xclient build/xdgc build/badclient build/layerc
	tests/crash.sh

outputs-test: mwc build/vinput build/pngtool build/xclient build/layerc
	tests/outputs.sh

tile-test: mwc build/vinput build/pngtool build/xclient build/xdgc
	tests/tile.sh

snap-test: mwc build/vinput build/pngtool build/xclient build/xdgc
	tests/snap.sh

move-test: mwc build/vinput build/pngtool build/xclient build/xdgc
	tests/move.sh

window-test: mwc build/vinput build/pngtool build/xclient build/xdgc
	tests/window.sh

layer-test: mwc build/vinput build/pngtool build/layerc
	tests/layer.sh

config-test: mwc build/vinput build/pngtool
	tests/config.sh

test: smoke input-test wallpaper-test config-test layer-test window-test \
	move-test snap-test outputs-test tile-test crash-test pointer-test state-test misc-test session-test tabs-test

clean:
	rm -rf build mwc

.SECONDARY: $(VINPUT_CODE) $(LAYERC_GEN) $(XDGC_GEN) $(POWERC_GEN)
.PHONY: tabs-test all install uninstall smoke input-test wallpaper-test config-test layer-test window-test move-test snap-test outputs-test tile-test crash-test pointer-test state-test misc-test session-test test clean
