/*
 * Window management: xdg-shell toplevels and popups, plus the operations
 * shared with XWayland windows (focus, stacking, maximize, iconify).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "mwc.h"

struct wlr_surface *view_surface(struct view *view)
{
	if (view->type == VIEW_XDG)
		return view->xdg_toplevel->base->surface;
	return view->xsurface->surface;
}

const char *view_title(struct view *view)
{
	const char *t = view->type == VIEW_XDG ?
		view->xdg_toplevel->title : view->xsurface->title;
	return t ? t : "";
}

const char *view_app_id(struct view *view)
{
	const char *t = view->type == VIEW_XDG ?
		view->xdg_toplevel->app_id : view->xsurface->class;
	return t ? t : "";
}

struct output *output_at(struct server *server, double lx, double ly)
{
	struct wlr_output *o = wlr_output_layout_output_at(server->output_layout,
		lx, ly);
	if (o)
		return o->data;
	struct output *out;
	wl_list_for_each(out, &server->outputs, link)
		return out;
	return NULL;
}

struct wlr_box output_full_area(struct server *server, double lx, double ly)
{
	struct wlr_box box = { 0, 0, 1024, 768 };
	struct output *o = output_at(server, lx, ly);
	if (o)
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	return box;
}

struct wlr_box output_usable_area(struct server *server, double lx, double ly)
{
	struct output *o = output_at(server, lx, ly);
	return o ? o->usable : output_full_area(server, lx, ly);
}

/* Position of the client content inside the window tree. xdg clients may
 * draw shadows outside their window geometry, which we hide by offset. */
static void view_update_content_pos(struct view *view)
{
	int x = frame_left(view), y = frame_top(view);
	if (view->type == VIEW_XDG) {
		x -= view->xdg_toplevel->base->geometry.x;
		y -= view->xdg_toplevel->base->geometry.y;
	}
	wlr_scene_node_set_position(&view->content_tree->node, x, y);
}

static void xwayland_configure(struct view *view)
{
	if (view->type != VIEW_XWAYLAND)
		return;
	/* The size last requested, not view->width: a new size may not have
	 * been committed yet, and this configure must not take it back. */
	wlr_xwayland_surface_configure(view->xsurface,
		view->x + frame_left(view), view->y + frame_top(view),
		view->xsurface->width, view->xsurface->height);
}

void view_set_position(struct view *view, int x, int y)
{
	view->anchor_edges = 0;
	view->x = x;
	view->y = y;
	wlr_scene_node_set_position(&view->tree->node, x, y);
	xwayland_configure(view);
}

void view_send_size(struct view *view, int w, int h)
{
	if (view->type == VIEW_XDG) {
		view->anchor_serial = wlr_xdg_toplevel_set_size(
			view->xdg_toplevel, w, h);
		view->requested_w = w;
		view->requested_h = h;
	} else {
		wlr_xwayland_surface_configure(view->xsurface,
			view->x + frame_left(view), view->y + frame_top(view),
			w, h);
	}
}

void view_set_geometry(struct view *view, int x, int y, int w, int h)
{
	view->anchor_edges = 0;
	view->x = x;
	view->y = y;
	wlr_scene_node_set_position(&view->tree->node, x, y);
	view_send_size(view, w, h);
}

/* Called from the commit handlers once the client has a (new) size. */
static void view_apply_size(struct view *view, int w, int h)
{
	if (w == view->width && h == view->height) {
		view_update_content_pos(view);
		return;
	}
	/* Keep the opposite edge anchored while resizing from left/top. */
	if (view->anchor_edges) {
		if (view->anchor_edges & WLR_EDGE_LEFT)
			view->x = view->anchor_right - (w + frame_extra_w(view));
		if (view->anchor_edges & WLR_EDGE_TOP)
			view->y = view->anchor_bottom - (h + frame_extra_h(view));
		wlr_scene_node_set_position(&view->tree->node,
			view->x, view->y);
	}
	view->width = w;
	view->height = h;
	view_update_content_pos(view);
	frame_render(view);
}

/* Scene layers windows can be in, bottom to top. */
enum view_layer {
	VL_DESKTOP,
	VL_BELOW,
	VL_NORMAL,
	VL_ABOVE,
	VL_FULLSCREEN,
};

static enum view_layer own_layer(struct view *view, struct view *focus)
{
	if (view->wtype == WT_DESKTOP)
		return VL_DESKTOP;
	/* A fullscreen window covers top-layer panels only while it or one of
	 * its dialogs has the focus. Otherwise a window focused beside it, or a
	 * panel, would come up in the wrong place. */
	if (view->fullscreen && focus && view_transient_root(focus) == view)
		return VL_FULLSCREEN;
	bool x11 = view->type == VIEW_XWAYLAND;
	if (x11 && view->xsurface->below)
		return VL_BELOW;
	if ((x11 && view->xsurface->above) || view->wtype == WT_DOCK ||
			view->wtype == WT_NOTIFICATION)
		return VL_ABOVE;
	return VL_NORMAL;
}

void views_update_layers(struct server *server)
{
	struct view *focus = NULL, *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->activated)
			focus = v;
	}
	wl_list_for_each(v, &server->views, link) {
		enum view_layer layer = own_layer(v, focus);
		/* Dialogs follow the window they belong to into its layer, at
		 * least the normal one. */
		struct view *root = view_transient_root(v);
		if (root != v) {
			enum view_layer r = own_layer(root, focus);
			if (r < VL_NORMAL)
				r = VL_NORMAL;
			if (r > layer)
				layer = r;
		}
		struct wlr_scene_tree *tree[] = {
			[VL_DESKTOP] = server->layer_shell[1],
			[VL_BELOW] = server->layer_below,
			[VL_NORMAL] = server->layer_views,
			[VL_ABOVE] = server->layer_shell[2],
			[VL_FULLSCREEN] = server->layer_fullscreen,
		};
		wlr_scene_node_reparent(&v->tree->node, tree[layer]);
	}
}

void view_raise_one(struct view *view)
{
	wlr_scene_node_raise_to_top(&view->tree->node);
	wl_list_remove(&view->link);
	wl_list_insert(&view->server->views, &view->link);
	if (view->type == VIEW_XWAYLAND)
		wlr_xwayland_surface_restack(view->xsurface, NULL,
			XCB_STACK_MODE_ABOVE);
}

void view_set_activated(struct view *view, bool activated)
{
	view->activated = activated;
	if (view->type == VIEW_XDG)
		wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
	else
		wlr_xwayland_surface_activate(view->xsurface, activated);
	views_update_layers(view->server);
	frame_render(view);
}

/* Dialogs stay above the window they belong to, in their own order. */
static void view_raise(struct view *view)
{
	struct server *server = view->server;
	view_raise_one(view);
	int n = 0, count = wl_list_length(&server->views);
	struct view **kids = calloc(count, sizeof(*kids));
	struct view *v;
	wl_list_for_each_reverse(v, &server->views, link) {
		if (v->mapped && view_is_transient_of(v, view))
			kids[n++] = v;
	}
	for (int i = 0; i < n; i++)
		view_raise_one(kids[i]);
	free(kids);
}

static struct view *focused_view(struct server *server)
{
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->activated)
			return v;
	}
	return NULL;
}

void view_focus(struct view *view)
{
	if (!view || !view->mapped)
		return;
	struct server *server = view->server;
	struct wlr_seat *seat = server->seat;

	if (view->minimized || view->hidden_with_parent) {
		view_restore(view);
		return;
	}
	/* A panel or a window without input focus only comes to the front. */
	if (!view_focusable(view)) {
		view_raise(view);
		if (view->shaded)
			tile_unshade(view);
		input_pointer_changed(server);
		return;
	}

	struct view *prev = focused_view(server);
	if (prev && prev != view)
		view_set_activated(prev, false);
	if (!view->activated)
		view_set_activated(view, true);
	view_raise(view);

	/* An exclusive layer surface keeps the keyboard until it goes away. */
	if (server->exclusive_layer)
		return;
	server->focused_layer = NULL;
	struct wlr_surface *surface = view_surface(view);
	if (surface) {
		/* A client's menu closes when the focus goes elsewhere, and only then does
		 * the keyboard follow: its grab keeps it where it is. A drag has to run
		 * its course, see keyboard_grab_end(). */
		if (surface != seat->keyboard_state.focused_surface && !seat->drag)
			wlr_seat_pointer_end_grab(seat);
		input_keyboard_enter(server, surface);
	}
	/* Typing into a window that can't be seen helps no one. Focused first, so a
	 * full stack shades another one to make room. */
	if (view->shaded)
		tile_unshade(view);
	input_pointer_changed(server);
}

/* Give the focus to the topmost remaining window, or to nobody. A shaded window
 * only gets it when asked for by name. */
static void focus_next(struct server *server, struct view *except)
{
	/* Prefer the window a dialog belongs to. */
	struct view *v = except ? view_parent(except) : NULL;
	if (v && v != except && view_focusable(v) && !v->shaded) {
		view_focus(v);
		return;
	}
	wl_list_for_each(v, &server->views, link) {
		if (v != except && view_focusable(v) && !v->shaded) {
			view_focus(v);
			return;
		}
	}
	if (except && except->activated)
		view_set_activated(except, false);
	if (!server->focused_layer)
		wlr_seat_keyboard_notify_clear_focus(server->seat);
}

static void view_maximize_at(struct view *view, bool maximized, double px,
	double py);

/* Fill what its state says on the output at (px, py), as far as the client's
 * limits allow: all of the usable area, or a snap position in it. */
static void view_fit_state(struct view *view, double px, double py)
{
	view->fit_px = px;
	view->fit_py = py;
	struct wlr_box a = output_usable_area(view->server, px, py);
	if (view->snap != SNAP_NONE)
		a = snap_rect(view->snap, a);
	int w = a.width - frame_extra_w(view), h = a.height - frame_extra_h(view);
	view_apply_size_hints(view, &w, &h, false);
	view_set_geometry(view, a.x, a.y, w, h);
}

static void view_refit(struct view *view)
{
	view_fit_state(view, view->x + view->width / 2.0,
		view->y + view->height / 2.0);
}

/* What the client should know about it. Snapped halves count as maximized
 * vertically for X11 programs, as in mwm-x11; xdg clients hear which of their
 * edges touch something. */
static void view_publish_snap(struct view *view)
{
	bool half = view->snap == SNAP_LEFT || view->snap == SNAP_RIGHT;
	if (view->type == VIEW_XWAYLAND) {
		/* Maximized or snapped is kept while tiled, not shown. */
		bool shown = !view->tiled;
		wlr_xwayland_surface_set_maximized(view->xsurface,
			shown && view->maximized, shown && (view->maximized || half));
		return;
	}
	if (!view->xdg_toplevel->base->initialized)
		return;
	uint32_t edges = WLR_EDGE_NONE;
	switch (view->tiled ? SNAP_NONE : view->snap) {
	case SNAP_LEFT:
		edges = WLR_EDGE_LEFT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
		break;
	case SNAP_RIGHT:
		edges = WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
		break;
	case SNAP_TOP_LEFT:
		edges = WLR_EDGE_LEFT | WLR_EDGE_TOP;
		break;
	case SNAP_TOP_RIGHT:
		edges = WLR_EDGE_RIGHT | WLR_EDGE_TOP;
		break;
	case SNAP_BOTTOM_LEFT:
		edges = WLR_EDGE_LEFT | WLR_EDGE_BOTTOM;
		break;
	case SNAP_BOTTOM_RIGHT:
		edges = WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM;
		break;
	default:
		break;
	}
	if (view->tiled)
		edges = WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
	wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, edges);
	/* From maximized straight to snapped, it must stop thinking it is. */
	wlr_xdg_toplevel_set_maximized(view->xdg_toplevel,
		view->maximized && !view->tiled);
}

/* What it goes back to is only recorded while it is free. The size is the one last
 * asked for: right after a change, the client may not have committed it yet. */
static void view_save_free(struct view *view)
{
	if (view->snap != SNAP_NONE || view->maximized)
		return;
	if (!view->mapped) {
		/* It hasn't been anywhere yet. Its size is its own to choose, which is
		 * what a size of 0 tells it, and the place is the start of the cascade. */
		struct wlr_box a = output_usable_area(view->server, view->server->cursor->x,
			view->server->cursor->y);
		view->saved = (struct wlr_box){ a.x + 32, a.y + 32, 0, 0 };
		return;
	}
	view->saved = (struct wlr_box){ view->x, view->y, view->width, view->height };
	if (view->type == VIEW_XWAYLAND) {
		view->saved.width = view->xsurface->width;
		view->saved.height = view->xsurface->height;
	} else {
		view->saved.width = view->requested_w;
		view->saved.height = view->requested_h;
	}
}

void view_snap(struct view *view, enum snap s, double px, double py)
{
	if (view->fullscreen || view->tiled)
		return;
	if (s == SNAP_MAXIMIZE) {
		view_maximize_at(view, true, px, py);
		return;
	}
	if (s == SNAP_NONE) {
		if (view->snap == SNAP_NONE) {
			view_set_maximized(view, false);
			return;
		}
		view->snap = SNAP_NONE;
		view_set_geometry(view, view->saved.x, view->saved.y,
			view->saved.width, view->saved.height);
	} else {
		view_save_free(view);
		view->snap = s;
		view->maximized = false;
		view_fit_state(view, px, py);
	}
	view_publish_snap(view);
	frame_render(view);
}

void view_focus_next(struct view *except)
{
	focus_next(except->server, except);
}

void view_close(struct view *view)
{
	if (view->type == VIEW_XDG)
		wlr_xdg_toplevel_send_close(view->xdg_toplevel);
	else
		wlr_xwayland_surface_close(view->xsurface);
}

static void view_lower_one(struct view *view)
{
	struct server *server = view->server;
	wlr_scene_node_lower_to_bottom(&view->tree->node);
	wl_list_remove(&view->link);
	wl_list_insert(server->views.prev, &view->link);
}

/* Whether anything could take the focus from it. If not it keeps it, even when it
 * goes to the back: it is still the only window there is to type into. */
static bool view_has_successor(struct view *view)
{
	struct view *v;
	wl_list_for_each(v, &view->server->views, link) {
		if (v != view && view_focusable(v) && !v->shaded &&
				view_transient_root(v) != view_transient_root(view))
			return true;
	}
	return false;
}

void view_lower(struct view *view)
{
	struct server *server = view->server;
	view_lower_one(view);
	if (view->activated && view_has_successor(view)) {
		focus_next(server, view);
		/* Losing the focus may have moved it to another scene layer. */
		wlr_scene_node_lower_to_bottom(&view->tree->node);
	}
	input_pointer_changed(server);
}

void view_set_maximized(struct view *view, bool maximized)
{
	view_maximize_at(view, maximized, view->x + view->width / 2.0,
		view->y + view->height / 2.0);
}

/* An xdg surface takes no configures before its first commit, or after a null
 * buffer started it over. Until then a state can only be noted, and goes out with
 * the first configure, see xdg_send_first_configure(). */
static bool view_configurable(struct view *view)
{
	return view->type != VIEW_XDG || view->xdg_toplevel->base->initialized;
}

static void view_maximize_at(struct view *view, bool maximized, double px,
	double py)
{
	if (!view_configurable(view)) {
		view->maximized = maximized;
		return;
	}
	/* In the layout, the layout decides; see tile_promote(). */
	if (view->maximized == maximized || view->fullscreen || view->tiled)
		goto ack;
	if (maximized && !(view->funcs & FUNC_MAXIMIZE))
		goto ack;
	grab_give_way(view);
	if (maximized) {
		view_save_free(view);
		view->snap = SNAP_NONE;
		view->maximized = true;
		view_fit_state(view, px, py);
	} else {
		view->maximized = false;
		view_set_geometry(view, view->saved.x, view->saved.y,
			view->saved.width, view->saved.height);
	}
	frame_render(view);
	if (view->type == VIEW_XDG)
		view_publish_snap(view);
ack:
	/* Maximized or snapped is kept while tiled, not shown. */
	bool shown = view->maximized && !view->tiled;
	if (view->type == VIEW_XDG) {
		if (view->xdg_toplevel->base->initialized)
			wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, shown);
	} else {
		wlr_xwayland_surface_set_maximized(view->xsurface, shown, shown);
	}
}

/* Back to the geometry or state it floated in, on the output it came from. */
static void view_apply_floating(struct view *view)
{
	if (view->maximized || view->snap != SNAP_NONE)
		view_fit_state(view, view->fit_px, view->fit_py);
	else
		view_set_geometry(view, view->saved.x, view->saved.y,
			view->saved.width, view->saved.height);
}

/* Only a client that is shown should be seen. */
static void view_show_content(struct view *view)
{
	if (view->content_tree)
		wlr_scene_node_set_enabled(&view->content_tree->node, !view->shaded);
}

void view_set_tiled(struct view *view, bool on, bool apply)
{
	if (view->tiled == on)
		return;
	grab_give_way(view);
	if (on && !view->tile_back)
		view_save_free(view);
	view->shaded = false;
	view->auto_shaded = false;
	view_show_content(view);
	view->tile_back = false;
	view->tiled = on;
	view->tile_main = false;
	view_update_decorations(view);
	/* What it tells the client depends on being tiled. */
	view_maximize_at(view, view->maximized, 0, 0);
	view_publish_snap(view);
	if (!on && apply)
		view_apply_floating(view);
	frame_render(view);
}

void view_apply_tile(struct view *view)
{
	const struct wlr_box r = view->tile;
	int w = r.width - frame_extra_w(view), h = r.height - frame_extra_h(view);
	if (view->shaded) {
		/* It keeps its client height for when it unrolls. */
		h = view->type == VIEW_XDG ? view->requested_h : view->xsurface->height;
	}
	view_apply_size_hints(view, &w, &h, true);
	/* The main window keeps to the right edge, the stack to the left. */
	int x = view->tile_main ? r.x + r.width - (w + frame_extra_w(view)) : r.x;
	view_set_geometry(view, x, r.y, w, h);
	if (view->tile_main && view->type == VIEW_XDG) {
		/* Also if the client decides on a size of its own. */
		view->anchor_edges = WLR_EDGE_LEFT;
		view->anchor_right = r.x + r.width;
	}
	view_show_content(view);
	frame_render(view);
}

void view_set_fullscreen(struct view *view, bool fullscreen)
{
	if (!view_configurable(view)) {
		view->fullscreen = fullscreen;
		view->decorated = view->ssd && !fullscreen;
		return;
	}
	if (view->fullscreen != fullscreen) {
		grab_give_way(view);
		if (fullscreen) {
			/* A tiled window leaves the layout for fullscreen and goes back
			 * into it afterwards; saved still holds its floating geometry. */
			if (view->tiled) {
				view_set_tiled(view, false, false);
				view->tile_back = true;
			} else {
				view_save_free(view);
			}
			struct wlr_box a = output_full_area(view->server,
				view->x + view->width / 2.0,
				view->y + view->height / 2.0);
			view->fullscreen = true;
			views_update_layers(view->server);
			/* Fullscreen windows lose their frame for the duration. */
			view->decorated = false;
			view_update_content_pos(view);
			xwayland_publish_frame_extents(view);
			frame_render(view);
			view_set_geometry(view, a.x, a.y, a.width, a.height);
			view_focus(view);
		} else {
			view->fullscreen = false;
			views_update_layers(view->server);
			view->decorated = view->ssd;
			view_update_content_pos(view);
			xwayland_publish_frame_extents(view);
			if (view->tile_back && view->server->tiling)
				tile_arrange(view->server); /* places it, saved untouched */
			if (!view->tiled) {
				/* Floating, or no longer fit for the layout. */
				view->tile_back = false;
				view_apply_floating(view);
			}
			frame_render(view);
		}
		/* Into fullscreen the others close the gap; out of it a window fit
		 * for the layout joins it, wherever it came from. */
		tile_arrange(view->server);
		input_pointer_changed(view->server);
	}
	if (view->type == VIEW_XDG) {
		if (view->xdg_toplevel->base->initialized)
			wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel,
				view->fullscreen);
	} else {
		wlr_xwayland_surface_set_fullscreen(view->xsurface,
			view->fullscreen);
	}
}

/* Only X11 panels reserve space through their window. */
static void struts_changed(struct view *view)
{
	if (view->type == VIEW_XWAYLAND && view->xsurface->strut_partial)
		layers_arrange_all(view->server);
}

static void view_show(struct view *view, bool show)
{
	wlr_scene_node_set_enabled(&view->tree->node, show);
	if (view->type == VIEW_XWAYLAND)
		wlr_xwayland_surface_set_minimized(view->xsurface, !show);
}

void view_minimize(struct view *view)
{
	if (view->minimized || !view->mapped)
		return;
	struct server *server = view->server;
	grab_give_way(view);
	bool had_focus = view->activated;
	view->minimized = true;
	view_show(view, false);
	/* Dialogs go away with the window they belong to, without icons. */
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->mapped && !v->minimized && !v->hidden_with_parent &&
				view_is_transient_of(v, view)) {
			had_focus |= v->activated;
			v->hidden_with_parent = true;
			view_show(v, false);
			if (v->activated)
				view_set_activated(v, false);
		}
	}
	icon_render(view);
	icons_arrange(server);
	wlr_scene_node_set_enabled(&view->icon_node->node, true);
	struts_changed(view);
	/* The layout first: a shaded window may become the main one, and take the focus. */
	tile_arrange(server);
	if (had_focus)
		focus_next(server, view);
	input_pointer_changed(server);
}

void view_restore(struct view *view)
{
	if (view->hidden_with_parent) {
		/* It comes back with the nearest window up the chain that was
		 * minimized itself. */
		struct view *a = view;
		while (a && a->hidden_with_parent)
			a = view_parent(a);
		if (a)
			view_restore(a);
		return;
	}
	if (view->minimized) {
		view->minimized = false;
		wlr_scene_node_set_enabled(&view->icon_node->node, false);
		view_show(view, true);
		struct view *v;
		wl_list_for_each(v, &view->server->views, link) {
			if (v->hidden_with_parent && view_is_transient_of(v, view)) {
				v->hidden_with_parent = false;
				view_show(v, true);
			}
		}
		struts_changed(view);
		/* Back from an icon it is the newest window of the layout. */
		view->tile_seq = --view->server->tile_front;
		tile_arrange(view->server);
		view_focus(view);
	} else if (view->shaded) {
		tile_unshade(view);
	} else if (view->fullscreen) {
		view_set_fullscreen(view, false);
	} else if (view->tiled) {
		return;
	} else if (view->maximized) {
		view_set_maximized(view, false);
	} else if (view->snap != SNAP_NONE) {
		view_snap(view, SNAP_NONE, 0, 0);
	}
}

void view_update_title(struct view *view)
{
	frame_render(view);
	if (view->minimized)
		icon_render(view);
}

/* Fill the usable area again after it, or the frame's size, has changed. */

void views_usable_area_changed(struct server *server)
{
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		v->icon_placed = false;
		if (v->mapped && (v->maximized || v->snap != SNAP_NONE) &&
				!v->fullscreen && !v->tiled)
			view_refit(v);
	}
	icons_arrange(server);
	tile_arrange(server);
}

void views_layout_changed(struct server *server)
{
	/* What they point at may have gone with an output. */
	if (server->menu.open)
		menu_close(server);
	if (server->grabbed_view)
		grab_give_way(server->grabbed_view);
	if (wl_list_empty(&server->outputs))
		return;
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		v->icon_placed = false;
		if (!v->mapped)
			continue;
		struct wlr_box box = { v->x, v->y, v->width + frame_extra_w(v),
			frame_height(v) };
		bool reachable = wlr_output_layout_intersects(server->output_layout, NULL,
			&box);
		/* output_at() falls back to an existing output for any other point. */
		struct wlr_box a = output_usable_area(server, v->x + box.width / 2.0,
			v->y + box.height / 2.0);
		if (!reachable) {
			/* What it goes back to was on that output as well. */
			v->saved.x = a.x + 32;
			v->saved.y = a.y + 32;
			v->fit_px = a.x + a.width / 2.0;
			v->fit_py = a.y + a.height / 2.0;
		}
		if (v->tiled) {
			continue;
		} else if (v->fullscreen) {
			struct wlr_box f = output_full_area(server, reachable ?
				v->x + box.width / 2.0 : a.x + 1, reachable ?
				v->y + box.height / 2.0 : a.y + 1);
			view_set_geometry(v, f.x, f.y, f.width, f.height);
		} else if (v->maximized || v->snap != SNAP_NONE) {
			if (!reachable)
				view_fit_state(v, v->fit_px, v->fit_py);
			else
				view_refit(v);
		} else if (!reachable) {
			view_set_position(v, a.x + 32, a.y + 32);
		}
	}
	icons_arrange(server);
	tile_arrange(server);
	input_pointer_changed(server);
}

void view_frame_changed(struct view *view)
{
	/* The client keeps its size and the frame around it changes, except
	 * where the frame's size is fixed. */
	view_update_content_pos(view);
	if ((view->maximized || view->snap != SNAP_NONE) && !view->fullscreen &&
			!view->tiled)
		view_refit(view);
	else
		xwayland_configure(view);
	frame_render(view);
	xwayland_publish_frame_extents(view);
}

void view_theme_changed(struct view *view)
{
	config_view_shades(view);
	if (!view->mapped)
		return;
	view_frame_changed(view); /* the title bar grows with the font */
	if (view->minimized)
		icon_render(view);
}

void view_cycle(struct server *server, bool backwards)
{
	struct view *top = NULL, *v;
	wl_list_for_each(v, &server->views, link) {
		if (view_focusable(v)) {
			top = v;
			break;
		}
	}
	if (!top)
		return;
	if (backwards) {
		/* Push the current window to the back, with what belongs to it, and focus
		 * the next one. */
		struct view *active = focused_view(server);
		struct view *root = view_transient_root(active ? active : top);
		bool had_successor = view_has_successor(root);
		int count = wl_list_length(&server->views), n = 0;
		struct view **group = calloc(count, sizeof(*group));
		wl_list_for_each(v, &server->views, link) {
			if (v != root && view_transient_root(v) == root)
				group[n++] = v;
		}
		for (int i = 0; i < n; i++)
			view_lower_one(group[i]);
		free(group);
		view_lower_one(root);
		if (had_successor)
			focus_next(server, root);
		input_pointer_changed(server);
		return;
	}
	wl_list_for_each_reverse(v, &server->views, link) {
		if (view_focusable(v)) {
			view_focus(v);
			return;
		}
	}
}

void view_begin_interactive(struct view *view, enum cursor_mode mode,
	uint32_t edges, bool until_click)
{
	struct server *server = view->server;
	/* A client can ask for this at any time: before it is mapped, while minimized,
	 * or during another grab. Only something visible and free can be grabbed. */
	if (!view->mapped || view->minimized || view->hidden_with_parent ||
			server->cursor_mode != CURSOR_PASSTHROUGH)
		return;
	if (view->fullscreen)
		return;
	/* The client's limits may have changed since it was mapped. */
	xwayland_read_motif_hints(view);
	view_update_properties(view);
	if (mode == CURSOR_RESIZE &&
			(view->maximized || !(view->funcs & FUNC_RESIZE)))
		return;
	if (mode == CURSOR_MOVE && !(view->funcs & FUNC_MOVE))
		return;
	server->grabbed_view = view;
	server->cursor_mode = mode;
	server->grab_until_click = until_click;
	server->grab_outline = outline_wanted();
	server->grab_detached = false;
	server->grab_changed = false;
	server->grab_px = server->cursor->x;
	server->grab_py = server->cursor->y;
	/* The size last asked for: the client may not have got there yet. */
	int w = view->type == VIEW_XDG ? view->requested_w : view->xsurface->width;
	int h = view->type == VIEW_XDG ? view->requested_h : view->xsurface->height;
	server->grab_orig = (struct wlr_box){ view->x, view->y, w, h };
	server->grab_orig_maximized = view->maximized;
	server->grab_orig_snap = view->snap;
	server->grab_zone = SNAP_NONE;
	server->grab_target = server->grab_orig;
	server->grab_geobox = (struct wlr_box){ view->x, view->y,
		w + frame_extra_w(view), h + frame_extra_h(view) };

	if (mode == CURSOR_MOVE) {
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
		cursor_set_name(server, "move");
	} else {
		double bx = view->x + ((edges & WLR_EDGE_RIGHT) ?
			server->grab_geobox.width : 0);
		double by = view->y + ((edges & WLR_EDGE_BOTTOM) ?
			server->grab_geobox.height : 0);
		server->grab_x = server->cursor->x - bx;
		server->grab_y = server->cursor->y - by;
		server->resize_edges = edges;
		if (view->type == VIEW_XDG) {
			if (!server->grab_outline)
				wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, true);
			view->anchor_edges = edges;
			view->anchor_right = view->x + server->grab_geobox.width;
			view->anchor_bottom = view->y + server->grab_geobox.height;
			view->anchor_serial = view->xdg_toplevel->base->
				scheduled_serial;
		}
	}
	wlr_seat_pointer_notify_clear_focus(server->seat);
	grab_started(server);
}

void view_drag_free(struct view *view)
{
	struct server *server = view->server;
	double frac = (server->cursor->x - view->x) /
		(double)(view->width + frame_extra_w(view));
	/* Maximized since its first commit, it has no size of its own yet, and will
	 * choose one. Until then the outline needs something to show. */
	int w = view->saved.width ? view->saved.width : view->width / 2;
	int h = view->saved.height ? view->saved.height : view->height / 2;
	server->grab_detaching = true;
	view_snap(view, SNAP_NONE, 0, 0);
	server->grab_detaching = false;
	int nw = w + frame_extra_w(view);
	view_set_position(view, server->cursor->x - frac * nw,
		server->cursor->y - frame_border(view) - frame_title(view) / 2);
	server->grab_x = server->cursor->x - view->x;
	server->grab_y = server->cursor->y - view->y;
	/* The client hasn't committed its old size yet, so don't go by view->width. */
	server->grab_target.width = w;
	server->grab_target.height = h;
	server->grab_geobox.width = nw;
	server->grab_geobox.height = h + frame_extra_h(view);
}

struct view *view_at(struct server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy,
	enum frame_part *part, uint32_t *edges, bool *on_icon)
{
	*surface = NULL;
	*part = PART_NONE;
	*edges = 0;
	*on_icon = false;
	double nx, ny;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, &nx, &ny);
	if (!node)
		return NULL;
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *ss =
			wlr_scene_surface_try_from_buffer(sb);
		if (ss) {
			if (layer_surface_is_desktop(ss->surface))
				return NULL;
			*surface = ss->surface;
			*sx = nx;
			*sy = ny;
		}
	}
	struct wlr_scene_node *n = node;
	while (n && !n->data) {
		n = n->parent ? &n->parent->node : NULL;
	}
	if (!n)
		return NULL;
	struct view *view = n->data;
	if (n == &view->icon_node->node) {
		*on_icon = true;
		*surface = NULL;
		return view;
	}
	if (node == &view->frame_node->node) {
		*part = frame_hit(view, (int)(lx - view->x),
			(int)(ly - view->y), edges);
	} else {
		*part = PART_CLIENT;
	}
	return view;
}

/* ---- common view lifecycle -------------------------------------------- */

void view_init_common(struct view *view)
{
	struct server *server = view->server;
	view->tree = wlr_scene_tree_create(server->layer_views);
	view->tree->node.data = view;
	view->frame_node = wlr_scene_buffer_create(view->tree, NULL);
	view->icon_node = wlr_scene_buffer_create(server->layer_icons, NULL);
	view->icon_node->node.data = view;
	wlr_scene_node_set_enabled(&view->icon_node->node, false);
	wlr_scene_node_set_enabled(&view->tree->node, false);
	wl_list_insert(&server->views, &view->link);
	view->shade_active = theme.active;
	view->shade_inactive = theme.inactive;
	view->decor = DECOR_ALL;
	view->funcs = FUNC_ALL;
}

/* New windows cascade from the top-left of the output under the pointer. A
 * dialog goes over the window it belongs to, or to the centre. */
static void view_place(struct view *view)
{
	struct server *server = view->server;
	struct wlr_box a = output_usable_area(server, server->cursor->x,
		server->cursor->y);
	int w = view->width + frame_extra_w(view);
	int h = view->height + frame_extra_h(view);
	int x, y;
	struct view *parent = view_parent(view);
	if (parent && parent->mapped && !parent->minimized) {
		x = parent->x + (parent->width + frame_extra_w(parent) - w) / 2;
		y = parent->y + (parent->height + frame_extra_h(parent) - h) / 2;
	} else if (view->wtype == WT_DIALOG || view->wtype == WT_SPLASH) {
		x = a.x + (a.width - w) / 2;
		y = a.y + (a.height - h) / 2;
	} else {
		int step = theme.title_height + theme.border;
		x = a.x + 32 + server->cascade * step;
		y = a.y + 32 + server->cascade * step;
		if (x + w > a.x + a.width || y + h > a.y + a.height) {
			server->cascade = 0;
			x = a.x + (a.width - w) / 2;
			y = a.y + (a.height - h) / 2;
		} else {
			server->cascade = (server->cascade + 1) % 10;
		}
	}
	/* Keep at least the title bar reachable. */
	if (x + w > a.x + a.width)
		x = a.x + a.width - w;
	if (y + h > a.y + a.height)
		y = a.y + a.height - h;
	if (x < a.x)
		x = a.x;
	if (y < a.y)
		y = a.y;
	view_set_position(view, x, y);
}

void view_handle_map(struct view *view)
{
	view->mapped = true;
	config_view_shades(view); /* the class or app_id is known by now */
	config_view_tiling(view);
	view->tile_seq = --view->server->tile_front; /* a new window leads the layout */
	view_update_properties(view);
	wlr_scene_node_set_enabled(&view->tree->node, true);
	view_update_content_pos(view);
	/* A dialog of a window that is an icon by now comes back with it. */
	struct view *parent = view_parent(view);
	bool hide = parent && (parent->minimized || parent->hidden_with_parent);
	if (view->placed)
		view_set_position(view, view->x, view->y);
	else if (!view->maximized && !view->fullscreen)
		view_place(view);
	frame_render(view);
	views_update_layers(view->server);
	struts_changed(view);
	if (hide) {
		view->hidden_with_parent = true;
		view_show(view, false);
	}
	/* Into the layout before it gets the focus, so it doesn't jump. */
	tile_arrange(view->server);
	if (hide)
		return;
	if (view->wtype != WT_SPLASH)
		view_focus(view);
	else
		view_raise(view);
}

void view_handle_unmap(struct view *view)
{
	struct server *server = view->server;
	if (server->grabbed_view == view)
		grab_forget(server);
	if (server->menu.open && server->menu.target == view)
		menu_close(server);
	if (server->last_click_view == view)
		server->last_click_view = NULL;
	/* Dialogs hidden with it have nothing left to come back with. */
	struct view *v, *shown = NULL;
	wl_list_for_each(v, &server->views, link) {
		if (v->hidden_with_parent && view_is_transient_of(v, view)) {
			v->hidden_with_parent = false;
			view_show(v, true);
			shown = v;
		}
	}
	if (shown && !focused_view(server))
		view_focus(shown);
	bool was_active = view->activated;
	view->mapped = false;
	view->activated = false;
	view->minimized = false;
	view->hidden_with_parent = false;
	view->tiled = false;
	view->tile_main = false;
	view->tile_back = false;
	view->shaded = false;
	view_show_content(view);
	wlr_scene_node_set_enabled(&view->tree->node, false);
	wlr_scene_node_set_enabled(&view->icon_node->node, false);
	struts_changed(view);
	/* The layout first: a shaded window may become the main one, and take the focus. */
	tile_arrange(server);
	if (was_active)
		focus_next(server, view);
	input_pointer_changed(server);
}

void view_handle_destroy(struct view *view)
{
	/* A window that was never mapped gets here without view_handle_unmap(). */
	struct server *server = view->server;
	if (server->grabbed_view == view)
		grab_forget(server);
	if (server->menu.open && server->menu.target == view)
		menu_close(server);
	if (server->last_click_view == view)
		server->last_click_view = NULL;
	wl_list_remove(&view->link);
	wlr_scene_node_destroy(&view->icon_node->node);
	wlr_scene_node_destroy(&view->tree->node);
	free(view);
}

/* ---- xdg-shell ---------------------------------------------------------- */

struct xdg_deco {
	struct wlr_xdg_toplevel_decoration_v1 *wlr;
	struct view *view;
	struct wl_listener request_mode;
	struct wl_listener destroy;
};

static struct view *view_from_xdg_toplevel(struct server *server,
	struct wlr_xdg_toplevel *toplevel)
{
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->type == VIEW_XDG && v->xdg_toplevel == toplevel)
			return v;
	}
	return NULL;
}

/* The state noted so far goes out with the first configure, and with it the size
 * that belongs to it. Otherwise the client chooses. */
static void xdg_send_first_configure(struct view *view)
{
	struct server *server = view->server;
	struct wlr_xdg_toplevel *top = view->xdg_toplevel;
	view_update_properties(view);
	view->placed = false;
	if (view->fullscreen) {
		struct wlr_box a = output_full_area(server, server->cursor->x,
			server->cursor->y);
		view->x = a.x;
		view->y = a.y;
		view->placed = true;
		view_send_size(view, a.width, a.height);
		wlr_xdg_toplevel_set_fullscreen(top, true);
	} else if (view->maximized) {
		struct wlr_box a = output_usable_area(server, server->cursor->x,
			server->cursor->y);
		int w = a.width - frame_extra_w(view), h = a.height - frame_extra_h(view);
		view_apply_size_hints(view, &w, &h, false);
		view->x = a.x;
		view->y = a.y;
		view->placed = true;
		view_send_size(view, w, h);
		wlr_xdg_toplevel_set_maximized(top, true);
	} else {
		view_send_size(view, 0, 0);
	}
	/* What it goes back to, if it asked for a state before ever being anywhere. */
	if (view->placed && !view->saved.x && !view->saved.y && !view->saved.width) {
		struct wlr_box a = output_usable_area(server, server->cursor->x,
			server->cursor->y);
		view->saved = (struct wlr_box){ a.x + 32, a.y + 32, 0, 0 };
	}
}

static void xdg_commit(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, commit);
	struct wlr_xdg_surface *base = view->xdg_toplevel->base;

	if (base->initial_commit) {
		wlr_xdg_toplevel_set_wm_capabilities(view->xdg_toplevel,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
		struct xdg_deco *deco = view->deco;
		if (deco)
			wlr_xdg_toplevel_decoration_v1_set_mode(deco->wlr,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		xdg_send_first_configure(view);
		return;
	}
	if (!view->mapped)
		return;
	view_apply_size(view, base->geometry.width, base->geometry.height);
	/* Caught up: from now on what it committed is what counts, even if that
	 * isn't quite what it was asked for. */
	if (base->current.configure_serial >= view->anchor_serial) {
		view->requested_w = view->width;
		view->requested_h = view->height;
	}
	/* Its limits may have changed. */
	view_update_properties(view);

	/* The drag is over and the client has answered the last size request. */
	struct server *server = view->server;
	bool grabbed = server->cursor_mode == CURSOR_RESIZE &&
		server->grabbed_view == view;
	if (view->anchor_edges && !grabbed &&
			base->current.configure_serial >= view->anchor_serial)
		view->anchor_edges = 0;
}

static void xdg_map(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, map);
	struct wlr_box *g = &view->xdg_toplevel->base->geometry;
	view->width = view->requested_w = g->width;
	view->height = view->requested_h = g->height;
	view_handle_map(view);
}

static void xdg_unmap(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, unmap);
	view_handle_unmap(view);
	/* Unmapped, the surface starts over, and so do its states. */
	view->maximized = false;
	view->fullscreen = false;
	view->snap = SNAP_NONE;
	view->decorated = view->ssd;
}

static void xdg_destroy(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, destroy);
	struct xdg_deco *deco = view->deco;
	if (deco)
		deco->view = NULL;
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->request_show_window_menu.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_parent.link);
	wl_list_remove(&view->set_app_id.link);
	view_handle_destroy(view);
}

static void xdg_set_parent(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_parent);
	view_update_properties(view);
	views_update_layers(view->server);
	tile_arrange(view->server); /* dialogs float */
}

static void xdg_set_app_id(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_app_id);
	if (!view->mapped)
		return; /* the map picks it up */
	config_view_shades(view);
	config_view_tiling(view);
	view_theme_changed(view);
	tile_arrange(view->server);
}

static void xdg_request_move(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_move);
	struct wlr_xdg_toplevel_move_event *event = data;
	/* Only for a button still down: a late request would glue the window to the
	 * pointer until the next click. */
	if (wlr_seat_validate_pointer_grab_serial(view->server->seat, NULL,
			event->serial))
		view_begin_interactive(view, CURSOR_MOVE, 0, false);
}

static void xdg_request_resize(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct view *view = wl_container_of(listener, view, request_resize);
	if (wlr_seat_validate_pointer_grab_serial(view->server->seat, NULL,
			event->serial))
		view_begin_interactive(view, CURSOR_RESIZE, event->edges, false);
}

static void xdg_request_maximize(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_maximize);
	view_set_maximized(view, view->xdg_toplevel->requested.maximized);
}

static void xdg_request_fullscreen(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_fullscreen);
	view_set_fullscreen(view, view->xdg_toplevel->requested.fullscreen);
}

static void xdg_request_minimize(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_minimize);
	view_minimize(view);
}

static void xdg_request_show_window_menu(struct wl_listener *listener,
	void *data)
{
	struct view *view = wl_container_of(listener, view,
		request_show_window_menu);
	struct wlr_xdg_toplevel_show_window_menu_event *event = data;
	/* In answer to something the user just did to this window, since the menu takes
	 * over the keyboard. */
	if (!view->activated ||
			!wlr_seat_client_validate_event_serial(event->seat, event->serial))
		return;
	menu_open_window(view->server, view, view->x + frame_left(view) + event->x,
		view->y + frame_top(view) + event->y);
}

static void xdg_set_title(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_title);
	view_update_title(view);
}

static void new_xdg_toplevel(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	struct view *view = calloc(1, sizeof(*view));
	view->server = server;
	view->type = VIEW_XDG;
	view->xdg_toplevel = toplevel;
	view_init_common(view);
	view->content_tree = wlr_scene_xdg_surface_create(view->tree,
		toplevel->base);
	xdg_surface_set_tree(toplevel->base, view->content_tree);

	view->map.notify = xdg_map;
	wl_signal_add(&toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = xdg_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
	view->commit.notify = xdg_commit;
	wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
	view->destroy.notify = xdg_destroy;
	wl_signal_add(&toplevel->events.destroy, &view->destroy);
	view->request_move.notify = xdg_request_move;
	wl_signal_add(&toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = xdg_request_resize;
	wl_signal_add(&toplevel->events.request_resize, &view->request_resize);
	view->request_maximize.notify = xdg_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize,
		&view->request_maximize);
	view->request_minimize.notify = xdg_request_minimize;
	wl_signal_add(&toplevel->events.request_minimize,
		&view->request_minimize);
	view->request_fullscreen.notify = xdg_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen,
		&view->request_fullscreen);
	view->request_show_window_menu.notify = xdg_request_show_window_menu;
	wl_signal_add(&toplevel->events.request_show_window_menu,
		&view->request_show_window_menu);
	view->set_title.notify = xdg_set_title;
	wl_signal_add(&toplevel->events.set_title, &view->set_title);
	view->set_parent.notify = xdg_set_parent;
	wl_signal_add(&toplevel->events.set_parent, &view->set_parent);
	view->set_app_id.notify = xdg_set_app_id;
	wl_signal_add(&toplevel->events.set_app_id, &view->set_app_id);
}

/* ---- popups ------------------------------------------------------------- */

struct tree_link {
	struct wlr_xdg_surface *base;
	struct wl_listener destroy;
};

static void tree_link_destroy(struct wl_listener *listener, void *data)
{
	struct tree_link *link = wl_container_of(listener, link, destroy);
	link->base->data = NULL;
	wl_list_remove(&link->destroy.link);
	free(link);
}

void xdg_surface_set_tree(struct wlr_xdg_surface *base, struct wlr_scene_tree *tree)
{
	/* The scene destroys the tree when the surface goes, so the surface is always
	 * still there for this. */
	struct tree_link *link = calloc(1, sizeof(*link));
	link->base = base;
	link->destroy.notify = tree_link_destroy;
	wl_signal_add(&tree->node.events.destroy, &link->destroy);
	base->data = tree;
}

struct popup {
	struct server *server;
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener reposition;
	struct wl_listener destroy;
};

static struct view *view_from_tree(struct wlr_scene_tree *tree)
{
	struct wlr_scene_node *n = &tree->node;
	while (n && !n->data)
		n = n->parent ? &n->parent->node : NULL;
	return n ? n->data : NULL;
}

static void popup_unconstrain(struct popup *popup)
{
	/* Keep the popup on the output its toplevel is on. */
	struct wlr_scene_tree *tree = popup->xdg_popup->base->data;
	struct view *view = tree ? view_from_tree(tree) : NULL;
	if (view && view->type == VIEW_XDG) {
		struct wlr_box a = output_usable_area(view->server,
			view->x + 1, view->y + 1);
		struct wlr_box *g = &view->xdg_toplevel->base->geometry;
		a.x -= view->x + frame_left(view) - g->x;
		a.y -= view->y + frame_top(view) - g->y;
		/* wlroots wants the box relative to the toplevel, however deep the popup
		 * is nested. */
		wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &a);
	} else if (tree) {
		/* A layer surface's popup, relative to that surface likewise. */
		int lx, ly;
		if (layer_root_coords(popup->xdg_popup->parent, &lx, &ly)) {
			struct wlr_box a = output_full_area(popup->server, lx, ly);
			a.x -= lx;
			a.y -= ly;
			wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &a);
		}
	}
}

static void popup_commit(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, commit);
	if (!popup->xdg_popup->base->initial_commit)
		return;
	popup_unconstrain(popup);
	wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
}

/* The client gave new rules, and the same limits apply. wlroots sends the configure. */
static void popup_reposition(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, reposition);
	popup_unconstrain(popup);
}

static void popup_destroy(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->reposition.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void new_xdg_popup(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_xdg_popup);
	struct wlr_xdg_popup *xdg_popup = data;
	struct popup *popup = calloc(1, sizeof(*popup));
	popup->server = server;
	popup->xdg_popup = xdg_popup;

	/* Without a parent it belongs to a layer surface, whose new_popup event
	 * follows and adds it to the scene. */
	if (xdg_popup->parent) {
		struct wlr_xdg_surface *parent =
			wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
		if (!parent || !parent->data) {
			wlr_log(WLR_ERROR, "popup without a known xdg parent");
			free(popup);
			return;
		}
		struct wlr_scene_tree *parent_tree = parent->data;
		xdg_surface_set_tree(xdg_popup->base, wlr_scene_xdg_surface_create(
			parent_tree, xdg_popup->base));
	}

	popup->commit.notify = popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->reposition.notify = popup_reposition;
	wl_signal_add(&xdg_popup->events.reposition, &popup->reposition);
	popup->destroy.notify = popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ---- decorations -------------------------------------------------------- */

/* Whether it gets a frame can change at any time, GTK3 says so only after its first
 * configure: what fills an area has to make room for the frame or take the room back. */
static void view_decoration_changed(struct view *view)
{
	view->decorated = view->ssd && !view->fullscreen;
	if (view->mapped) {
		view_update_properties(view);
		view_frame_changed(view);
	} else if (view->xdg_toplevel->base->initialized && view->maximized) {
		struct wlr_box a = output_usable_area(view->server, view->x + 1, view->y + 1);
		int w = a.width - frame_extra_w(view), h = a.height - frame_extra_h(view);
		view_apply_size_hints(view, &w, &h, false);
		view_send_size(view, w, h);
	}
}

/* We always ask for server-side decorations. */
static void deco_request_mode(struct wl_listener *listener, void *data)
{
	struct xdg_deco *deco = wl_container_of(listener, deco, request_mode);
	if (deco->wlr->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(deco->wlr,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void deco_destroy(struct wl_listener *listener, void *data)
{
	struct xdg_deco *deco = wl_container_of(listener, deco, destroy);
	if (deco->view) {
		deco->view->deco = NULL;
		deco->view->ssd = false;
		view_decoration_changed(deco->view);
	}
	wl_list_remove(&deco->request_mode.link);
	wl_list_remove(&deco->destroy.link);
	free(deco);
}

static void new_xdg_decoration(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		new_xdg_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *wlr = data;
	struct view *view = view_from_xdg_toplevel(server, wlr->toplevel);
	if (!view)
		return;

	struct xdg_deco *deco = calloc(1, sizeof(*deco));
	deco->wlr = wlr;
	deco->view = view;
	deco->request_mode.notify = deco_request_mode;
	wl_signal_add(&wlr->events.request_mode, &deco->request_mode);
	deco->destroy.notify = deco_destroy;
	wl_signal_add(&wlr->events.destroy, &deco->destroy);

	view->deco = deco;
	view->ssd = true;
	if (wlr->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(wlr,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	view_decoration_changed(view);
}

/* KDE's older protocol, still used by GTK3 to decide against CSD. */
struct kde_deco {
	struct wlr_server_decoration *wlr;
	struct server *server;
	struct wl_listener mode;
	struct wl_listener destroy;
};

static void kde_deco_mode(struct wl_listener *listener, void *data)
{
	struct kde_deco *deco = wl_container_of(listener, deco, mode);
	struct wlr_xdg_toplevel *toplevel =
		wlr_xdg_toplevel_try_from_wlr_surface(deco->wlr->surface);
	if (!toplevel)
		return;
	struct view *view = view_from_xdg_toplevel(deco->server, toplevel);
	if (!view || view->deco)
		return; /* xdg-decoration takes precedence */
	view->ssd = deco->wlr->mode ==
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER;
	view_decoration_changed(view);
}

static void kde_deco_destroy(struct wl_listener *listener, void *data)
{
	struct kde_deco *deco = wl_container_of(listener, deco, destroy);
	wl_list_remove(&deco->mode.link);
	wl_list_remove(&deco->destroy.link);
	free(deco);
}

static void new_server_decoration(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		new_server_decoration);
	struct wlr_server_decoration *wlr = data;
	struct kde_deco *deco = calloc(1, sizeof(*deco));
	deco->wlr = wlr;
	deco->server = server;
	deco->mode.notify = kde_deco_mode;
	wl_signal_add(&wlr->events.mode, &deco->mode);
	deco->destroy.notify = kde_deco_destroy;
	wl_signal_add(&wlr->events.destroy, &deco->destroy);
	kde_deco_mode(&deco->mode, NULL);
}

void xdg_init(struct server *server)
{
	server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
	server->new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel,
		&server->new_xdg_toplevel);
	server->new_xdg_popup.notify = new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup,
		&server->new_xdg_popup);

	struct wlr_xdg_decoration_manager_v1 *xdm =
		wlr_xdg_decoration_manager_v1_create(server->display);
	server->new_xdg_decoration.notify = new_xdg_decoration;
	wl_signal_add(&xdm->events.new_toplevel_decoration,
		&server->new_xdg_decoration);

	struct wlr_server_decoration_manager *sdm =
		wlr_server_decoration_manager_create(server->display);
	wlr_server_decoration_manager_set_default_mode(sdm,
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	server->new_server_decoration.notify = new_server_decoration;
	wl_signal_add(&sdm->events.new_decoration,
		&server->new_server_decoration);
}
