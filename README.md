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

## Configuration (environment)

| Variable          | Default        |                                  |
|-------------------|----------------|----------------------------------|
| `MWC_ACTIVE`      | `#7387ad`      | focused frame colour             |
| `MWC_INACTIVE`    | `#aeb2c3`      | other frames, menus, icons       |
| `MWC_ROOT`        | `#5b7792`      | desktop colour                   |
| `MWC_FONT`        | `Sans Bold 10` | Pango font for titles and menus  |
| `MWC_TERMINAL`    | first of xterm, foot, alacritty, … | "New Window"  |

## Decorations

Frames are server-side. Clients that speak `xdg-decoration` (foot, Qt,
alacritty, kitty, mpv, …) or KDE's server-decoration protocol (GTK3) get a
Motif frame. GTK4/libadwaita apps (Nautilus, Ptyxis, GNOME Text Editor)
always draw their own title bar and therefore stay unframed.
