/*
 * desktop.c - virtual desktops
 *
 * MWC_DESKTOPS of them, global: switching changes what every output shows. A
 * window belongs to one desktop (view.desktop); off_desktop marks those not on the
 * current one, and counts as one more way of being hidden (view_hidden()), so focus,
 * Alt+Tab, clicks and the layout pass them by as they do minimized windows.
 *
 * - Docked windows are on all of them, as are programs with Mwm*NAME*allDesktops,
 *   windows put there through "On All Desktops" in their window menu, and their
 *   dialogs. A tab group is on all of them if one of its windows is.
 * - A dialog goes where its window is, a tab group as a whole.
 * - Icons belong to their window's desktop and are only seen there; each desktop
 *   has its own icon slots.
 * - Tiling lays out the current desktop's windows; the others keep their place in
 *   the layout for when their desktop comes back.
 * - Hiding is the scene's alone: clients aren't told, and a capture of the output
 *   sees the current desktop.
 * - A window there that would take the focus, a dialog opening, doesn't switch: it
 *   is marked (view.attention), and its desktop is urgent while one there is.
 *
 * Switching is for other programs to ask for, through ext-workspace-v1
 * (workspace.c); mwc binds no keys to it. A window moves through its window menu.
 */
#include <wlr/types/wlr_scene.h>
#include "mwc.h"

static bool by_rule(const struct view *view)
{
	return view->all_desktops || view_docked(view);
}

static bool everywhere(const struct view *view)
{
	return view->sticky || by_rule(view);
}

/* A tab group by a rule of any of its windows, as "On All Desktops" makes it one as a
 * whole: it is on one desktop or on all of them. */
static bool group_by_rule(struct view *root)
{
	if (!root->group)
		return by_rule(root);
	struct view *m;
	wl_list_for_each(m, &root->group->members, tab_link) {
		if (by_rule(m))
			return true;
	}
	return false;
}

bool view_on_all_desktops(struct view *view)
{
	struct view *root = view_transient_root(view);
	return everywhere(view) || root->sticky || group_by_rule(root);
}

bool view_sticky_by_rule(struct view *view)
{
	return by_rule(view) || group_by_rule(view_transient_root(view));
}

/* After off_desktop changed: the window and its icon as its other states say. */
static void apply(struct view *view)
{
	if (view->off_desktop) {
		struct server *server = view->server;
		grab_give_way(view);
		/* A frame button held down on it would act on it hidden. */
		if (server->grabbed_view == view)
			grab_forget(server);
		if (view->activated)
			view_set_activated(view, false);
		wlr_scene_node_set_enabled(&view->tree->node, false);
		wlr_scene_node_set_enabled(&view->icon_node->node, false);
		return;
	}
	wlr_scene_node_set_enabled(&view->tree->node, !view_hidden(view));
	wlr_scene_node_set_enabled(&view->icon_node->node, view->minimized);
}

static bool off(struct view *view)
{
	return view->mapped && !view_on_all_desktops(view) &&
		view->desktop != view->server->desktop;
}

/* Where it belongs: with its window, or here if it is everywhere or on its own. */
static int desktop_for(struct view *view)
{
	struct view *root = view_transient_root(view);
	if (root == view || view_on_all_desktops(view))
		return view->server->desktop;
	return root->desktop;
}

void desktop_view_mapped(struct view *view)
{
	view->desktop = desktop_for(view);
	view->attention = false;
	view->off_desktop = off(view);
}

/* Whatever had the focus may have gone: the topmost window that is left gets it. */
static void focus_after(struct server *server, bool lost, bool asked)
{
	if (lost && asked)
		view_focus_top_instead(server); /* a client's doing: a launcher keeps the keys */
	else if (lost)
		view_focus_top(server);
	icons_arrange(server);
	tile_arrange(server);
	views_update_layers(server);
	input_pointer_changed(server);
	workspace_update(server);
}

void desktop_switch(struct server *server, int n)
{
	if (n < 0 || n >= MWC_DESKTOPS || n == server->desktop)
		return;
	if (server->menu.open)
		menu_close(server);
	/* While the box still shows the desktop it leaves. */
	iconbox_desktop_changed();
	server->desktop = n;
	bool lost = true;
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		bool now = off(v);
		if (now != v->off_desktop) {
			v->off_desktop = now;
			apply(v);
		}
		if (!now)
			v->attention = false; /* it is in sight now */
		if (v->activated)
			lost = false;
	}
	focus_after(server, lost, false);
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
		if (!now)
			v->attention = false;
	}
	focus_after(server, lost, false);
}

/* One of desktop_recheck()'s family; true if it went out of sight or into it. */
static bool recheck_one(struct view *v, bool *lost)
{
	if (!v->mapped)
		return false;
	/* A window in sight stays in sight: one that is no longer everywhere stays
	 * on the current desktop. A dialog goes with its window. */
	if (!v->off_desktop || view_transient_root(v) != v)
		v->desktop = desktop_for(v);
	bool now = off(v);
	if (now == v->off_desktop)
		return false;
	*lost |= now && v->activated;
	v->off_desktop = now;
	apply(v);
	if (!now)
		v->attention = false;
	return true;
}

/* The window, its tab group and their dialogs. */
static bool in_family(struct view *v, struct view *root)
{
	struct view *r = view_transient_root(v);
	return r == root || (root->group && r->group == root->group);
}

void desktop_recheck(struct view *view)
{
	/* Dialogs depend on whether their window is everywhere: the windows first, which
	 * they follow. */
	struct view *root = view_transient_root(view);
	bool lost = false, changed = false;
	struct view *v;
	wl_list_for_each(v, &view->server->views, link) {
		if (view_transient_root(v) == v && in_family(v, root))
			changed |= recheck_one(v, &lost);
	}
	wl_list_for_each(v, &view->server->views, link) {
		if (view_transient_root(v) != v && in_family(v, root))
			changed |= recheck_one(v, &lost);
	}
	if (changed)
		focus_after(view->server, lost, true);
}

/* Out of sight or into it, as each window's desktop now says; the arranging after it
 * only if one did, unless always. */
static void recheck_all(struct server *server, bool always)
{
	struct view *v;
	bool lost = false, changed = false;
	wl_list_for_each(v, &server->views, link) {
		bool now = off(v);
		if (now != v->off_desktop) {
			changed = true;
			lost |= now && v->activated;
			v->off_desktop = now;
			apply(v);
		}
		if (!now)
			v->attention = false;
	}
	if (changed || always)
		focus_after(server, lost, false);
}

void desktop_config_changed(struct server *server)
{
	struct view *v;
	/* Windows in sight stay in sight: one that is no longer everywhere stays here. */
	wl_list_for_each(v, &server->views, link) {
		if (v->mapped && !v->off_desktop)
			v->desktop = desktop_for(v);
	}
	recheck_all(server, true);
}

void view_toggle_sticky(struct view *view)
{
	struct server *server = view->server;
	struct view *root = view_transient_root(view);
	if (view_sticky_by_rule(root))
		return;
	/* On the current desktop it stays when it stops; a client may ask from another
	 * desktop, and comes into sight. */
	bool on = !root->sticky;
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		struct view *r = view_transient_root(v);
		if (r != root && !(root->group && r->group == root->group))
			continue;
		if (r == v)
			v->sticky = on;
		v->desktop = server->desktop;
	}
	recheck_all(server, false);
}

void desktop_attention(struct view *view)
{
	if (view->attention || !view->off_desktop)
		return;
	view->attention = true;
	workspace_update(view->server);
}

bool desktop_urgent(struct server *server, int n)
{
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->mapped && v->attention && v->off_desktop && v->desktop == n)
			return true;
	}
	return false;
}
