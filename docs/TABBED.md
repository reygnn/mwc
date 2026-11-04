# Tabbed windows: spec

Status: implemented on `feature/tabbed`. Kept as the record of how it is meant to work.

## Goal

Several windows share one frame and one place on the screen. Their titles sit side by
side in the title bar, as tabs, and one of them is shown at a time. It works for floating
and tiled windows alike, and it keeps the Motif look: tabs are bevels like the rest of
the frame.

    +---+----------+----------+----------+---+---+
    | - |  xterm   |  [ mfm ] | Firefox  | . | o |
    +---+----------+----------+----------+---+---+
    |                                            |
    |                the shown tab               |

Not goals for the first version: a tabbed layout mode that puts every new window into a
group, grouping by mwmrc rules, nested groups, saving groups across sessions, and tabs
across outputs. The model below doesn't stand in the way of the first two.

## The model

A **group** is an ordered list of two or more views, one of them **shown**. A window
alone has no group: there is no group of one.

The central rule: **the shown view is the window, and the others don't exist for the
rest of mwc.** Moving, resizing, maximize, snap, tiling, stacking and Alt+Tab all see only
the shown view, and no code outside `src/tabs.c` loops over members. A hidden member
stops being taken into account the same way a dialog hidden with its minimized parent
does, and comes back the same way.

    struct tab_group {
    	struct wl_list members;  /* view.tab_link, in tab order */
    	struct view *shown;
    	int first_visible;       /* see "Too many tabs" */
    };

    /* in struct view */
    struct tab_group *group;     /* NULL when alone */
    struct wl_list tab_link;
    bool tab_hidden;             /* in a group, and not the shown one */

### Hiding a member

`view_show(view, false)` does what is needed: the scene tree is disabled, and an X window
is marked minimized. That gives X programs `WM_STATE` Iconic and `_NET_WM_STATE_HIDDEN`,
which is true enough, and is what they already handle. `view->minimized` stays false, so
no icon appears.

xdg clients additionally get `wlr_xdg_toplevel_set_suspended(true)`. Without a frame
callback most stop drawing anyway, and this says why.

`hidden_with_parent` is checked in about 20 places today, nearly all of which need
`tab_hidden` too. Rather than doubling them, those that mean "not on screen right now" go
through one helper, `view_hidden(view)`, in a preparatory commit with no change in
behaviour.

### Switching

`tabs_show(view)`, the only way a member becomes shown, does this in order:

1. **State moves over.** The new view takes these from the old one: `x`, `y`, `saved`,
   `fit_px`, `fit_py`, `maximized`, `snap`, `tiled`, `tile`, `tile_main`, `tile_seq`,
   `shaded`, `auto_shaded`, `tile_back`.
2. **Place in the stack moves over.** Its `server.views` link and scene node go where the
   old view's were, or it would jump in the stacking order.
3. **The client is told.** Maximized and tiled states go to the new view's xdg toplevel,
   or to the X window together with its position.
4. **Visibility changes.** Old hidden and suspended, new shown and unsuspended.
5. **Focus follows,** if the old view had it. A grab or menu on the old view ends first.
6. **The frame is drawn** for the new view.

### Keeping sizes in step

Moving state only at a switch would make every switch after a resize visibly jump, since
the new client answers a configure one frame later. So sizes are sent ahead: when the
shown view's committed size changes, and no interactive resize is running, hidden members
are sent that size. This has one call site, in the commit handlers. During an interactive
resize they wait for the final commit.

A hidden client may not answer while it is suspended. Then the switch shows its old buffer
for a frame, which is acceptable.

Sizes can still differ, as `xterm` resizes in character steps. The frame follows the
shown client's real size, as tiling does today through `view_apply_tile()`, anchored at
the top left.

## Who can be in a group

A window can join if it is mapped, `WT_NORMAL`, without a parent, not fullscreen, not
minimized, and has a frame with a title bar (`frame_title(view) > 0`). Not allowed:

- **Fixed-size windows** (`view_fixed()`): they can't take the group's size.
- **A minimum size larger than the output's usable area.** A smaller minimum than the
  group needs is fine, and a larger one grows the group.
- **A different `no_tile`** from the group's: tiling could not treat the group as one
  window otherwise. After a reload changes that, the shown view decides.

A window that stops qualifying, such as one asking for client-side decorations, leaves
the group by itself.

**Limit:** clients drawing their own title bar have no frame, so they cannot be tabs. That
may cover Firefox unless its own title bar is switched off. Not yet checked on the real
program, and worth doing before the implementation starts.

## Using it

### Keyboard

| Key | Action |
|---|---|
| Super+G | Join the window that had the focus before this one: both become one group |
| Super+Shift+G | Take the shown tab out, as a window of its own |
| Super+Tab | Next tab |
| Super+Shift+Tab | Previous tab |
| Super+1 to Super+9 | The n-th tab, where there is one |

Alt+Tab keeps cycling windows, so a group counts once. It is the shown view that takes
the focus. Bindings use the same keysym lookup as the rest, so Caps Lock doesn't matter.

### Mouse

| Gesture | Action |
|---|---|
| Left click on a tab | Show it, and focus the group |
| Middle click on a title or tab | Lowers the window or group, as on a title bar today |
| Wheel over the tabs | Next or previous tab |
| Left drag on a tab | Moves the whole group, as dragging a title bar does today |
| Middle drag on a title or tab | Takes that one window along: see below |
| Ctrl+left drag on a title or tab | The same, for touchpads without a middle button |
| Double click on a tab | Toggles maximize, as on a title bar |

A tab switches on release, once the press hasn't turned into a drag past
`DRAG_THRESHOLD`, so that dragging a group doesn't change its tab.

The middle button lowers on press today. It has to wait for the release now, once the
press hasn't turned into a drag, for plain titles too.

**Taking a window along** follows PWM, which bound this to the middle button: dropped on
any part of another window, it joins that window's group, and dropped on the desktop it
goes there, alone. That makes the target large, not just a title bar. It shows the
existing outline at the window's size, without the snap zones, and the window under the
pointer is highlighted through the snap preview. Escape cancels. It is a new `grab_tab` flag on the existing `CURSOR_MOVE` grab, not a new
cursor mode. Dragging applies to floating windows; in the layout, where moving does
nothing today, it is keyboard and menu only.

### Window menu

"Tab With Previous Window", "Next Tab", "Previous Tab" and "Detach Tab", each insensitive
where it can't apply. Mnemonics follow the existing entries.

## Drawing

The title bar row stays as it is: same height, same buttons. Therefore
`frame_extra_w/h` don't change, and **forming a group doesn't resize anything.**

With a group, the title's area in `struct title_layout` is divided evenly among the tabs,
each a bevel:

- The **shown** tab is sunken, in the view's current frame colours.
- **Other tabs** are raised, in that member's own unfocused colours. So a blood-red xterm
  stays recognisable next to a brown Firefox, in a group of any colour.
- Text goes through `draw_text()`, ellipsized.

A new `PART_TAB` in `enum frame_part`, with `frame_hit()` also reporting which tab.

### Too many tabs

Each tab has a minimum width of three title heights. When they don't all fit, as many as
fit are drawn from `first_visible`, which moves so the shown tab stays in view, and small
arrows at either end show there are more. Wheel and keys still reach all of them.

A title change on a hidden member redraws the group's frame.

## Interplay

| Situation | Behaviour |
|---|---|
| **Maximize, snap** | On the shown view; the state moves over at a switch. |
| **Tiling** | The group is one cell. `tileable()` skips hidden members. Joining closes the gap, detaching makes a new main window, as any new window does. |
| **Shaded in the stack** | The tabs stay visible in the rolled-up title. A click unrolls and shows that tab. |
| **Minimize** | One icon for the group, showing the shown view. Restoring brings it back as it was. |
| **Fullscreen** | No tabs while fullscreen, and Super+Tab does nothing, as a window that fills the screen has no frame to show them. Alt+Tab still leaves it. |
| **Hidden member asks for fullscreen** | Refused until it is shown. |
| **Closing the shown tab** | Alt+F4 closes that one window only. The tab to its right takes over, or else the one to its left. |
| **A member unmaps or dies** | Shown: as above, with the state moved over first, in `view_handle_unmap()`. Hidden: it just leaves. If one is left, the group dissolves and its frame is redrawn as an ordinary title. |
| **Dialog of a hidden member** | Its tab is shown, and the dialog takes the focus: a new window takes the focus today, and a prompt must not stay out of sight. While the group is an icon, it stays hidden and comes back with it, as today. |
| **Focus requests** | `view_focus()` on a hidden member shows it first, in the same way as it restores a minimized window. That covers `_NET_ACTIVE_WINDOW`, `focus_in` and X `WM_CHANGE_STATE` alike. |
| **Output unplugged** | Only the shown view is rescued; the others take its place at the next switch. |
| **X stacking** | A hidden X window stays somewhere in the X stack. Input follows the scene, so it can't catch clicks. Tests must show this. |
| **Menus and grabs on a member** | End when it stops being shown. |
| **Reload** | Colours and frames of all members are renewed, and groups stay. |

## Configuration

Nothing new in `mwmrc` for the first version, since per-program colours already work per
member. A minimum tab width setting can wait until someone needs one.

## Tests

`tests/tabs.sh` with `make tabs-test`, using both `xdgc` and `xclient`. The state dump
gets `group=`, `tab=`, `tab_hidden=` per view, so tests don't need screenshots except for
drawing. As for all recent work, each fix or feature gets a mutation probe, and everything
runs under ASan/UBSan.

- **Forming:** Super+G, a group of three, no resize, no group of one, what cannot join.
- **Switching:** keys, click, wheel. Position, size and stacking stay, only the shown view
  receives pointer and keyboard, and hidden X windows catch no clicks.
- **Sizes:** resize the group, then switch: the new tab already has the size.
- **Leaving:** close the shown tab, close a hidden one, kill a client, null buffer, X
  withdraw, Super+Shift+G, down to dissolving.
- **Dragging:** moves the group, no switch; middle and Ctrl+left drag join, tear out, Escape.
  A middle click still lowers.
- **Interplay:** each row of the table above, including tiling and shading.
- **Drawing:** pixels for sunken and raised tabs, a member's colours, too many tabs.
- **Crash cases in `tests/crash.sh`:** a client dying during a switch or grab, a dialog of
  a dying member, a reload with a menu open.

Also to try by hand, since the tests have no such clients: Firefox, Qt, GTK with a server
side frame, and how Java takes being iconified while hidden.

## Implementation order

Each step is one commit, tested, on `feature/tabbed`.

1. `view_hidden()` replaces the existing checks, with no change in behaviour.
2. `src/tabs.c`: the group, joining, leaving, `tabs_show()`, and the keys. A group works
   but still looks like one window. The state dump and the first tests.
3. Sizes kept in step, and suspended.
4. Drawing, `PART_TAB`, click and wheel.
5. Dragging: group move without a switch, taking one window along.
6. The window menu, the rest of the interplay table, and the README.

## Risks

- **Moving state at a switch** is the heart of this, and has the most ways to go wrong.
  Any new field in `struct view` that belongs to the window's place has to be added to
  its list. It should be one function, with a comment saying so in `struct view`.
- **Hidden X programs treated as iconified** may stop updating or act oddly, Java above
  all. If so, the alternative is to leave them looking mapped and only hide them in the
  scene, which is what the frame design allows for without other changes.
- **Suspended clients** that don't answer configures until shown make the first switch
  after a resize lag. This is cosmetic.
