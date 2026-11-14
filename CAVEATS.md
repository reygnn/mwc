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

This holds for X11 programs too, since Xwayland uses the same protocols for pointer
grabs and warps.


## Shortcuts in VM and RDP viewers

mwc offers no keyboard-shortcuts-inhibit-unstable-v1. A VM or RDP viewer cannot take
over the window manager's keys: Alt+Tab, Alt+F4, Alt+F9 and the other bindings act on
mwc's windows, not inside the guest or the remote session. Keys mwc doesn't bind reach
the viewer as usual.
