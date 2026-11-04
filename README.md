# mwc — Motif-style Wayland compositor

A small wlroots 0.19 compositor that draws the classic Motif window manager
look server-side: 3D bevels, resize border with corner grooves, window menu
button, minimize/maximize buttons, desktop icons and mwm's menus and key
bindings. X11 applications (including real Motif apps) run through XWayland
and get the same frame.

## Build (Ubuntu 26.04)

    sudo apt install build-essential pkg-config libwlroots-0.19-dev \
        wayland-protocols libwayland-dev libxkbcommon-dev libpixman-1-dev \
        libcairo2-dev libpango1.0-dev xwayland
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
any client type on the user's behalf.

`make wallpaper-test` renders generated images and checks pixels in screenshots,
and `make config-test` does the same for `mwmrc`, including reloading it.
`make layer-test` checks panels with a small layer-shell client
(`tests/layerc.c`).

`make test` runs them all. None of this tests real input hardware, DRM/KMS or VT
switching: use a VM or a free TTY for those.

Inside the box, the host's X server isn't reachable: `/tmp/.X11-unix` is a
private tmpfs because XWayland won't start without it. X11 apps only run inside
mwc.

## Mouse

| Where              | Action                                              |
|--------------------|-----------------------------------------------------|
| Title bar          | drag: move · double-click: maximize · btn3: menu    |
| Menu button        | click: window menu · double-click: close            |
| Border / corners   | resize                                              |
| Icon               | double-click: restore · btn3: window menu           |
| Desktop            | btn1/btn3: root menu                                |
| Alt + btn1/2/3     | move / lower / resize anywhere in the window        |

## Keys (mwm defaults)

Alt+Tab / Alt+Shift+Tab cycle · Alt+Space or Shift+Esc window menu ·
Alt+F3 lower · Alt+F4 close · Alt+F5 restore · Alt+F7 move · Alt+F8 size ·
Alt+F9 minimize · Alt+F10 maximize · Alt+F11 fullscreen ·
Ctrl+Alt+Return / Super+Return terminal · Ctrl+Alt+Backspace quit.

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

Copying a file from mwm-x11 works, except that `fontList` takes Pango names
like `Sans Bold 10`: a fontconfig pattern like `Sans:bold:size=10` is logged and
ignored. Resources mwc doesn't know are ignored too.

A program with its own `activeBackground` keeps its colour when unfocused, in a
paler shade (mixed with the global `background`), and so does its desktop icon.
Set `Mwm*Name*background` to choose the unfocused colour yourself. Colours are
`#rrggbb`, names like `gold` or `firebrick`, or `rgb:rr/gg/bb`. Lines starting
with `!` are comments; a `!` after a value is part of the value, so comments go
on their own line. Later lines win.

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
| `MWC_TERMINAL`    | first of xterm, foot, alacritty, … | "New Window"  |

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

## Decorations

Frames are server-side. Clients that speak `xdg-decoration` (foot, Qt,
alacritty, kitty, mpv, …) or KDE's server-decoration protocol (GTK3) get a
Motif frame. GTK4/libadwaita apps (Nautilus, Ptyxis, GNOME Text Editor)
always draw their own title bar and therefore stay unframed.
