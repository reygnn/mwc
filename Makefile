# mwc - Motif-style Wayland compositor

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = wlroots-0.19 wayland-server xkbcommon pixman-1 cairo pangocairo

WAYLAND_PROTOCOLS = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER = $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror=implicit \
	-DWLR_USE_UNSTABLE -Ibuild $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

SRC = src/main.c src/view.c src/xwayland.c src/frame.c src/menu.c \
	src/input.c src/draw.c
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

clean:
	rm -rf build mwc

.PHONY: all install uninstall clean
