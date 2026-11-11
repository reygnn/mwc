/*
 * desktop.c - virtual desktops
 *
 * MWC_DESKTOPS of them, global: switching changes what every output shows. A
 * window belongs to one desktop (view.desktop); off_desktop marks those not on the
 * current one, and counts as one more way of being hidden (view_hidden()), so focus,
 * Alt+Tab, clicks and the layout pass them by as they do minimized windows.
 *
 * - Panels, desktop windows, notifications and splash screens are on all of them.
 * - A dialog goes where its window is, a tab group as a whole.
 * - Icons belong to their window's desktop and are only seen there; each desktop
 *   has its own icon slots.
 * - Tiling lays out the current desktop's windows; the others keep their place in
 *   the layout for when their desktop comes back.
 * - Hiding is the scene's alone: clients aren't told, an X11 window on another
 *   desktop isn't iconic, and a capture of the output sees the current desktop.
 *
 * Switching is for other programs to ask for, through ext-workspace-v1
 * (workspace.c); mwc binds no keys to it. A window moves through its window menu.
 */
#include <wlr/types/wlr_scene.h>
#include "mwc.h"

bool view_on_all_desktops(const struct view *view)
{
	return view->wtype == WT_DOCK || view->wtype == WT_DESKTOP ||
		view->wtype == WT_NOTIFICATION || view->wtype == WT_SPLASH;
}

/* After off_desktop changed: the window and its icon as its other states say. */
static void apply(struct view *view)
{
	if (view->off_desktop) {
		grab_give_way(view);
		if (view->activated)
			view_set_activated(view, false);
		wlr_scene_node_set_enabled(&view->tree->node, false);
		wlr_scene_node_set_enabled(&view->icon_node->node, false);
		return;
	}
	wlr_scene_node_set_enabled(&view->tree->node, !view_hidden(view));
	wlr_scene_node_set_enabled(&view->icon_node->node, view->minimized);
}

static bool off(const struct view *view)
{
	return view->mapped && !view_on_all_desktops(view) &&
		view->desktop != view->server->desktop;
}

void desktop_view_mapped(struct view *view)
{
	struct view *root = view_transient_root(view);
	view->desktop = root != view ? root->desktop : view->server->desktop;
	view->off_desktop = off(view);
}

/* Whatever had the focus may have gone: the topmost window that is left gets it. */
static void focus_after(struct server *server, bool lost)
{
	if (lost)
		view_focus_top(server);
	icons_arrange(server);
	tile_arrange(server);
	views_update_layers(server);
	input_pointer_changed(server);
	workspace_changed(server);
}

void desktop_switch(struct server *server, int n)
{
	if (n < 0 || n >= MWC_DESKTOPS || n == server->desktop)
		return;
	if (server->menu.open)
		menu_close(server);
	server->desktop = n;
	bool lost = true;
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		bool now = off(v);
		if (now != v->off_desktop) {
			v->off_desktop = now;
			apply(v);
		}
		if (v->activated)
			lost = false;
	}
	focus_after(server, lost);
}

void view_set_desktop(struct view *view, int n)
{
	struct server *server = view->server;
	if (n < 0 || n >= MWC_DESKTOPS || view_on_all_desktops(view))
		return;
	struct view *root = view_transient_root(view);
	bool lost = false;
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		struct view *r = view_transient_root(v);
		if (r != root && !(root->group && r->group == root->group))
			continue;
		v->desktop = n;
		bool now = off(v);
		if (now != v->off_desktop) {
			lost |= v->activated;
			v->off_desktop = now;
			apply(v);
		}
	}
	focus_after(server, lost);
}

void desktop_attention(struct view *view)
{
	workspace_urgent(view->server, view->desktop);
}
