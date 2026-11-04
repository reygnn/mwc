# mwc — Motif-style Wayland compositor

A small wlroots 0.19 compositor that draws the classic Motif window manager
look server-side: 3D bevels, resize border with corner grooves, window menu
button, minimize/maximize buttons, desktop icons and mwm's menus and key
bindings. X11 applications (including real Motif apps) run through XWayland
and get the same frame.

## Build (Ubuntu 26.04)

    sudo apt install build-essential pkg-config libwlroots-0.19-dev \
        wayland-protocols libwayland-dev libxkbcommon-dev libpixman-1-dev \
        libcairo2-dev libpango1.0-dev libxcb1-dev libxcb-icccm4-dev xwayland
    make
    sudo make install   # /usr/local/bin/mwc + GDM session entry

## Run

- **Nested** inside the running GNOME session (opens as a window):
  `./mwc -s xterm`
- **Real session**: log out, pick "Mwc (Motif)" in GDM's gear menu, or start
  `mwc` from a TTY.

`~/.config/mwc/autostart` (executable) runs on startup unless `-s` is given.

## Develop and test in a container

`distrobox.ini` defines a rootless box with all dependencies, so nothing has to
be installed on the host except Podman and Distrobox:

    sudo apt install podman distrobox uidmap
    distrobox assemble create --replace --file distrobox.ini
    distrobox enter mwc-dev
    make test           # headless tests
    ./mwc -s xterm      # nested, opens as a window

`make smoke` starts mwc headless and checks that a Wayland client (foot) and an
X11 client (xterm) connect, with a screenshot in `build/smoke.png`.

`make input-test` drives the same two clients with a simulated mouse and
keyboard (`tests/vinput.c`) and checks geometry and focus after each move,
resize, maximize, minimize, menu and key binding. The simulated devices and the
state file only exist when `MWC_TEST_STATE` is set, because those protocols let
any client type on the user's behalf. Other protocols are always on and open to every
client, as in most wlroots compositors: `wlr-screencopy` lets any of them read the
screen, and `wlr-layer-shell` lets any of them cover it.

`make wallpaper-test` renders generated images and checks pixels in screenshots,
and `make config-test` does the same for `mwmrc`, including reloading it.
`make layer-test` checks panels with a small layer-shell client
(`tests/layerc.c`), and `make window-test` window behaviour with clients that can
ask for any hint (`tests/xclient.c`, `tests/xdgc.c`). `make move-test` checks the
outline, the box and the keys during a move or resize, and `make snap-test` the
snap zones and every way back out of them. `make outputs-test` runs on two outputs,
`make tile-test` walks through tiling, `make pointer-test` checks who gets the mouse
and keyboard when, including drag and drop, and `make crash-test` tries what used to
end the session, which is worth running against a sanitizer build too:

    make clean
    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined" \
        LDFLAGS="-fsanitize=address,undefined" \
        ASAN_OPTIONS=detect_leaks=0 make test

`make state-test` tries orderings of window states, and `make misc-test` key bindings,
autostart and settings the others don't reach. `make session-test` checks monitor and
keyboard settings and the properties X11 programs look for.

`make test` runs them all. None of this tests real input hardware, DRM/KMS or VT
switching: use a VM or a free TTY for those.

Inside the box, the host's X server isn't reachable: `/tmp/.X11-unix` is a
private tmpfs because XWayland won't start without it. X11 apps only run inside
mwc.

## Mouse

| Where              | Action                                              |
|--------------------|-----------------------------------------------------|
| Title bar          | drag: move · double-click: maximize · btn2: lower · btn3: menu |
| Menu button        | click: window menu · double-click: close            |
| Border / corners   | resize                                              |
| Icon               | double-click: restore · btn3: window menu           |
| Desktop            | btn1/btn3: root menu                                |
| Alt + btn1/2/3     | move / lower / resize anywhere in the window        |

## Keys (mwm defaults)

Alt+Tab / Alt+Shift+Tab (or Alt+Esc) cycle · Alt+Space or Shift+Esc window menu ·
Alt+F3 lower · Alt+F4 close · Alt+F5 restore · Alt+F7 move · Alt+F8 size ·
Alt+F9 minimize · Alt+F10 maximize · Alt+F11 fullscreen ·
Ctrl+Alt+Return / Super+Return terminal · Ctrl+Alt+Backspace quit ·
Super+arrows snap (see below) · Super+Shift+arrows push a floating window against
that edge of the usable area (two in a row: a corner) · Super+T tiling on/off.

## Configuration

`~/.config/mwc/mwmrc` uses the same Motif resource syntax as mwm-x11. Each
program can get its own frame colour, matched by an X11 window's `WM_CLASS`
(instance or class name) or a Wayland client's `app_id`, ignoring case:

```
! ~/.config/mwc/mwmrc
Mwm*XTerm*activeBackground:    #8a0303
Mwm*foot*activeBackground:     gold

! global defaults
Mwm*activeBackground:          #7387ad
Mwm*background:                #aeb2c3
Mwm*fontList:                  Sans Bold 10
Mwm*wallpaper:                 ~/pictures/desk.png
```

| Resource | Meaning |
|---|---|
| `activeBackground` | frame colour while focused |
| `background` | frame colour while unfocused (globally: also menus and icons) |
| `activeForeground`, `foreground` | title text; default is black or white, whichever contrasts |
| `fontList` | Pango font name for titles and menus, title bars grow with it (global only) |
| `rootBackground` | desktop colour, or `none` to leave the desktop to a wallpaper tool such as `swaybg` (global only) |
| `wallpaper` | PNG on the desktop, see below (global only) |
| `moveOpaque` | `true`: drag windows themselves instead of an outline (global only) |
| `tiling` | `true`: start in tiling mode (global only) |
| `tileMainWidth` | share of the width for the main tiled window, `10` to `90` (`%` optional), default `55` (global only) |
| `DoNotTile` | programs that always float in tiling mode, comma-separated (global only) |
| `TileOnly` | the only programs that tile, comma-separated; the later of the two wins (global only) |

Copying a file from mwm-x11 works, except that `fontList` takes Pango names
like `Sans Bold 10`: anything else, like `Sans:bold:size=10` or `Sans-12`, is logged
and ignored. Resources mwc doesn't know are ignored too.

A program with its own `activeBackground` keeps its colour when unfocused, in a
paler shade (mixed with the global `background`), and so does its desktop icon.
Set `Mwm*Name*background` to choose the unfocused colour yourself. Colours are
`#rrggbb`, names like `gold` or `firebrick`, or `rgb:rr/gg/bb`. Lines starting
with `!` are comments; a `!` after a value is part of the value, so comments go
on their own line. Later lines win.

A program's `app_id` may contain dots: `Mwm*org.gnome.Terminal*activeBackground`.

The file is read at startup, on `SIGHUP` and on "Restart..." in the root menu. A
Wayland compositor can't restart and keep its windows, so "Restart..." only
applies the file to what is running.

The environment overrides the global entries:

| Variable          | Default        |                                  |
|-------------------|----------------|----------------------------------|
| `MWC_ACTIVE`      | `#7387ad`      | focused frame colour             |
| `MWC_INACTIVE`    | `#aeb2c3`      | other frames, menus, icons       |
| `MWC_ROOT`        | `#5b7792`      | desktop colour, `none`: not drawn |
| `MWC_WALLPAPER`   | none           | PNG on the desktop, see below    |
| `MWC_FONT`        | `Sans Bold 10` | Pango font for titles and menus  |
| `MWC_MOVE_OPAQUE` | `0`            | `1`: opaque move/resize          |
| `MWC_TILING`      | `0`            | `1`: start in tiling mode        |
| `MWC_TILE_MAIN_WIDTH` | `55`       | main tiled window's share in %   |
| `MWC_TERMINAL`    | first of xterm, foot, alacritty, … | "New Window"  |

The cursor theme and size come from `XCURSOR_THEME` and `XCURSOR_SIZE`, the keyboard
layout from `XKB_DEFAULT_LAYOUT` and the like. `-d` logs debug output.

The wallpaper is the path of a PNG file; a leading `~/` is expanded. The image is
scaled to cover each monitor: the aspect ratio stays and what sticks out is cut
off evenly. If it can't be read, mwc logs why and shows the desktop colour.

## Panels and wallpaper tools

Programs using `wlr-layer-shell` work: panels, docks, launchers and wallpaper
tools. Space a panel reserves stays free of maximized and new windows, icons and
menus, and maximized windows and icons follow when panels come and go.

The top layer stays above windows, but a fullscreen window covers it while it has
the focus. The overlay layer goes above everything. A click on the background
layer counts as a click on the desktop, so a wallpaper tool doesn't cost the root
menu. While a surface holds the keyboard exclusively, the window key bindings are
off, except VT switching and Ctrl+Alt+Backspace.

X11 panels work as well: a window of type dock with `_NET_WM_STRUT_PARTIAL`
reserves space the same way, and `_NET_WORKAREA` tells X11 programs what is free.

## Moving and resizing

As in Motif mwm (`moveOpaque: False`), a window being moved or resized is shown as
an outline that follows the pointer; the window itself moves and resizes once, when
you let go. A box in the middle of the screen shows the position, or the size in the
program's own units, such as 80 x 24 for a terminal. For opaque dragging put
`Mwm*moveOpaque: true` in mwmrc or set `MWC_MOVE_OPAQUE=1`.

During a move or resize, also a drag, the arrow keys nudge the window (Shift for
1 px steps), Return confirms and Escape cancels, which also maximizes again a window
that was dragged out of its maximized state. Alt+F7 and Alt+F8 start one from the
keyboard.

## Snapping (as in Windows 11)

Drag a window by its title (or with Alt+btn1) until the pointer touches an edge of
the monitor. An outline in the window's colour shows where it will go, and it snaps
there when you let go:

| Pointer at …            | Window fills …                     |
|-------------------------|------------------------------------|
| left / right edge       | left / right half                  |
| a corner (64 px zone)   | that quarter                       |
| top edge                | the whole usable area (maximized)  |

Dragging a snapped or maximized window away gives it back its previous size, still
under the pointer, and Escape during the drag puts it back. Panels stay uncovered.

Keyboard: Super+Left/Right snap to a half (pressing the other direction restores
the window). Super+Up goes from a half to its top quarter, or maximizes. Super+Down
goes from a half to its bottom quarter, from a maximized window back to normal, and
from a normal window to its icon.

## Tiling

Super+T, or "Tile Windows" in the root menu, switches to tiling: the main window
takes the right part of the screen (55 %, or `Mwm*tileMainWidth`; all of it while it
is the only one), the other windows share the left part, one above the other. Gaps
around and between them keep the desktop and its root menu within reach. A new
window, or one back from an icon, becomes the main window.

The decoration stays, but tiled windows cannot be moved or resized. The title bar
buttons do this instead:

| Button   | Main window (right)              | Stack window (left)            |
|----------|----------------------------------|--------------------------------|
| Minimize | goes into the stack; the top stack window takes its place (nothing while it is alone) | shades: rolls up to its title bar |
| Maximize | nothing                          | becomes the main window        |

A click on a shaded window, focusing it (Alt+Tab) or Restore (Alt+F5) unrolls it.
When the stack runs out of room, its lower windows are shaded, and when even that is
not enough the oldest become icons. The layout ends above the row of icons. Dialogs,
windows that cannot be resized, windows too large for their place, fullscreen and
minimized windows float as usual; a window that leaves fullscreen joins the layout.
The layout covers the first monitor.

`Mwm*DoNotTile` in mwmrc keeps the listed programs out of the layout: they float
above or between the tiled windows. `Mwm*TileOnly` is the reverse: only the listed
programs tile. Programs go by `WM_CLASS` (instance or class name) or `app_id`,
ignoring case, separated by commas. When both are set, the later line wins; an empty
value switches the filter off.

```
! Firefox floats (app_id "firefox")
Mwm*DoNotTile:   firefox

! or the other way round: only these tile
Mwm*TileOnly:    XTerm, foot
```

While tiling is on, the root menu offers "Raise Tiled Windows" when a floating window
lies on top, and "Lower Tiled Windows" otherwise: all tiled windows (with their
dialogs) go above or below all floating ones, each group keeping its own order, and
the focus follows to the group on top. Switching tiling off puts every window back
where it was. `Mwm*tiling: true` in mwmrc or `MWC_TILING=1` starts with tiling on,
and reloading the configuration keeps the current choice.

## Keyboard and monitors

mwc reads these from mwmrc as well, which also reaches a session started from GDM. In
such a session nothing else sets the layout, so without them you get us:

```
Mwm*keyboardLayout:         de
Mwm*keyboardVariant:        nodeadkeys
Mwm*keyboardOptions:        ctrl:nocaps
Mwm*DP-1*outputMode:        3440x1440@144
Mwm*DP-1*outputPosition:    0,0
Mwm*HDMI-A-1*outputScale:   1.5
Mwm*HDMI-A-1*outputTransform: 90
```

| Resource | Meaning |
|---|---|
| `keyboardLayout`, `keyboardVariant`, `keyboardOptions`, `keyboardModel` | as in XKB; `XKB_DEFAULT_*` in the environment wins. A layout that doesn't exist falls back to us, and the log says so |
| `Name*outputMode` | `WIDTHxHEIGHT` or `WIDTHxHEIGHT@HZ`; default the monitor's preferred mode |
| `Name*outputScale` | scale factor from 0.1 to 10. Frames and menus are drawn at 1x and look soft on a scaled monitor, and X11 programs are not scaled by themselves |
| `Name*outputPosition` | `X,Y` in the layout, up to ±1000000; default the next free place in the row |
| `Name*outputTransform` | `normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180`, `flipped-270` |

mwc logs each monitor's name and modes when it appears. All of this also applies on
"Restart..." and `SIGHUP`. Tools like `wlr-randr` and `kanshi` don't work: mwc doesn't
offer their protocol.

## Window behaviour

- **Dialogs** stay above the window they belong to and minimize with it, without
  an icon of their own. They open centred over it, without Minimize and Maximize.
- **Size limits** hold while resizing: minimum and maximum size for all windows,
  resize steps, base size and aspect ratio from `WM_NORMAL_HINTS` for X11 ones. A
  window with a fixed size gets a thin border and no Size or Maximize.
- **`_MOTIF_WM_HINTS`** decide which decorations and functions an X11 window gets.
  Functions it doesn't have are grey in its menu and their keys do nothing.
- **`_NET_WM_WINDOW_TYPE`**: dialog, utility/toolbar/menu, splash, dock, desktop,
  notification. Docks and notifications stay above normal windows, desktop windows
  below them, none of the three takes the focus. `_NET_WM_STATE_ABOVE` and `BELOW`
  work too, as do programs that ask to start maximized or fullscreen.
- **Placement** of X11 windows follows `USPosition`, `PPosition` and `win_gravity`.
  An X11 window that takes no input focus (`WM_HINTS`) only comes to the front.

Only `_NET_WM_STRUT_PARTIAL` counts for X11 panels, not the older `_NET_WM_STRUT`,
because that is what wlroots reads.

X11 programs find `_NET_FRAME_EXTENTS`, `_NET_WORKAREA` and `_MOTIF_WM_INFO`. Xwayland
starts on the first X11 connection, so a program that asks for those before its window
is mapped may miss them the very first time.

## Decorations

Frames are server-side. Clients that speak `xdg-decoration` (foot, Qt,
alacritty, kitty, mpv, …) or KDE's server-decoration protocol (GTK3) get a
Motif frame. GTK4/libadwaita apps (Nautilus, Ptyxis, GNOME Text Editor)
always draw their own title bar and therefore stay unframed.
