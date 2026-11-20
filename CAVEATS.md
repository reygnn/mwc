# Caveats

Known limits of mwc that are left as they are, with what they mean in use.


## Several keyboards

Each keyboard keeps its own modifiers. Alt held on one keyboard and Tab pressed on
another is no Alt+Tab, and Shift on one doesn't make capitals on the other. This
includes the virtual keyboard of an RDP server such as wayrdp next to a local one.
Typing on one keyboard at a time is not affected.

Lifting it would take a `wlr_keyboard_group` for the physical keyboards.


## Pointer capture

mwc offers neither pointer-constraints-unstable-v1 nor relative-pointer-unstable-v1.
A program cannot lock the pointer in its window or get raw motion:

* In a VM or RDP viewer the pointer can leave the window while the guest still
  expects it, and the guest's own pointer may lag behind or drift.
* Games that turn the view with the mouse, and Blender's view rotation and sculpting,
  stop at the edge of the screen.


## Shortcuts in VM and RDP viewers

mwc offers no keyboard-shortcuts-inhibit-unstable-v1. A VM or RDP viewer cannot take
over the window manager's keys: Alt+Tab, Alt+F4, Alt+F9 and the other bindings act on
mwc's windows, not inside the guest or the remote session. Keys mwc doesn't bind reach
the viewer as usual.


## Protocols open to every client

As in most wlroots compositors, any client may use these, and mwc asks nobody:
wlr-screencopy and ext-image-copy-capture-v1 read the screen, ext-data-control-v1 and
wlr-data-control read and set the clipboard, wlr-layer-shell covers the screen, and
ext-workspace-v1 switches the desktop. wlr-output-power-management lets any client
switch every monitor off; input does not bring them back, only a client does (wlopm
or a screen saver such as displayOff). Virtual keyboard and pointer are the exception:
they exist only with `MWC_VIRTUAL_INPUT=1` or in the tests.


## Virtual input

With `MWC_VIRTUAL_INPUT=1` and wlroots before 0.19.3, a virtual pointer that scrolls on
both axes in one frame with an axis source other than the wheel (finger, continuous or
wheel tilt) ends the session where wlroots is built with assertions, as Ubuntu's is.
wlroots applies the source to one axis only, leaves the other at the wheel and aborts
on the mismatch. Any client of the protocol can send that; whether wayrdp does is
unchecked. wlroots 0.19.3 fixes it (commit 878aebf).


## Games out of sight

A window using wp_fifo_v1 (SDL 3, Mesa's Vulkan FIFO mode) that nobody sees gets about
one update a second. When it shows again, the update held at that moment can take up
to that second to come: mwc looks at whether it is seen when an update arrives, not
when the window comes back.


## Output management

wlr-output-management is off unless `Mwm*outputManagement: true`. wlroots up to 0.20
aborts when one client of the protocol has an object for a mode that another lacks,
which any client can bring about, most easily on headless and nested monitors, the
ones a VNC or RDP server uses. The fix is on wlroots' master (commit 40640950).

With it on, any client can change the monitors, and switch all of them off; then only
such a client brings them back.


## Scaled monitors

Frames, menus and icons are drawn at 1x. With `outputScale` above 1 they look soft.


## Programs that draw their own frame

GTK4 and libadwaita programs (Nautilus, Ptyxis, GNOME Text Editor) always draw their own
title bar and stay without a Motif frame.


## Programs mwc starts

`terminal`, `runDialog`, `screenSaver`, the screenshot commands and `rootMenuItem` run
through `/bin/sh -c` without mwc waiting for them. A command that fails, such as a
misspelt program name, shows nothing on the screen: the key or menu item does nothing,
and only the shell's message ("not found") is in mwc's log.


## Desktops and what clients see

Clients aren't told about desktops, and a capture of a monitor sees the current desktop
only.


## The first monitor

The tiling layout, the icon box and the dock use the first monitor only. In tiling
mode every window that tiles is laid out there, also one opened on another monitor;
the others keep only floating windows.


## Dock

The dock sits at the monitor's edge, also where a panel is: the two overlap. A docked
tool larger than the monitor sticks out of it.

Beside the dock only snapping, Super+Shift+arrows and placing use its strip. A
maximized window, the tiling layout and the icon box keep to the area without the whole
strip. Snapping uses it only beside a dock along the bottom or top: beside a column, a
quarter in a row clear of it would overlap the half next to it.


## Placement by geometry

`geometry` does not apply to a window that starts maximized or fullscreen, nor to a
client that sets its `app_id` only after its window has appeared.


## Many icons

Icons on the desktop take their places one after another, each checked against all the
others. With a few hundred icons, a change of the usable area (a panel coming or going)
takes noticeably long. The icon box is not affected.


## X11 programs

There is no Xwayland: programs that speak only X11 (xterm, Motif programs, Wine, older
Java) don't start. `DISPLAY` is unset for programs mwc starts, so that nested in an X
session they don't open there.


## Files from mwm-x11

`fontList` takes Pango names such as `Sans Bold 10`. Forms like `Sans:bold:size=10` or
`Sans-12` from an mwm-x11 file are logged and ignored.
