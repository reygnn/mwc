# mwc - Motif-style Wayland compositor

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = wlroots-0.19 wayland-server xkbcommon pixman-1 cairo pangocairo

ifeq ($(filter clean uninstall,$(MAKECMDGOALS)),)
MISSING := $(foreach p,$(PKGS) wayland-protocols wayland-scanner,\
	$(if $(shell $(PKG_CONFIG) --exists $(p) && echo ok),,$(p)))
ifneq ($(strip $(MISSING)),)
$(error Missing build dependencies: $(strip $(MISSING)) -- see README.md for the apt install line)
endif
endif

WAYLAND_PROTOCOLS = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER = $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror=implicit \
	-DWLR_USE_UNSTABLE -Ibuild $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

SRC = src/main.c src/view.c src/xwayland.c src/frame.c src/menu.c \
	src/input.c src/draw.c src/wallpaper.c src/config.c src/testhook.c
OBJ = $(SRC:src/%.c=build/%.o)
PROTO = build/xdg-shell-protocol.h

all: mwc

build:
	mkdir -p build

build/xdg-shell-protocol.h: | build
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

build/%.o: src/%.c src/mwc.h $(PROTO) | build
	$(CC) $(CFLAGS) -c -o $@ $<

mwc: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

install: mwc
	install -Dm755 mwc $(DESTDIR)$(PREFIX)/bin/mwc
	install -Dm644 data/mwc.desktop \
		$(DESTDIR)/usr/share/wayland-sessions/mwc.desktop

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

config-test: mwc build/vinput build/pngtool
	tests/config.sh

test: smoke input-test wallpaper-test config-test

clean:
	rm -rf build mwc

.SECONDARY: $(VINPUT_CODE)
.PHONY: all install uninstall smoke input-test wallpaper-test config-test test clean
