# mwc

<img src="data/logo.png" alt="mwc logo" width="128" align="right">

mwc is a small Wayland compositor with the look and behaviour of the Motif window
manager. Frames are drawn server side: bevelled borders with corner grooves, a window
menu button, minimize and maximize, desktop icons, and mwm's menus and key bindings.
It runs Wayland programs only: there is no Xwayland, so X11 programs don't start. It is
built on wlroots 0.19.


## Build

On Ubuntu 26.04:

    sudo apt install build-essential pkg-config libwlroots-0.19-dev \
        wayland-protocols libwayland-dev libxkbcommon-dev libpixman-1-dev \
        libcairo2-dev libpango1.0-dev
    make
    sudo make install

`make install` puts `mwc` in `/usr/local/bin` and adds a GDM session entry.


## Run

Nested in a running session, as a window:

    ./mwc -s foot

As a session: choose "Mwc (Motif)" in GDM, or start `mwc` from a TTY.
`~/.config/mwc/autostart`, if executable, runs at startup unless `-s` is given.

`-d` logs debug output. `-v` prints the version, which is set in `src/version.h`, and the
commit built, as `0.5.0+DATE.TIME.HASH` with its commit time in UTC and `.dirty` for
changes not committed.

To serve mwc headless over RDP with [wayrdp](https://github.com/reygnn/wayrdp), see
[docs/WAYLAND-RDP.md](docs/WAYLAND-RDP.md);
[docs/wayland-rdp-upgrade.sh](docs/wayland-rdp-upgrade.sh) moves a server from mwm-x11
with xrdp.


## Mouse

| Where | Action |
|---|---|
| Title bar | drag: move; double click: maximize; button 2: lower; button 3: window menu |
| Menu button | click: window menu; double click: close |
| Border, corners | resize |
| Icon | double click: restore; button 3: window menu |
| Icon box | wheel: scroll; double click on the title: maximize; click on the logo: show the box |
| Desktop | buttons 1 and 3: root menu; button 2: run dialog |
| Alt + button 1, 2, 3 | move, lower, resize anywhere in the window |


## Keys

The mwm defaults:

| Keys | Action |
|---|---|
| Alt+Tab, Alt+Shift+Tab, Alt+Esc | cycle windows |
| Alt+Space, Shift+Esc | window menu |
| Alt+F3, F4, F5 | lower, close, restore |
| Alt+F7, F8 | move, size |
| Alt+F9, F10, F11 | minimize, maximize, fullscreen |
| Ctrl+Alt+Return, Super+Return | terminal |
| Ctrl+Alt+Backspace | quit |
| Ctrl+Alt+F1 to F12 | switch VT |

Added by mwc:

| Keys | Action |
|---|---|
| Super+arrows | snap, see Snapping |
| Super+Shift+arrows | push a floating window against that edge; twice in a row: a corner |
| Super+T | tiling on or off |
| Super+G, Super+Tab, Super+1 to 9 | tabs, see Tabs |
| Super+Esc | screen saver |
| Print, Shift+Print | a rectangle or the screen to the clipboard; with Ctrl to a file |

During a move or resize, the arrows nudge the window by 10 px, with Shift by 1 px.
Return confirms, Escape cancels. Held arrows repeat in menus, during a move or resize,
and in the icon box.


## Configuration

`~/.config/mwc/mwmrc` uses mwm's resource syntax, so a file from mwm-x11 can be copied.
It is read at startup, on SIGHUP and on "Reload" in the root menu. A Wayland compositor
cannot restart and keep its windows, so a reload applies the file to what is running.

    ! global
    Mwm*activeBackground:        #7387ad
    Mwm*background:              #aeb2c3
    Mwm*fontList:                Sans Bold 10
    Mwm*wallpaper:               ~/pictures/desk.png

    ! per program, by app_id
    Mwm*firefox*activeBackground:  #8a0303
    Mwm*foot*activeBackground:     gold

Lines starting with `!` are comments; a `!` after a value belongs to the value. Later
lines win. Colours are `#rrggbb`, names such as `gold`, or `rgb:rr/gg/bb`. Programs are
matched by their `app_id`, ignoring case; it may contain dots. Unknown resources are
logged and ignored.

Global resources:

| Resource | Meaning |
|---|---|
| `activeBackground` | frame colour while focused |
| `background` | frame colour while unfocused; also menus and icons |
| `activeForeground`, `foreground` | title text; default black or white, whichever contrasts |
| `fontList` | font of titles and menus; title bars grow with it |
| `rootBackground` | desktop colour, or `none` for a wallpaper tool such as swaybg |
| `wallpaper` | PNG on the desktop, see below |
| `moveOpaque` | `true`: move and resize the window itself instead of an outline |
| `terminal` | for "New Window" and Super+Return; default the first of foot, alacritty and others found |
| `rootMenuItem` | a program in the root menu, see Root menu |
| `runDialog` | button 2 on the desktop; default `mrun` |
| `screenSaver` | Super+Esc; default `displayOff --stars` |
| `clipboardRectangleScreenshot`, `clipboardScreenshot`, `rectangleScreenshot`, `screenshot` | the Print keys, see Screenshots |
| `tiling` | `true`: start in tiling mode |
| `tileMainWidth` | the main tiled window's share of the width, 10 to 90 %, default 55 |
| `DoNotTile`, `TileOnly` | programs that never tile, or the only ones that do |
| `iconPlacement` | where icons go, see Icons |
| `useIconBox`, `iconBoxSize` | the icon box, see Icons |
| `dockPlacement` | where the dock goes, see Dock |
| `keyboardLayout`, `keyboardVariant`, `keyboardOptions`, `keyboardModel` | see Keyboard and monitors |
| `outputManagement` | `true`: offer wlr-output-management, see Keyboard and monitors |

Per program:

| Resource | Meaning |
|---|---|
| `activeBackground`, `background` | its frame colours; unfocused it defaults to a paler shade of its own |
| `activeForeground`, `foreground` | its title text |
| `geometry` | where its windows appear, see below |
| `allDesktops` | `true`: its windows are on every desktop |
| `dock` | `true`: its windows go into the dock, see Dock |

Per monitor, by name: `outputMode`, `outputScale`, `outputPosition`,
`outputTransform`, see Keyboard and monitors.

The environment overrides the global entries:

| Variable | Default | |
|---|---|---|
| `MWC_ACTIVE` | `#7387ad` | focused frame colour |
| `MWC_INACTIVE` | `#aeb2c3` | other frames, menus, icons |
| `MWC_ROOT` | `#5b7792` | desktop colour, `none`: not drawn |
| `MWC_WALLPAPER` | none | PNG on the desktop |
| `MWC_FONT` | `Sans Bold 10` | font of titles and menus |
| `MWC_MOVE_OPAQUE` | `0` | `1`: opaque move and resize |
| `MWC_TILING` | `0` | `1`: start in tiling mode |
| `MWC_TILE_MAIN_WIDTH` | `55` | main tiled window's share in % |
| `MWC_TERMINAL` | see `terminal` | the terminal |
| `MWC_VIRTUAL_INPUT` | `0` | `1`: virtual keyboard and pointer for clients such as wayrdp |

The cursor comes from `XCURSOR_THEME` and `XCURSOR_SIZE`, the keyboard layout also
from `XKB_DEFAULT_LAYOUT` and the like.

The wallpaper is the path of a PNG; a leading `~/` is expanded. It is scaled to cover
each monitor, keeping its aspect ratio and cutting off what sticks out evenly. If it
cannot be read, the log says why and the desktop keeps its colour.

### Placement

`geometry` puts a program's windows in a fixed place when they appear. The value is a
position as in X11's `-geometry`, without a size: `+` counts from the left or top edge
of the usable area, `-` from the right or bottom one, to the frame. The monitor is the
one under the pointer. `pointer` puts the middle of the title bar under the pointer
instead, as far as the monitor allows.

    Mwm*pulseControl*geometry:  -0-0
    Mwm*foot*geometry:          +20-40
    Mwm*mrun*geometry:          pointer

The entry wins over a position the program asks for, but not for dialogs. In tiling
mode the layout decides, unless the program is in `DoNotTile`. A window placed from the
right or bottom edge keeps that edge when it changes its size by itself, until it is
moved or resized by hand. Hidden and shown again, a window returns to its entry's
place.


## Moving and resizing

As in mwm, a window being moved or resized is shown as an outline, and changes once
when the button is released. A box in the middle of the monitor shows the position or
the size. `moveOpaque` drags the window itself. Escape during a drag also restores a
window that was dragged out of its maximized state.


## Snapping

A window dragged until the pointer touches an edge of the monitor snaps there on
release. An outline in the window's colour shows where.

| Pointer at | Window fills |
|---|---|
| left or right edge | that half |
| a corner, within 64 px | that quarter |
| top edge | the usable area: maximized |

Halves and quarters keep 8 px from the edges and from each other, so the desktop stays
in reach. Dragged away, a window gets its earlier size back under the pointer.

Super+Left and Super+Right snap to a half; the other direction restores. Super+Up goes
from a half to its top quarter, or maximizes. Super+Down goes from a half to its bottom
quarter, from maximized to normal, and from normal to an icon.


## Tiling

Super+T, or "Tile Windows" in the root menu, switches tiling on. The main window takes
the right part of the screen, 55 % or `tileMainWidth`, all of it while alone;
the others stack on the left. A new window, or one back from an icon, becomes the main
window. Tiled windows keep their frame but cannot be moved or resized; the title bar
buttons do this instead:

| Button | Main window | Stack window |
|---|---|---|
| Minimize | goes into the stack; the top stack window takes its place | rolls up to its title bar |
| Maximize | nothing | becomes the main window |

A click on a rolled up window, focusing it or Alt+F5 unrolls it. When the stack runs
out of room its lower windows roll up, and then the oldest become icons. Dialogs,
windows of a fixed size or too large for their place, and fullscreen and minimized
windows float. The layout leaves the icons' strip free.

`DoNotTile` keeps the listed programs floating; `TileOnly` tiles only those. Names are
separated by commas; the later line wins, and an empty value switches the filter off.

    Mwm*DoNotTile:  firefox
    Mwm*TileOnly:   foot, firefox

While tiling is on, the root menu offers "Raise Tiled Windows" or "Lower Tiled
Windows", which put all tiled windows above or below all floating ones. Switching
tiling off puts every window back where it was.


## Tabs

Several windows can share one frame, with their titles as tabs in the title bar.

| Keys or mouse | Action |
|---|---|
| Super+G | join the window that had the focus before |
| Super+Shift+G | take the shown tab out |
| Super+Tab, Super+Shift+Tab | next or previous tab |
| Super+1 to Super+9 | the nth tab |
| click or wheel on a tab | show it |
| drag | move the group |
| button 2 drag, Ctrl+button 1 drag | take one window: dropped on another it joins it, on the desktop it stands alone |

A window taken out gets its own size back. A group counts as one window: one tiling
cell, one icon, one place in Alt+Tab. Windows of a fixed size, dialogs and programs that
draw their own title bar cannot be tabs. More in [docs/TABBED.md](docs/TABBED.md).


## Virtual desktops

There are four desktops, shared by all monitors. mwc binds no keys to them: a pager
switches through ext-workspace-v1, where they are workspaces 1 to 4 of one group. A
window opens on the current desktop, a dialog on its window's. "To Desktop" in the
window menu moves a window with its dialogs and tab group; "On All Desktops" keeps it
on every one. Docked programs and programs with `allDesktops` are on all desktops,
with their dialogs; a program with `allDesktops` in a tab group takes the whole group
there. A dialog that opens on another desktop marks that desktop urgent.


## Icons

`iconPlacement` works as in mwm: `primary secondary [tight]`. The first word is the
direction a row or column fills in, the second where the next one goes: `left`, `right`,
`top`, `bottom`. One must be horizontal, the other vertical. `tight` leaves no space
between icons. The default `left bottom` fills the bottom row from the left. `center` in
place of the first word puts the icons in the middle of the edge the second names.

    Mwm*iconPlacement:  top right
    Mwm*iconPlacement:  center right

`useIconBox` puts the icons of the current desktop into a box instead: one row or
column where `iconPlacement` would start them. It is as long as
its icons need, up to `iconBoxSize` icons (default 10, or what fits), and scrolls from
there. Empty, it shows the mwc logo on a tile. It cannot be moved or resized and has no
window menu. Maximize switches between full length and what the icons need; minimize
leaves the logo, a click on which brings the box back. A click into the box or Alt+Tab
gives it the keyboard: the arrows choose an icon, Return restores it. Each desktop has
its own box.

    Mwm*useIconBox:   true
    Mwm*iconBoxSize:  8


## Dock

`dock` puts the windows of small tools side by side in one frame at the edge of the
first monitor, in the order of their lines in the file. `dockPlacement` takes the
words of `iconPlacement` except `tight`; the default `right bottom` starts in the
bottom right corner and grows to the left. Tools of different heights line up along
the monitor's edge. The dock follows when a tool comes, goes or changes its size, and
its strip is kept free like a panel's, but only over the dock. A window beside the dock
may reach into the strip when it is snapped, pushed with Super+Shift+arrows or placed,
by `geometry` counted from the monitor's edge there. A half and its quarters decide
together, so they stay in line. A window the dock grows over moves out of the strip.
Docked windows keep their frames, are on every desktop and stay above other windows.
They cannot be moved, resized, minimized, maximized or made fullscreen, and neither
tile nor join tab groups; Alt+Tab passes them by, a click gives them the keyboard.
Their dialogs open outside.

    Mwm*pulseControl*dock:  true
    Mwm*deskControl*dock:   true
    Mwm*dockPlacement:      right bottom

A reload takes effect at once: a window that loses its entry stays where it is as a
normal window, and the others close up.


## Root menu

`rootMenuItem` adds a program to the top of the root menu. Every line counts, in the
order of the file, up to 20. The label ends at the first colon; the rest is a shell
command.

    Mwm*rootMenuItem:  Firefox: firefox
    Mwm*rootMenuItem:  Notes: foot vi ~/notes.txt


## Screenshots

| Keys | Resource | Default |
|---|---|---|
| Print | `clipboardRectangleScreenshot` | `screenShot -c -r` |
| Shift+Print | `clipboardScreenshot` | `screenShot -c` |
| Ctrl+Print | `rectangleScreenshot` | `screenShot -r` |
| Ctrl+Shift+Print | `screenshot` | `screenShot` |

[screenShot](https://github.com/reygnn/screenShot) saves files under `Screenshots` in
the pictures folder. Programs without a window, such as `wl-copy`, reach the clipboard
through ext-data-control-v1 or wlr-data-control.


## Panels and idle tools

Programs using wlr-layer-shell work: panels, docks, launchers, wallpaper tools. Space a
panel reserves stays free of maximized and new windows, icons and menus; a reservation
leaving less than 100 px is ignored. The top layer stays above windows, but a focused
fullscreen window covers it, unless a surface in it on the same output has the
keyboard. A click on the background layer counts as a click on the desktop. A surface
asking for the keyboard exclusively gets it, the one in the highest layer first; one
asking on demand gets it when it appears or is clicked. While a surface holds the
keyboard exclusively, the window bindings are off, except VT switching and
Ctrl+Alt+Backspace.

ext-idle-notify-v1, idle-inhibit-unstable-v1 and wlr-output-power-management-unstable-v1
are there for tools such as [displayOff](https://github.com/reygnn/displayOff). A
monitor that is off stays in the layout. A window inhibiting idle counts once it has
appeared, also while minimized.


## Keyboard and monitors

    Mwm*keyboardLayout:             de
    Mwm*keyboardVariant:            nodeadkeys
    Mwm*keyboardOptions:            ctrl:nocaps
    Mwm*DP-1*outputMode:            3440x1440@144
    Mwm*DP-1*outputPosition:        0,0
    Mwm*HDMI-A-1*outputScale:       1.5
    Mwm*HDMI-A-1*outputTransform:   90

| Resource | Meaning |
|---|---|
| `keyboardLayout`, `keyboardVariant`, `keyboardOptions`, `keyboardModel` | as in XKB; `XKB_DEFAULT_*` wins. Without them the layout is `us`, also in a GDM session |
| `keyboardRepeatRate`, `keyboardRepeatDelay` | a held key repeats this many times a second (0 to 1000, 0 is off) after this many ms (1 to 10000); default 25 and 600 |
| `Name*outputMode` | `WIDTHxHEIGHT` or `WIDTHxHEIGHT@HZ`; default the preferred mode |
| `Name*outputScale` | 0.1 to 10 |
| `Name*outputPosition` | `X,Y` in the layout; default the next free place in the row |
| `Name*outputTransform` | `normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180`, `flipped-270` |

The log names each monitor and its modes. All of this applies on reload too.

Tools such as wlr-randr, kanshi and wdisplays, and VNC or RDP servers that size a
headless monitor to the viewer's window, need wlr-output-management. It is offered with
`Mwm*outputManagement: true`, read at startup only, and off by default. With it on,
what a client set stays
over a reload until mwmrc says something new about that monitor, and a monitor a client
switched off leaves the layout and stays off until such a client switches it on.


## Window behaviour

Dialogs stay above their window, open centred over it and minimize with it. A window's
minimum and maximum size hold while resizing. A window of a fixed size gets a thin
border and no Size or Maximize; functions a window lacks are grey in its menu.

A fullscreen window smaller than the monitor sits in its middle, on black.

Programs that wait for the refresh through wp_fifo_v1, as SDL 3 and Mesa's Vulkan FIFO
mode do, get one update per refresh; when nobody sees them (minimized, on another
desktop, under opaque windows, on a monitor that is off), about one a second.

Clients that speak xdg-decoration (foot, Qt, alacritty, kitty, mpv) or KDE's server
decoration protocol (GTK3) get a Motif frame.


## How this is built

mwc is developed with Claude (Anthropic) as a coding assistant. I set the scope and the
architecture and review the changes; Claude writes most of the code and the tests. The
checks:

* `make test` runs headless tests; `make smoke` starts mwc and checks that a client
  connects, with a screenshot.
* `make input-test` drives real clients with a simulated mouse and keyboard and checks
  geometry and focus after every move, resize, maximize, minimize, menu and key binding.
* What is open, and which protocols any client can use, is written down rather than
  hidden: see TODO and CAVEATS.


## Testing

`distrobox.ini` defines a rootless box with all dependencies, so the host needs only
Podman and Distrobox:

    sudo apt install podman distrobox uidmap
    distrobox assemble create --replace --file distrobox.ini
    distrobox enter mwc-dev
    make test

`tests/Containerfile` does the same in a plain Podman container.

`make test` runs every script in `tests/`, each also on its own as `make NAME-test`
(`make smoke` for the first): smoke, input, wallpaper, config, layer, window, move,
snap, outputs, tile, crash, pointer, state, misc, session, tabs, fifo, rdp, desktop,
iconbox, dock, matrix and build. All but build run mwc on a headless output with small
clients in `tests/` that can ask for any hint, and check the state mwc writes and pixels
in screenshots; build checks the Makefile with dry runs and an install into a scratch
directory. matrix takes each starting point (floating, tabs, tiling, rolled up,
tiling with tabs, each also on all desktops) through each operation and checks after
every step that the state fits together (`tests/invariants.awk`). `make walk` does the
same operations in a random order, `WALK_STEPS` of them from `WALK_SEED`; `make check`
takes a short walk too. None of this tests real input hardware, DRM/KMS or VT switching; use a VM or
a free TTY for those.

`make check` runs the same tests against a build with AddressSanitizer and
UndefinedBehaviorSanitizer, `build/mwc-asan`, beside the normal one, where warnings
are errors. A finding aborts
mwc and fails that test; the report is in its log under `build/`. Leaks are not
checked.

`make analyze` runs GCC's static analyzer over the sources, with findings as errors:
NULL dereferences, use after free and leaks on a path.

`make lint` reports C lines longer than 100 columns, tabs counted to every eighth
column.

`make valgrind` runs the tests against mwc in valgrind's memcheck, for reads of
uninitialized memory and errors inside wlroots, Pango and cairo, which the sanitizers
don't see. It is many times slower and leaves out the tests that go by timing: layer,
move, fifo, rdp and state.

The state file and the virtual input devices exist only with `MWC_TEST_STATE` set (the
devices also with `MWC_VIRTUAL_INPUT=1`, for wayrdp), because those protocols let a
client type for the user.


## License

MIT, see [LICENSE](LICENSE).
