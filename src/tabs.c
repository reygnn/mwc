/*
 * Tab groups, see docs/TABBED.md. Several windows share one frame and one place on
 * the screen, and one of them is shown.
 *
 * The shown view is the window, the others don't exist for the rest of mwc: moving,
 * maximizing, tiling and Alt+Tab only ever see the shown one. What makes up a window's
 * place moves over to the next one when it is shown, see move_place().
 *
 * There is no group of one.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>
#include "mwc.h"

static bool tab_material(struct view *view)
{
	return view->wtype == WT_NORMAL && !view_fixed(view) && view->ssd && (view->decor & DECOR_TITLE);
}

bool tabs_can_join(struct view *view)
{
	return view->mapped && !view->minimized && !view->hidden_with_parent &&
		!view->fullscreen && tab_material(view);
}

static void tell_suspended(struct view *view, bool suspended)
{
	if (view->type == VIEW_XDG && view->xdg_toplevel->base->initialized)
		wlr_xdg_toplevel_set_suspended(view->xdg_toplevel, suspended);
}

/*
 * Everything that makes up the window's place on the screen. A new field of struct view
 * that belongs to the place rather than the client has to be added here.
 */
static void move_place(struct view *to, struct view *from)
{
	to->x = from->x;
	to->y = from->y;
	to->saved = from->saved;
	to->fit_px = from->fit_px;
	to->fit_py = from->fit_py;
	to->maximized = from->maximized;
	to->snap = from->snap;
	to->tiled = from->tiled;
	to->tile = from->tile;
	to->tile_main = from->tile_main;
	to->tile_seq = from->tile_seq;
	to->tile_back = from->tile_back;
	to->shaded = from->shaded;
	to->auto_shaded = from->auto_shaded;

	from->maximized = false;
	from->snap = SNAP_NONE;
	from->tiled = false;
	from->tile_main = false;
	from->tile_back = false;
	from->shaded = false;
	from->auto_shaded = false;
}

/* Its dialogs go and come with it, as with an icon. */
static void dialogs_show(struct view *view, bool show)
{
	struct view *v;
	wl_list_for_each(v, &view->server->views, link) {
		/* Going: those on the screen. Coming: those that went. */
		if (!v->mapped || !view_is_transient_of(v, view) ||
				v->hidden_with_parent != show)
			continue;
		v->hidden_with_parent = !show;
		view_show(v, show);
		if (!show && v->activated)
			view_set_activated(v, false);
	}
}

/* The size last asked for: a new one may not have been committed yet. */
static void asked_size(struct view *view, int *w, int *h)
{
	*w = view->type == VIEW_XDG ? view->requested_w : view->xsurface->width;
	*h = view->type == VIEW_XDG ? view->requested_h : view->xsurface->height;
}

/*
 * A view that is on its way out, gone, can't be told anything any more, but its place
 * still counts.
 */
static void switch_to(struct tab_group *g, struct view *to, struct view *from, bool gone)
{
	struct server *server = to->server;
	/* The focus stays with the group, also if a dialog of the old one had it. */
	bool focus = from->activated;
	struct view *d;
	wl_list_for_each(d, &server->views, link) {
		if (d->activated && view_is_transient_of(d, from))
			focus = true;
	}
	g->busy = true;
	if (!gone) {
		grab_give_way(from);
		if (server->menu.open && server->menu.target == from)
			menu_close(server);
	} else {
		from->activated = false;
	}

	int w, h;
	asked_size(from, &w, &h);
	move_place(to, from);
	from->tab_hidden = true;
	to->tab_hidden = false;
	g->shown = to;

	/* It goes where the other one was, not to the back or the front. */
	wl_list_remove(&to->link);
	wl_list_insert(&from->link, &to->link);
	wlr_scene_node_reparent(&to->tree->node, from->tree->node.parent);
	wlr_scene_node_place_above(&to->tree->node, &from->tree->node);

	if (!gone) {
		dialogs_show(from, false);
		view_show(from, false);
		tell_suspended(from, true);
		view_publish_state(from);
		view_update_properties(from);
	}
	tell_suspended(to, false);
	view_show(to, true);
	view_show_content(to);
	dialogs_show(to, true);
	view_update_properties(to);
	view_publish_state(to);

	if (to->tiled) {
		view_apply_tile(to);
	} else if (to->maximized || to->snap != SNAP_NONE) {
		view_refit(to);
	} else {
		view_apply_size_hints(to, &w, &h, false);
		view_set_geometry(to, to->x, to->y, w, h);
	}
	frame_render(to);
	g->busy = false;

	if (focus)
		view_focus(to);
	else
		views_update_layers(server);
	tile_arrange(server);
	input_pointer_changed(server);
}

/* The neighbour taking over from it: the one to the right, or else to the left. */
static struct view *neighbour(struct view *view)
{
	struct tab_group *g = view->group;
	struct wl_list *next = view->tab_link.next == &g->members ?
		view->tab_link.prev : view->tab_link.next;
	struct view *v = wl_container_of(next, v, tab_link);
	return v;
}

/* A group takes the size of the window it began with, and only a window that floats has
 * its real size at hand: otherwise it is what it goes back to. */
static void remember_own_size(struct view *view)
{
	if (view->maximized || view->snap != SNAP_NONE || view->tiled) {
		view->tab_own_w = view->saved.width;
		view->tab_own_h = view->saved.height;
	} else {
		asked_size(view, &view->tab_own_w, &view->tab_own_h);
	}
}

void tabs_own_size(struct view *view, int *w, int *h)
{
	*w = view->group ? view->tab_own_w : view->width;
	*h = view->group ? view->tab_own_h : view->height;
}

/* Out of a group, with no one to share with. In a state, it is what it goes back to. */
static void back_to_own_size(struct view *view)
{
	int w = view->tab_own_w, h = view->tab_own_h;
	if (view->maximized || view->snap != SNAP_NONE || view->tiled) {
		view->saved.width = w;
		view->saved.height = h;
		return;
	}
	/* A size of 0 means the client chooses, which X doesn't know. */
	if (!w && view->type != VIEW_XDG)
		return;
	if (w)
		view_apply_size_hints(view, &w, &h, false);
	view_send_size(view, w, h);
}

/* Out of the list, which ends the group when one is left. It has been hidden by now. */
static void remove_member(struct view *view)
{
	struct tab_group *g = view->group;
	wl_list_remove(&view->tab_link);
	view->group = NULL;
	view->tab_hidden = false;
	if (wl_list_length(&g->members) > 1) {
		frame_render(g->shown);
		return;
	}
	struct view *last = wl_container_of(g->members.next, last, tab_link);
	last->group = NULL;
	last->tab_hidden = false;
	free(g);
	back_to_own_size(last);
	frame_render(last);
}

void tabs_show(struct view *view)
{
	struct tab_group *g = view->group;
	if (g && g->shown != view)
		switch_to(g, view, g->shown, false);
}

void tabs_bring(struct view *view)
{
	struct tab_group *g = view->group;
	if (!g || g->shown == view)
		return;
	if (g->shown->fullscreen)
		view_set_fullscreen(g->shown, false);
	if (g->shown->minimized)
		view_restore(g->shown);
	/* Restoring may have brought it already, for a dialog of its own. */
	if (view->group == g)
		tabs_show(view);
}

bool tabs_fit_together(struct view *view, struct view *target)
{
	if (target->tab_hidden)
		target = target->group->shown;
	/* A hidden tab may be taken along, and needs only to qualify as a window. */
	return view != target && (!view->group || view->group != target->group) &&
		(view->tab_hidden ? tab_material(view) : tabs_can_join(view)) &&
		tabs_can_join(target) && view->no_tile == target->no_tile;
}

bool tabs_join(struct view *view, struct view *target)
{
	if (!tabs_fit_together(view, target))
		return false;
	if (target->tab_hidden)
		target = target->group->shown;
	if (view->group)
		tabs_detach(view);
	remember_own_size(view);

	struct tab_group *g = target->group;
	if (!g) {
		remember_own_size(target);
		g = calloc(1, sizeof(*g));
		wl_list_init(&g->members);
		wl_list_insert(&g->members, &target->tab_link);
		g->shown = target;
		target->group = g;
	}
	/* It takes the group's place, and what it was doing until now is over. */
	grab_give_way(view);
	view->tiled = false;
	view->tile_main = false;
	view->shaded = false;
	view->auto_shaded = false;
	view->tab_hidden = true;
	view->group = g;
	wl_list_insert(g->members.prev, &view->tab_link);
	switch_to(g, view, g->shown, false);
	return true;
}

struct view *tabs_previous(struct view *view)
{
	struct view *v;
	wl_list_for_each(v, &view->server->views, link) {
		if (!v->tab_hidden && tabs_fit_together(view, v))
			return v;
	}
	return NULL;
}

struct view *tabs_nth(struct view *view, int n)
{
	if (!view->group)
		return NULL;
	struct view *v;
	wl_list_for_each(v, &view->group->members, tab_link) {
		if (n-- == 0)
			return v;
	}
	return NULL;
}

void tabs_detach(struct view *view)
{
	struct tab_group *g = view->group;
	if (!g)
		return;
	if (g->shown == view)
		switch_to(g, neighbour(view), view, false);
	remove_member(view);
	struct server *server = view->server;
	tell_suspended(view, false);
	view_show(view, true);
	view_show_content(view);
	dialogs_show(view, true);
	view->tile_seq = --server->tile_front;
	back_to_own_size(view);
	/* It is placed by the size it is about to have. */
	int width = view->width, height = view->height;
	if (view->tab_own_w) {
		view->width = view->tab_own_w;
		view->height = view->tab_own_h;
	}
	view_place(view);
	view->width = width;
	view->height = height;
	frame_render(view);
	view_focus(view);
	tile_arrange(server);
	input_pointer_changed(server);
}

void tabs_view_gone(struct view *view)
{
	struct tab_group *g = view->group;
	if (!g)
		return;
	if (g->shown == view)
		switch_to(g, neighbour(view), view, true);
	remove_member(view);
}

void tabs_view_changed(struct view *view)
{
	if (view->group && !view->group->busy && !tab_material(view))
		tabs_detach(view);
}

void tabs_sync_sizes(struct view *view)
{
	struct tab_group *g = view->group;
	struct server *server = view->server;
	if (!g || view->tab_hidden || g->busy || (server->grabbed_view == view &&
			server->cursor_mode == CURSOR_RESIZE))
		return;
	int w, h;
	asked_size(view, &w, &h);
	struct view *m;
	wl_list_for_each(m, &g->members, tab_link) {
		if (m == view)
			continue;
		int mw = w, mh = h, cw, ch;
		view_apply_size_hints(m, &mw, &mh, false);
		asked_size(m, &cw, &ch);
		if (mw != cw || mh != ch)
			view_send_size(m, mw, mh);
	}
}

void tabs_step(struct view *view, bool forward)
{
	struct tab_group *g = view->group;
	if (!g || g->shown->fullscreen)
		return;
	struct wl_list *l = forward ? g->shown->tab_link.next : g->shown->tab_link.prev;
	if (l == &g->members)
		l = forward ? l->next : l->prev;
	struct view *v = wl_container_of(l, v, tab_link);
	tabs_show(v);
}

void tabs_show_nth(struct view *view, int n)
{
	struct tab_group *g = view->group;
	if (!g || g->shown->fullscreen)
		return;
	struct view *v = tabs_nth(view, n);
	if (v)
		tabs_show(v);
}

void tabs_after_restore(struct view *view)
{
	struct tab_group *g = view->group;
	if (!g)
		return;
	/* A dialog that appeared in a hidden tab meanwhile has waited long enough. */
	struct view *m, *d;
	wl_list_for_each(m, &g->members, tab_link) {
		if (m == g->shown)
			continue;
		wl_list_for_each(d, &view->server->views, link) {
			if (d->mapped && d->hidden_with_parent && view_is_transient_of(d, m)) {
				tabs_show(m);
				view_focus(d);
				return;
			}
		}
	}
}

int tabs_index(struct view *view)
{
	if (!view->group)
		return -1;
	int i = 0;
	struct view *v;
	wl_list_for_each(v, &view->group->members, tab_link) {
		if (v == view)
			return i;
		i++;
	}
	return -1;
}
