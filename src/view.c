/*
 * Window management: xdg-shell toplevels and popups, focus, stacking, maximize,
 * iconify.
 */
#define _POSIX_C_SOURCE 200809L
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "mwc.h"

struct wlr_surface *view_surface(struct view *view)
{
	return view->xdg_toplevel->base->surface;
}

const char *view_title(struct view *view)
{
	const char *t = view->xdg_toplevel->title;
	return t ? t : "";
}

const char *view_app_id(struct view *view)
{
	const char *t = view->xdg_toplevel->app_id;
	return t ? t : "";
}

struct output *output_at(struct server *server, double lx, double ly)
{
	struct wlr_output *o = wlr_output_layout_output_at(server->output_layout,
		lx, ly);
	if (o && o->data)
		return o->data;
	/* Not one a client switched off: its area is empty, and windows put there
	 * would be out of reach. */
	struct output *out;
	wl_list_for_each(out, &server->outputs, link) {
		if (output_in_layout(out))
			return out;
	}
	wl_list_for_each(out, &server->outputs, link)
		return out;
	return NULL;
}

bool output_in_layout(struct output *output)
{
	return wlr_output_layout_get(output->server->output_layout, output->wlr_output);
}

struct output *output_first(struct server *server)
{
	struct output *o;
	wl_list_for_each_reverse(o, &server->outputs, link) {
		if (output_in_layout(o))
			return o;
	}
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

bool output_dock_in_the_way(struct output *o, struct wlr_box b)
{
	struct wlr_box hit;
	return o && wlr_box_intersection(&hit, &b, &o->dock);
}

static void view_place_within(struct view *view, struct wlr_box a, int x, int y);

/* The area a window at box b may use: with the dock's strip unless b reaches into
 * the dock. */
static struct wlr_box area_for(struct server *server, struct wlr_box b)
{
	double cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
	struct output *o = output_at(server, cx, cy);
	if (!o)
		return output_full_area(server, cx, cy);
	return output_dock_in_the_way(o, b) ? o->usable : o->open;
}

/* Position of the client content inside the window tree: for xdg its window geometry,
 * as wlroots' scene tree puts the surface, a shadow drawn around it, at -geometry.
 * In mwc's frame that shadow is cut off: it would lie over the frame and take its
 * clicks. */
static void view_update_content_pos(struct view *view)
{
	int x = frame_left(view), y = frame_top(view);
	struct wlr_scene_rect *backdrop = view->backdrop;
	if (view->fullscreen && view->mapped) {
		struct wlr_box a = output_full_area(view->server, view->x + 1, view->y + 1);
		wlr_scene_rect_set_size(backdrop, a.width, a.height);
		wlr_scene_node_set_position(&backdrop->node, a.x - view->x, a.y - view->y);
		wlr_scene_node_set_enabled(&backdrop->node, true);
		/* Smaller than the output: in its middle (xdg-shell). */
		if (view->width < a.width)
			x += a.x - view->x + (a.width - view->width) / 2;
		if (view->height < a.height)
			y += a.y - view->y + (a.height - view->height) / 2;
	} else {
		wlr_scene_node_set_enabled(&backdrop->node, false);
	}
	wlr_scene_node_set_position(&view->content_tree->node, x, y);
	/* Only a surface larger than its geometry draws a shadow: subsurfaces past it,
	 * as GTK's tooltips, are drawn whole. */
	struct wlr_xdg_surface *base = view->xdg_toplevel->base;
	const struct wlr_box *g = &base->geometry;
	const struct wlr_surface_state *s = &base->surface->current;
	bool shadow = g->x > 0 || g->y > 0 ||
		s->width > (long long)g->x + g->width ||
		s->height > (long long)g->y + g->height;
	/* The surface's own tree comes first; the popups after it stay whole. */
	struct wlr_scene_node *surface = wl_container_of(
		view->content_tree->children.next, surface, link);
	wlr_scene_subsurface_tree_set_clip(surface, view->ssd && shadow ? g : NULL);
}

static void view_move_to(struct view *view, int x, int y)
{
	view->x = x;
	view->y = y;
	wlr_scene_node_set_position(&view->tree->node, x, y);
}

void view_set_position(struct view *view, int x, int y)
{
	view->anchor_edges = 0;
	view_move_to(view, x, y);
}

void view_send_size(struct view *view, int w, int h)
{
	view->anchor_serial = wlr_xdg_toplevel_set_size(view->xdg_toplevel, w, h);
	view->requested_w = w;
	view->requested_h = h;
}

void view_set_geometry(struct view *view, int x, int y, int w, int h)
{
	view->anchor_edges = 0;
	view->x = x;
	view->y = y;
	wlr_scene_node_set_position(&view->tree->node, x, y);
	view_send_size(view, w, h);
}

/* Neither mwc nor the user is deciding its size and place right now. Otherwise mwc
 * does, and a group's frame belongs to all of it. */
static bool view_left_alone(struct view *view)
{
	return !view->maximized && !view->fullscreen && view->snap == SNAP_NONE &&
		!view->tiled && !view->group && view->server->grabbed_view != view;
}

void view_gravity_place(struct view *view, int ow, int oh, int w, int h,
	int *x, int *y)
{
	*x = view->x;
	*y = view->y;
	struct server *server = view->server;
	if (!view->gravity_edges || !view_left_alone(view))
		return;
	int ew = frame_extra_w(view), eh = frame_extra_h(view);
	if (view->gravity_edges & WLR_EDGE_RIGHT)
		*x = view->x + ow - w;
	if (view->gravity_edges & WLR_EDGE_BOTTOM)
		*y = view->y + oh - h;
	/* A window placed into the dock's strip beside the dock may stay there. */
	struct wlr_box a = area_for(server, (struct wlr_box){ *x, *y, w + ew, h + eh });
	/* Grown past the left or top edge: the title bar stays reachable. */
	if (*x < a.x)
		*x = a.x;
	if (*y < a.y)
		*y = a.y;
}

/* Called from the commit handlers once the client has a (new) size; own when the
 * client chose it, not mwc. */
static int size_cap(int v)
{
	return v > VIEW_MAX_SIZE ? VIEW_MAX_SIZE : v;
}

static void view_apply_size(struct view *view, int w, int h, bool own)
{
	if (w == view->width && h == view->height) {
		view_update_content_pos(view);
		return;
	}
	if (own && !view->anchor_edges) {
		int x, y;
		view_gravity_place(view, view->width, view->height, w, h, &x, &y);
		if (x != view->x || y != view->y) {
			view->x = x;
			view->y = y;
			wlr_scene_node_set_position(&view->tree->node, x, y);
		}
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
	dock_view_resized(view);
}

/* Scene layers windows can be in, bottom to top. */
enum view_layer {
	VL_NORMAL,
	VL_ABOVE,
	VL_FULLSCREEN,
};

static enum view_layer own_layer(struct view *view, struct view *focus)
{
	if (view_docked(view))
		return VL_ABOVE;
	/* A fullscreen window covers top-layer panels only while it or one of
	 * its dialogs has the focus. Otherwise a window focused beside it, or a
	 * panel, would come up in the wrong place. */
	if (view->fullscreen && focus && view_transient_root(focus) == view)
		return VL_FULLSCREEN;
	return VL_NORMAL;
}

void views_update_layers(struct server *server)
{
	struct view *focus = NULL, *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->activated)
			focus = v;
	}
	/* The window keeps the focus, but the keys go to a top-layer surface, a
	 * launcher or a prompt: it has to be seen, so on its output the window leaves
	 * the fullscreen layer for its own and stays below it (layer_raise_keyboard()).
	 * On another output it stays where it is. */
	struct output *keys = layer_keyboard_output(server);
	if (keys && focus) {
		struct view *root = view_transient_root(focus);
		if (output_at(server, root->x + root->width / 2.0,
				root->y + root->height / 2.0) == keys)
			focus = NULL;
	}
	/* Bottom first: a node goes on top of its new layer, so windows moving together,
	 * a fullscreen one and its dialog, keep their order. */
	wl_list_for_each_reverse(v, &server->views, link) {
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
			[VL_NORMAL] = server->layer_views,
			[VL_ABOVE] = server->layer_shell[2],
			[VL_FULLSCREEN] = server->layer_fullscreen,
		};
		wlr_scene_node_reparent(&v->tree->node, tree[layer]);
	}
	/* Windows above the others share the top layer with the surface that has the keys. */
	layer_raise_keyboard(server);
}

void view_raise_one(struct view *view)
{
	wlr_scene_node_raise_to_top(&view->tree->node);
	layer_raise_keyboard(view->server);
	wl_list_remove(&view->link);
	wl_list_insert(&view->server->views, &view->link);
	view->raise_seq = ++view->server->raise_hi;
}

void view_set_activated(struct view *view, bool activated)
{
	/* A window with the keyboard: the icon box doesn't have it any more. */
	if (activated)
		iconbox_blur();
	view->activated = activated;
	wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
	views_update_layers(view->server);
	frame_render(view);
}

/* Dialogs stay above the window they belong to, in their own order. */
void view_raise(struct view *view)
{
	struct server *server = view->server;
	view_raise_one(view);
	int n = 0, count = wl_list_length(&server->views);
	struct view **kids = calloc(count, sizeof(*kids));
	if (!kids)
		return;
	struct view *v;
	wl_list_for_each_reverse(v, &server->views, link) {
		if (v->mapped && view_is_transient_of(v, view))
			kids[n++] = v;
	}
	for (int i = 0; i < n; i++)
		view_raise_one(kids[i]);
	free(kids);
}

struct view *view_focused(struct server *server)
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
	/* Not from here: its desktop is marked, and the user goes there or not. */
	if (view->off_desktop) {
		desktop_attention(view);
		return;
	}
	struct server *server = view->server;
	struct wlr_seat *seat = server->seat;

	/* Asked for by name, a hidden tab comes to the front. */
	if (view->tab_hidden)
		tabs_bring(view);
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

	/* An on-demand layer surface lets go of the keys here, before the window comes
	 * up: else it would be raised back over a window above the others. */
	bool from_layer = server->focused_layer && !server->exclusive_layer;
	if (from_layer)
		server->focused_layer = NULL;
	struct view *prev = view_focused(server);
	if (prev && prev != view)
		view_set_activated(prev, false);
	if (!view->activated)
		view_set_activated(view, true);
	else if (from_layer)
		/* Already activated, it stayed below the top layer while the keys were
		 * there. */
		views_update_layers(server);
	view_raise(view);

	/* An exclusive layer surface keeps the keyboard until it goes away. */
	if (server->exclusive_layer)
		return;
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

/* Docked windows get the focus by a click only. */
static bool cyclable(struct view *view)
{
	return view_focusable(view) && !view_docked(view);
}

/* A window went away and v takes its place. An on-demand layer surface with the
 * keyboard keeps it, and its popup stays open: the user types there. v gets the keys
 * when the surface lets go (take_keyboard_back()). */
void view_focus_instead(struct view *v)
{
	struct server *server = v->server;
	if (!server->focused_layer || server->exclusive_layer || !view_focusable(v) ||
			v->off_desktop || v->tab_hidden || v->minimized ||
			v->hidden_with_parent) {
		view_focus(v);
		return;
	}
	struct view *prev = view_focused(server);
	if (prev && prev != v)
		view_set_activated(prev, false);
	if (!v->activated)
		view_set_activated(v, true);
	view_raise(v);
	input_pointer_changed(server);
}

/* Give the focus to the topmost remaining window, or to nobody. A shaded window
 * only gets it when asked for by name. gone: except went away by itself, not by the
 * user's doing here. */
static void focus_next_after(struct server *server, struct view *except, bool gone)
{
	/* Prefer the window a dialog belongs to. */
	struct view *v = except ? view_parent(except) : NULL, *next = NULL;
	if (v && v != except && cyclable(v) && !v->shaded) {
		next = v;
	} else {
		wl_list_for_each(v, &server->views, link) {
			if (v != except && cyclable(v) && !v->shaded) {
				next = v;
				break;
			}
		}
	}
	if (next) {
		if (gone)
			view_focus_instead(next);
		else
			view_focus(next);
		return;
	}
	if (except && except->activated)
		view_set_activated(except, false);
	if (!server->focused_layer) {
		input_end_client_grab(server);
		wlr_seat_keyboard_notify_clear_focus(server->seat);
	}
}

static void view_maximize_at(struct view *view, bool maximized, double px,
	double py);

/* Fill what its state says on the output at (px, py), as far as the client's
 * limits allow: all of the usable area, or a snap position in it. */
static void view_fit_state(struct view *view, double px, double py)
{
	view->fit_px = px;
	view->fit_py = py;
	view->fit_output = output_at(view->server, px, py);
	struct wlr_box a = snap_area(view->server,
		view->snap != SNAP_NONE ? view->snap : SNAP_MAXIMIZE, px, py);
	if (view->snap != SNAP_NONE)
		a = snap_rect(view->snap, a);
	int w = a.width - frame_extra_w(view), h = a.height - frame_extra_h(view);
	view_apply_size_hints(view, &w, &h);
	view_set_geometry(view, a.x, a.y, w, h);
}

/* Its centre, unless that is off the output its state fills: an output shrinking by
 * a scale or a mode moves its neighbours under windows that stay on it. */
static void view_state_point(struct view *view, double *px, double *py)
{
	*px = view->x + view->width / 2.0;
	*py = view->y + view->height / 2.0;
	struct output *o = view->fit_output;
	if (!o || !output_in_layout(o) || output_at(view->server, *px, *py) == o)
		return;
	struct wlr_box b;
	wlr_output_layout_get_box(view->server->output_layout, o->wlr_output, &b);
	*px = b.x + b.width / 2.0;
	*py = b.y + b.height / 2.0;
}

void view_refit(struct view *view)
{
	double px, py;
	view_state_point(view, &px, &py);
	view_fit_state(view, px, py);
}

void view_refit_at(struct view *view, double px, double py)
{
	view_fit_state(view, px, py);
}

/* What the client should know about it: which of its edges touch something. */
static void view_publish_snap(struct view *view)
{
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

/* A tab taking over a snapped place must hear it is, as it does when snapped itself. */
void view_publish_state(struct view *view)
{
	view_publish_snap(view);
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
	view->saved = (struct wlr_box){ view->x, view->y, view->requested_w,
		view->requested_h };
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
	focus_next_after(except->server, except, false);
}

void view_focus_top(struct server *server)
{
	focus_next_after(server, NULL, false);
}

void view_focus_top_instead(struct server *server)
{
	focus_next_after(server, NULL, true);
}

void view_close(struct view *view)
{
	wlr_xdg_toplevel_send_close(view->xdg_toplevel);
}

static void view_lower_one(struct view *view)
{
	struct server *server = view->server;
	wlr_scene_node_lower_to_bottom(&view->tree->node);
	wl_list_remove(&view->link);
	wl_list_insert(server->views.prev, &view->link);
	view->raise_seq = --server->raise_lo;
}

/* Whether anything could take the focus from it. If not it keeps it, even when it
 * goes to the back: it is still the only window there is to type into. */
static bool view_has_successor(struct view *view)
{
	struct view *v;
	wl_list_for_each(v, &view->server->views, link) {
		if (v != view && cyclable(v) && !v->shaded &&
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
		focus_next_after(server, view, false);
		/* Losing the focus may have moved it to another scene layer. */
		wlr_scene_node_lower_to_bottom(&view->tree->node);
	}
	input_pointer_changed(server);
}

void view_set_maximized(struct view *view, bool maximized)
{
	/* Not on any output yet, it goes where the pointer is, as a new window does. */
	struct wlr_cursor *c = view->server->cursor;
	view_maximize_at(view, maximized,
		view->mapped ? view->x + view->width / 2.0 : c->x,
		view->mapped ? view->y + view->height / 2.0 : c->y);
}

/* An xdg surface takes no configures before its first commit, or after a null
 * buffer started it over. Until then a state can only be noted, and goes out with
 * the first configure, see xdg_send_first_configure(). */
static bool view_configurable(struct view *view)
{
	return view->xdg_toplevel->base->initialized;
}

static void view_maximize_at(struct view *view, bool maximized, double px,
	double py)
{
	if (!view_configurable(view)) {
		view->maximized = maximized;
		return;
	}
	/* In the layout, the layout decides; see tile_promote(). A hidden tab has no
	 * state until it is shown. */
	if (view->maximized == maximized || view->fullscreen || view->tiled ||
			view->tab_hidden)
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
	view_publish_snap(view);
ack:
	/* Maximized or snapped is kept while tiled, not shown. */
	if (view->xdg_toplevel->base->initialized)
		wlr_xdg_toplevel_set_maximized(view->xdg_toplevel,
			view->maximized && !view->tiled);
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
void view_show_content(struct view *view)
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
		h = view->requested_h;
	}
	view_apply_size_hints(view, &w, &h);
	/* The main window keeps to the right edge, the stack to the left. */
	int x = view->tile_main ? r.x + r.width - (w + frame_extra_w(view)) : r.x;
	/* Every layout round comes here for every tiled window: one already where it
	 * belongs, at the size last asked of it, needs no configure and no new frame. */
	bool asked = view->requested_w == w && view->requested_h == h;
	bool in_place = asked && view->x == x && view->y == r.y;
	if (!in_place)
		view_set_geometry(view, x, r.y, w, h);
	if (view->tile_main) {
		/* Also if the client decides on a size of its own. */
		view->anchor_edges = WLR_EDGE_LEFT;
		view->anchor_right = r.x + r.width;
	}
	view_show_content(view);
	/* Rolled up or out, the frame changes height with the client's size the same. */
	if (!in_place || view->drawn_h != frame_height(view))
		frame_render(view);
}

/* The middle of the output a client asked to be fullscreen on, if it can have it.
 * xdg-shell: a preference, which mwc follows when that output is in the layout. */
static bool fullscreen_output_point(struct view *view, double *px, double *py)
{
	struct wlr_output *o = view->xdg_toplevel->requested.fullscreen_output;
	struct output *output = o ? o->data : NULL;
	if (!output || !output_in_layout(output))
		return false;
	struct wlr_box b;
	wlr_output_layout_get_box(view->server->output_layout, o, &b);
	*px = b.x + b.width / 2.0;
	*py = b.y + b.height / 2.0;
	return true;
}

static void view_fullscreen_at(struct view *view, bool fullscreen, bool asked_output,
	bool focus);

void view_set_fullscreen(struct view *view, bool fullscreen)
{
	view_fullscreen_at(view, fullscreen, false, true);
}

void view_set_fullscreen_asked(struct view *view, bool fullscreen, bool focus)
{
	view_fullscreen_at(view, fullscreen, false, focus);
}

/* asked_output: on the output the client named with its request, if any. */
static void view_fullscreen_at(struct view *view, bool fullscreen, bool asked_output,
	bool focus)
{
	if (view_docked(view))
		fullscreen = false; /* its place is the dock */
	if (!view_configurable(view)) {
		view->fullscreen = fullscreen;
		return;
	}
	if (view->tab_hidden)
		fullscreen = false; /* no state until it is shown */
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
			struct wlr_cursor *c = view->server->cursor;
			double px = view->mapped ? view->x + view->width / 2.0 : c->x;
			double py = view->mapped ? view->y + view->height / 2.0 : c->y;
			if (asked_output)
				fullscreen_output_point(view, &px, &py);
			struct wlr_box a = output_full_area(view->server, px, py);
			view->fit_output = output_at(view->server, px, py);
			view->fullscreen = true;
			views_update_layers(view->server);
			frame_render(view);
			view_set_geometry(view, a.x, a.y, a.width, a.height);
			/* At its new place: the backdrop is placed relative to it. */
			view_update_content_pos(view);
			if (focus)
				view_focus(view);
		} else {
			view->fullscreen = false;
			views_update_layers(view->server);
			view_update_content_pos(view);
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
	if (view->xdg_toplevel->base->initialized)
		wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, view->fullscreen);
}

void view_show(struct view *view, bool show)
{
	/* On another desktop it stays out of sight, whatever else it is. */
	wlr_scene_node_set_enabled(&view->tree->node, show && !view->off_desktop);
}

void view_parent_changed(struct view *view)
{
	view_update_properties(view);
	dock_view_changed(view); /* dialogs stay outside */
	views_update_layers(view->server);
	/* Hidden with a window it no longer belongs to, nothing would bring it back. */
	struct view *parent = view_parent(view);
	if (view->hidden_with_parent &&
			!(parent && view_hidden_by_state(parent))) {
		view->hidden_with_parent = false;
		view_show(view, true);
	}
	desktop_recheck(view); /* to the new window's desktop */
	tile_arrange(view->server); /* dialogs float */
}

void view_minimize(struct view *view)
{
	if (view->minimized || !view->mapped || view->tab_hidden || view_docked(view))
		return;
	struct server *server = view->server;
	grab_give_way(view);
	bool had_focus = view->activated;
	view->minimized = true;
	view_show(view, false);
	/* Dialogs go away with the window they belong to, without icons. */
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		/* Also those on another desktop with it, which hiding there doesn't count. */
		if (v->mapped && !view_hidden_by_state(v) && view_is_transient_of(v, view)) {
			had_focus |= v->activated;
			/* A dialog being moved or resized goes too: the grab ends with it. */
			grab_give_way(v);
			v->hidden_with_parent = true;
			view_show(v, false);
			if (v->activated)
				view_set_activated(v, false);
		}
	}
	icon_render(view);
	wlr_scene_node_set_enabled(&view->icon_node->node, !view->off_desktop);
	icons_arrange(server); /* the icon box decides which of its icons are in sight */
	/* The layout first: a shaded window may become the main one, and take the focus. */
	tile_arrange(server);
	if (had_focus)
		focus_next_after(server, view, true);
	input_pointer_changed(server);
}

void view_restore(struct view *view)
{
	if (view->tab_hidden) {
		view_focus(view);
		return;
	}
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
		icons_arrange(view->server); /* centred ones close up */
		view_show(view, true);
		struct view *v;
		wl_list_for_each(v, &view->server->views, link) {
			if (v->hidden_with_parent && view_is_transient_of(v, view)) {
				v->hidden_with_parent = false;
				view_show(v, true);
			}
		}
		/* Back from an icon it is the newest window of the layout. */
		view->tile_seq = --view->server->tile_front;
		tile_arrange(view->server);
		view_focus(view);
		tabs_after_restore(view);
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

bool view_app_id_changed(struct view *view)
{
	const char *id = view_app_id(view);
	if (view->known_app_id && strcmp(view->known_app_id, id) == 0)
		return false;
	free(view->known_app_id);
	view->known_app_id = strdup(id);
	return true;
}

void view_update_title(struct view *view)
{
	/* Programs that set a progress or the current line as their title may set the
	 * same one again and again. */
	const char *title = view_title(view);
	if (view->known_title && title && strcmp(view->known_title, title) == 0)
		return;
	free(view->known_title);
	view->known_title = title ? strdup(title) : NULL;
	/* A program may send thousands in a burst: at most one drawn a refresh, the last. */
	throttle_call(&view->title_redraw, view->server->event_loop);
}

static void title_redraw(void *data)
{
	struct view *view = data;
	/* The tabs of a group are all drawn by the shown view. */
	frame_render_title(view->tab_hidden ? view->group->shown : view);
	view->title_draws++;
	/* The icon's label is the title only without an app_id. */
	const char *app_id = view_app_id(view);
	if (view->minimized && (!app_id || !*app_id))
		icon_render(view);
}

/* Fill the usable area again after it, or the frame's size, has changed. */

void views_usable_area_changed(struct server *server)
{
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		v->icon_placed = false;
		/* Docked windows have no state; were one there, refitting it would
		 * change the dock, and so the usable area, without end. */
		if (v->mapped && (v->maximized || v->snap != SNAP_NONE) &&
				!v->fullscreen && !v->tiled && !view_docked(v))
			view_refit(v);
	}
	icons_arrange(server);
	tile_arrange(server);
}

void views_refit_snapped(struct server *server)
{
	struct view *v;
	wl_list_for_each(v, &server->views, link)
		if (v->mapped && v->snap != SNAP_NONE && !v->fullscreen && !v->tiled &&
				!view_docked(v))
			view_refit(v);
}

void views_clear_dock(struct output *output)
{
	struct view *v;
	wl_list_for_each(v, &output->server->views, link) {
		if (!v->mapped || v->minimized || view_docked(v) || !view_left_alone(v))
			continue;
		struct wlr_box b = { v->x, v->y,
			v->width + frame_extra_w(v), frame_height(v) };
		if (output_dock_in_the_way(output, b))
			view_place_within(v, output->usable, v->x, v->y);
	}
}

void views_output_gone(struct output *output)
{
	struct view *v;
	wl_list_for_each(v, &output->server->views, link) {
		if (v->fit_output == output)
			v->fit_output = NULL;
	}
}

/* Whether any output was added, removed, moved or resized since the last call. wlroots
 * reports a change for every configuration applied, a reload's unchanged one too. */
static bool layout_moved(struct server *server)
{
	struct wlr_box boxes[8];
	int n = 0;
	bool more = false;
	struct output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (!output_in_layout(o))
			continue;
		if (n == 8) {
			more = true;
			break;
		}
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &boxes[n++]);
	}
	bool moved = more || n != server->layout_count;
	for (int i = 0; !moved && i < n; i++)
		moved = !wlr_box_equal(&boxes[i], &server->layout_boxes[i]);
	memcpy(server->layout_boxes, boxes, sizeof(boxes));
	server->layout_count = more ? -1 : n;
	return moved;
}

void views_layout_changed(struct server *server)
{
	/* What they point at may have gone with an output. A grab keeps going if
	 * nothing moved: it would lose the size it is waiting to send. */
	bool moved = layout_moved(server);
	if (server->menu.open && moved)
		menu_close(server);
	if (server->grabbed_view && moved)
		grab_give_way(server->grabbed_view);
	if (wl_list_empty(&server->outputs))
		return;
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		v->icon_placed = false;
		if (!v->mapped || v->tab_hidden)
			continue;
		struct wlr_box box = { v->x, v->y,
			v->width + frame_extra_w(v), frame_height(v) };
		bool reachable = wlr_output_layout_intersects(server->output_layout, NULL,
			&box);
		/* output_at() falls back to an existing output for any other point. */
		struct wlr_box a = output_usable_area(server, box.x + box.width / 2.0,
			box.y + box.height / 2.0);
		/* What it goes back to may have been on that output, whether or not the window
		 * itself still was: tiling, a maximized state or fullscreen may have brought it
		 * over here. */
		struct wlr_box back = { v->saved.x, v->saved.y,
			v->saved.width + frame_extra_w(v), v->saved.height + frame_extra_h(v) };
		if (!reachable || !wlr_output_layout_intersects(server->output_layout, NULL,
				&back)) {
			v->saved.x = a.x + 32;
			v->saved.y = a.y + 32;
		}
		if (!reachable || !wlr_output_layout_contains_point(server->output_layout, NULL,
				v->fit_px, v->fit_py)) {
			v->fit_px = a.x + a.width / 2.0;
			v->fit_py = a.y + a.height / 2.0;
		}
		if (v->tiled) {
			continue;
		} else if (v->fullscreen) {
			double px = a.x + 1, py = a.y + 1;
			if (reachable)
				view_state_point(v, &px, &py);
			struct wlr_box f = output_full_area(server, px, py);
			v->fit_output = output_at(server, px, py);
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
	dock_arrange(server);
	icons_arrange(server);
	tile_arrange(server);
	input_pointer_changed(server);
}

/* Placed from the right or bottom edge by its entry, a window keeps that edge when its
 * frame grows or shrinks, as when it changes its size by itself: a program may ask for
 * a frame only once it is shown, or the title bar grows with the font. */
static void view_frame_gravity(struct view *view)
{
	/* Fullscreen, or with a size mwc decides, it has no edge to keep: that frame
	 * size doesn't count for later. */
	if (!view->gravity_edges || !view_left_alone(view))
		return;
	int ew = frame_extra_w(view), eh = frame_extra_h(view);
	int dw = ew - view->gravity_extra_w, dh = eh - view->gravity_extra_h;
	view->gravity_extra_w = ew;
	view->gravity_extra_h = eh;
	if (!dw && !dh)
		return;
	int x, y;
	view_gravity_place(view, view->width, view->height, view->width + dw,
		view->height + dh, &x, &y);
	if (x == view->x && y == view->y)
		return;
	view_move_to(view, x, y);
}

void view_frame_changed(struct view *view)
{
	/* The client keeps its size and the frame around it changes, except
	 * where the frame's size is fixed. */
	view_update_content_pos(view);
	view_frame_gravity(view);
	if ((view->maximized || view->snap != SNAP_NONE) && !view->fullscreen &&
			!view->tiled)
		view_refit(view);
	frame_render(view);
	/* In the layout, the cell stays and the client has to make room. */
	if (view->tiled)
		tile_arrange(view->server);
	dock_view_resized(view);
}

void view_theme_changed(struct view *view)
{
	config_view_shades(view);
	if (!view->mapped)
		return;
	view_frame_changed(view); /* the title bar grows with the font */
	if (view->minimized)
		icon_render(view);
	if (view->tab_hidden)
		frame_render(view->group->shown);
}

void view_cycle(struct server *server, bool backwards)
{
	struct view *top = NULL, *v;
	wl_list_for_each(v, &server->views, link) {
		if (cyclable(v)) {
			top = v;
			break;
		}
	}
	if (!top) {
		/* Every window an icon: the box is all there is to go to. */
		if (iconbox_cycle_takes(NULL, backwards))
			iconbox_focus(server);
		return;
	}
	if (backwards) {
		/* The icon box goes to the back as a window does. */
		if (iconbox_has_focus()) {
			iconbox_lowered(server);
			focus_next_after(server, NULL, false);
			return;
		}
		/* Push the current window to the back, with what belongs to it, and focus
		 * the next one. */
		struct view *active = view_focused(server);
		struct view *root = view_transient_root(active ? active : top);
		bool had_successor = view_has_successor(root);
		int count = wl_list_length(&server->views), n = 0;
		struct view **group = calloc(count, sizeof(*group));
		wl_list_for_each(v, &server->views, link) {
			if (group && v != root && view_transient_root(v) == root)
				group[n++] = v;
		}
		for (int i = 0; i < n; i++)
			view_lower_one(group[i]);
		free(group);
		view_lower_one(root);
		struct view *next = NULL;
		wl_list_for_each(v, &server->views, link) {
			if (v != root && cyclable(v) && !v->shaded) {
				next = v;
				break;
			}
		}
		if (iconbox_cycle_takes(next, true))
			iconbox_focus(server);
		else if (had_successor)
			focus_next_after(server, root, false);
		input_pointer_changed(server);
		return;
	}
	wl_list_for_each_reverse(v, &server->views, link) {
		if (cyclable(v)) {
			if (iconbox_cycle_takes(v, false))
				iconbox_focus(server);
			else
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
	if (!view->mapped || view_hidden(view) ||
			server->cursor_mode != CURSOR_PASSTHROUGH)
		return;
	if (view->fullscreen)
		return;
	/* The client's limits may have changed since it was mapped. */
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
	int w = view->requested_w, h = view->requested_h;
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
		/* The edge where it is drawn stays under the pointer, also while the
		 * client has yet to get the size last sent. */
		double bx = view->x + ((edges & WLR_EDGE_RIGHT) ?
			view->width + frame_extra_w(view) : 0);
		double by = view->y + ((edges & WLR_EDGE_BOTTOM) ?
			view->height + frame_extra_h(view) : 0);
		server->grab_x = server->cursor->x - bx;
		server->grab_y = server->cursor->y - by;
		server->resize_edges = edges;
		view->anchor_edges = edges;
		view->anchor_right = view->x + server->grab_geobox.width;
		view->anchor_bottom = view->y + server->grab_geobox.height;
		if (!server->grab_outline)
			wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, true);
		view->anchor_serial = view->xdg_toplevel->base->scheduled_serial;
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
	bool frame = node == &view->frame_fill->node;
	for (int i = 0; i < 4; i++)
		frame |= node == &view->frame_nodes[i]->node;
	*part = frame ? frame_hit(view, (int)(lx - view->x),
		(int)(ly - view->y), edges) : PART_CLIENT;
	return view;
}

/* ---- common view lifecycle -------------------------------------------- */

void view_init_common(struct view *view)
{
	struct server *server = view->server;
	view->title_redraw = (struct throttle){ .ms = 16, .run = title_redraw, .data = view };
	view->tree = wlr_scene_tree_create(server->layer_views);
	view->tree->node.data = view;
	view->backdrop = wlr_scene_rect_create(view->tree, 0, 0,
		(const float[4]){ 0, 0, 0, 1 });
	wlr_scene_node_set_enabled(&view->backdrop->node, false);
	view->frame_fill = wlr_scene_rect_create(view->tree, 0, 0,
		(const float[4]){ 0, 0, 0, 1 });
	wlr_scene_node_set_enabled(&view->frame_fill->node, false);
	for (int i = 0; i < 4; i++)
		view->frame_nodes[i] = wlr_scene_buffer_create(view->tree, NULL);
	view->icon_node = wlr_scene_buffer_create(server->layer_icons, NULL);
	view->icon_node->node.data = view;
	wlr_scene_node_set_enabled(&view->icon_node->node, false);
	wlr_scene_node_set_enabled(&view->tree->node, false);
	wl_list_insert(&server->views, &view->link);
	view->raise_seq = ++server->raise_hi;
	view->shade_active = theme.active;
	view->shade_inactive = theme.inactive;
	view->decor = DECOR_ALL;
	view->funcs = FUNC_ALL;
	view->dock_order = -1;
}

/* Inside a, keeping at least the title bar reachable. */
static struct wlr_box place_in(struct wlr_box a, int x, int y, int w, int h)
{
	if (x + w > a.x + a.width)
		x = a.x + a.width - w;
	if (y + h > a.y + a.height)
		y = a.y + a.height - h;
	if (x < a.x)
		x = a.x;
	if (y < a.y)
		y = a.y;
	return (struct wlr_box){ x, y, w, h };
}

/* Inside the usable area a; beside the dock, its strip may be used as well. */
static void view_place_within(struct view *view, struct wlr_box a, int x, int y)
{
	/* The size last sent: the one it is placed for. */
	int w = view->requested_w + frame_extra_w(view);
	int h = view->requested_h + frame_extra_h(view);
	struct output *o = output_at(view->server, a.x + a.width / 2.0,
		a.y + a.height / 2.0);
	struct wlr_box b;
	if (o && wlr_box_equal(&a, &o->usable)) {
		b = place_in(o->open, x, y, w, h);
		if (!output_dock_in_the_way(o, b)) {
			view_set_position(view, b.x, b.y);
			return;
		}
	}
	b = place_in(a, x, y, w, h);
	view_set_position(view, b.x, b.y);
}

/* New windows cascade from the top-left of the output under the pointer. A
 * dialog goes over the window it belongs to, or to the centre. */
void view_place(struct view *view)
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
	} else if (view->wtype == WT_DIALOG) {
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
	view_place_within(view, a, x, y);
}

/* A window that appears goes where its mwmrc entry says, if it has one. A tab taken
 * out of its group doesn't come through here. */
static void view_place_mapped(struct view *view)
{
	struct config_position pos;
	if (!config_view_position(view, &pos)) {
		view_place(view);
		return;
	}
	if (!pos.at_pointer)
		view->gravity_edges = (pos.from_right ? WLR_EDGE_RIGHT : 0) |
			(pos.from_bottom ? WLR_EDGE_BOTTOM : 0);
	view->gravity_extra_w = frame_extra_w(view);
	view->gravity_extra_h = frame_extra_h(view);
	struct wlr_box a = output_usable_area(view->server, view->server->cursor->x,
		view->server->cursor->y);
	int w = view->width + frame_extra_w(view);
	int h = view->height + frame_extra_h(view);
	if (pos.at_pointer) {
		/* The middle of the title under the pixel the hotspot is on, as the state
		 * file reports it: a click there doesn't resize. */
		int cx, cy;
		frame_title_center(view, &cx, &cy);
		view_place_within(view, a, (int)floor(view->server->cursor->x) - cx,
			(int)floor(view->server->cursor->y) - cy);
		return;
	}
	/* Counted from the output's edge beside the dock, from its strip over it. */
	struct output *o = output_at(view->server, view->server->cursor->x,
		view->server->cursor->y);
	if (o) {
		struct wlr_box b = { pos.from_right ? o->open.x + o->open.width - w - pos.x :
			o->open.x + pos.x, pos.from_bottom ? o->open.y + o->open.height - h -
			pos.y : o->open.y + pos.y, w, h };
		if (!output_dock_in_the_way(o, b))
			a = o->open;
	}
	view_place_within(view, a,
		pos.from_right ? a.x + a.width - w - pos.x : a.x + pos.x,
		pos.from_bottom ? a.y + a.height - h - pos.y : a.y + pos.y);
}

void view_handle_map(struct view *view, bool focus)
{
	view->mapped = true;
	/* view_place_mapped() sets it from the entry; until then a frame that changes
	 * moves nothing. */
	view->gravity_edges = 0;
	config_view_shades(view); /* the class or app_id is known by now */
	view->tile_seq = --view->server->tile_front; /* a new window leads the layout */
	view_update_properties(view); /* the window type, for desktop_view_mapped() */
	/* Docking needs the type, the functions need docking. */
	view->dock_order = config_view_dock(view);
	view_update_decorations(view);
	config_view_tiling(view);
	view->all_desktops = config_view_all_desktops(view);
	view_app_id_changed(view); /* the one the rules above are for */
	desktop_view_mapped(view);
	wlr_scene_node_set_enabled(&view->tree->node, !view->off_desktop);
	view_update_content_pos(view);
	/* A prompt must not stay out of sight in a hidden tab, unless the group is an icon:
	 * then it waits, as any dialog of a window that is an icon by now. */
	struct view *root = view_transient_root(view);
	if (root != view && root->tab_hidden && !root->group->shown->minimized) {
		if (focus)
			tabs_bring(root);
		else
			tabs_bring_instead(root);
	}
	struct view *parent = view_parent(view);
	/* Hidden with it, not for being on another desktop: it goes there too, and
	 * shows when that one does. */
	bool hide = parent && view_hidden_by_state(parent);
	if (view_docked(view)) {
		dock_admit(view); /* a state asked for before it was mapped */
		dock_arrange(view->server);
	} else if (view->placed)
		view_set_position(view, view->x, view->y);
	else if (!view->maximized && !view->fullscreen && view->snap == SNAP_NONE)
		view_place_mapped(view);
	frame_render(view);
	views_update_layers(view->server);
	if (hide) {
		view->hidden_with_parent = true;
		view_show(view, false);
	}
	/* Into the layout before it gets the focus, so it doesn't jump. */
	tile_arrange(view->server);
	if (hide)
		return;
	/* A dialog of a window on another desktop: there, and that desktop is marked. */
	if (view->off_desktop) {
		desktop_attention(view);
		return;
	}
	/* In sight, the keyboard where it is: the user decides. A dialog of the window
	 * with the focus has it next, when the panel above lets go. */
	if (!focus) {
		struct view *focused = view_focused(view->server);
		if (root != view && focused && view_transient_root(focused) == root &&
				!view_docked(view))
			view_focus_instead(view);
		else
			view_raise(view);
		input_pointer_changed(view->server);
		return;
	}
	if (!view_docked(view)) {
		view_focus(view);
	} else {
		view_raise(view);
		input_pointer_changed(view->server);
	}
}

void view_handle_unmap(struct view *view)
{
	struct server *server = view->server;
	if (server->grabbed_view == view || server->grab_take == view)
		grab_forget(server);
	if (server->menu.open && server->menu.target == view)
		menu_close(server);
	if (server->last_click_view == view)
		server->last_click_view = NULL;
	/* The map draws the title there is then: whatever comes after it is news. */
	free(view->known_title);
	view->known_title = NULL;
	tabs_view_gone(view);
	/* Dialogs hidden with it have nothing left to come back with. */
	struct view *v, *shown = NULL;
	wl_list_for_each(v, &server->views, link) {
		if (v->hidden_with_parent && view_is_transient_of(v, view)) {
			v->hidden_with_parent = false;
			view_show(v, true);
			shown = v;
		}
	}
	/* Nor from a panel or the icon box that has the keys. */
	if (shown && !view_focused(server) && !server->focused_layer &&
			!iconbox_has_focus())
		view_focus(shown);
	bool was_active = view->activated, was_icon = view->minimized;
	view->mapped = false;
	view->activated = false;
	view->minimized = false;
	view->hidden_with_parent = false;
	view->off_desktop = false;
	bool was_urgent = view->attention;
	view->attention = false;
	view->tiled = false;
	view->tile_main = false;
	view->tile_back = false;
	view->shaded = false;
	view_show_content(view);
	wlr_scene_node_set_enabled(&view->tree->node, false);
	wlr_scene_node_set_enabled(&view->icon_node->node, false);
	if (was_icon)
		icons_arrange(server); /* centred ones close up */
	if (view_docked(view)) {
		view->dock_order = -1; /* decided anew at the next map */
		dock_arrange(server); /* the others close up */
	}
	/* The layout first: a shaded window may become the main one, and take the focus. */
	tile_arrange(server);
	if (was_active)
		focus_next_after(server, view, true);
	if (was_urgent)
		workspace_update(server); /* its desktop may have nothing left to show */
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
	throttle_cancel(&view->title_redraw);
	wlr_scene_node_destroy(&view->icon_node->node);
	wlr_scene_node_destroy(&view->tree->node);
	free(view->known_title);
	free(view->known_app_id);
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
		if (v->xdg_toplevel == toplevel)
			return v;
	}
	return NULL;
}

/* The state noted so far goes out with the first configure, and with it the size
 * that belongs to it. Otherwise the client chooses. */
/* Clients hide what isn't offered: no maximize button on a dialog mwc won't maximize.
 * Changing them sends a configure, so only on a change. */
void xdg_publish_caps(struct view *view)
{
	/* Before the first configure there is nothing to send them with: that one does. */
	if (!view->xdg_toplevel->base->initialized)
		return;
	uint32_t caps = WLR_XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU |
		WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN;
	if (view->funcs & FUNC_MAXIMIZE)
		caps |= WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE;
	if (view->funcs & FUNC_MINIMIZE)
		caps |= WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE;
	if (caps == view->wm_caps)
		return;
	view->wm_caps = caps;
	wlr_xdg_toplevel_set_wm_capabilities(view->xdg_toplevel, caps);
}

static void xdg_send_first_configure(struct view *view)
{
	struct server *server = view->server;
	struct wlr_xdg_toplevel *top = view->xdg_toplevel;
	view->wm_caps = 0; /* sent again with this configure */
	view_update_properties(view);
	view->placed = false;
	/* wlroots keeps what was last scheduled across a null buffer, and the client would
	 * start over in the states of its last life. */
	wlr_xdg_toplevel_set_maximized(top, view->maximized);
	wlr_xdg_toplevel_set_fullscreen(top, view->fullscreen);
	wlr_xdg_toplevel_set_tiled(top, WLR_EDGE_NONE);
	wlr_xdg_toplevel_set_resizing(top, false);
	wlr_xdg_toplevel_set_suspended(top, false);
	/* False until view_focus(): a window mapped without the focus would draw itself
	 * focused. */
	wlr_xdg_toplevel_set_activated(top, view->activated);
	/* The output it goes back to, leaving a state. */
	view->fit_px = server->cursor->x;
	view->fit_py = server->cursor->y;
	if (view->fullscreen) {
		double px = server->cursor->x, py = server->cursor->y;
		fullscreen_output_point(view, &px, &py);
		struct wlr_box a = output_full_area(server, px, py);
		view->fit_output = output_at(server, px, py);
		view->x = a.x;
		view->y = a.y;
		view->placed = true;
		view_send_size(view, a.width, a.height);
	} else if (view->maximized) {
		struct wlr_box a = output_usable_area(server, server->cursor->x,
			server->cursor->y);
		int w = a.width - frame_extra_w(view), h = a.height - frame_extra_h(view);
		view_apply_size_hints(view, &w, &h);
		view->x = a.x;
		view->y = a.y;
		view->placed = true;
		view_send_size(view, w, h);
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
		struct xdg_deco *deco = view->deco;
		if (deco)
			wlr_xdg_toplevel_decoration_v1_set_mode(deco->wlr,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		xdg_send_first_configure(view);
		return;
	}
	if (!view->mapped)
		return;
	/* Caught up, and still not the size asked for: its own choice. */
	int w = size_cap(base->geometry.width), h = size_cap(base->geometry.height);
	bool own = base->current.configure_serial >= view->anchor_serial &&
		(w != view->requested_w || h != view->requested_h);
	view_apply_size(view, w, h, own);
	/* wlroots repeats the size mwc asked for last with every configure, a change of
	 * focus too, and an obedient client would shrink back to it. A size it chose
	 * itself is the one to repeat; 0 already leaves the choice to it. */
	struct wlr_xdg_toplevel *t = view->xdg_toplevel;
	if (own && view_left_alone(view) && (t->scheduled.width || t->scheduled.height) &&
			(t->scheduled.width != w || t->scheduled.height != h)) {
		wlr_xdg_toplevel_set_size(t, w, h);
		view->requested_w = w;
		view->requested_h = h;
	}
	/* Caught up: from now on what it committed is what counts, even if that
	 * isn't quite what it was asked for. */
	if (base->current.configure_serial >= view->anchor_serial) {
		view->requested_w = view->width;
		view->requested_h = view->height;
	}
	/* Its limits may have changed. */
	view_update_properties(view);
	tabs_sync_sizes(view);

	if (base->current.configure_serial >= view->anchor_serial)
		grab_resize_caught_up(view);
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
	view->width = view->requested_w = size_cap(g->width);
	view->height = view->requested_h = size_cap(g->height);
	view_handle_map(view, true);
}

static void xdg_unmap(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, unmap);
	view_handle_unmap(view);
	/* Unmapped, the surface starts over, and so do its states. */
	view->maximized = false;
	view->fullscreen = false;
	view->snap = SNAP_NONE;
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
	view_parent_changed(view);
}

static void xdg_set_app_id(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_app_id);
	if (!view->mapped || !view_app_id_changed(view))
		return; /* the map picks it up; or the same again */
	config_view_shades(view);
	config_view_tiling(view);
	view->all_desktops = config_view_all_desktops(view);
	desktop_recheck(view);
	view_theme_changed(view);
	dock_view_changed(view);
	tile_arrange(view->server);
}

static void xdg_request_move(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_move);
	struct wlr_xdg_toplevel_move_event *event = data;
	/* Only for a button still down: a late request would glue the window to the
	 * pointer until the next click. */
	if (wlr_seat_validate_pointer_grab_serial(view->server->seat, NULL,
			event->serial) && input_pointer_on(view->server, view_surface(view)))
		view_begin_interactive(view, CURSOR_MOVE, 0, false);
}

static void xdg_request_resize(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct view *view = wl_container_of(listener, view, request_resize);
	if (wlr_seat_validate_pointer_grab_serial(view->server->seat, NULL,
			event->serial) && input_pointer_on(view->server, view_surface(view)))
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
	view_fullscreen_at(view, view->xdg_toplevel->requested.fullscreen, true, true);
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
	/* Surface-local, so from the window geometry's origin, which the scene has with
	 * the frame and the centering of a small fullscreen window, less the geometry's
	 * offset in the surface (a shadow). Within the window geometry: the client may
	 * say any int32, the offset too. */
	struct wlr_box *g = &view->xdg_toplevel->base->geometry;
	int ox, oy;
	wlr_scene_node_coords(&view->content_tree->node, &ox, &oy);
	long long x = (long long)event->x - g->x, y = (long long)event->y - g->y;
	x = x < 0 ? 0 : x > view->width ? view->width : x;
	y = y < 0 ? 0 : y > view->height ? view->height : y;
	menu_open_window(view->server, view, ox + (int)x, oy + (int)y);
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
	if (!view) {
		wl_resource_post_no_memory(toplevel->resource);
		return;
	}
	view->server = server;
	view->xdg_toplevel = toplevel;
	view_init_common(view);
	/* KDE's protocol goes by the surface, which may ask before it has this role. */
	struct wlr_server_decoration *kde;
	wl_list_for_each(kde, &server->kde_decorations->decorations, link) {
		if (kde->surface == toplevel->base->surface)
			view->ssd = kde->mode == WLR_SERVER_DECORATION_MANAGER_MODE_SERVER;
	}
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

struct view *view_from_surface(struct wlr_surface *surface)
{
	/* Popups up to their toplevel; a client sets how deep, hence the bound. */
	for (int i = 0; surface && i < 16; i++) {
		surface = wlr_surface_get_root_surface(surface);
		struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
		if (xdg && xdg->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg->popup) {
			surface = xdg->popup->parent;
			continue;
		}
		if (xdg) {
			/* Its tree hangs in its view's, see view_parent(). */
			struct wlr_scene_tree *tree = xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL ?
				xdg->data : NULL;
			return tree && tree->node.parent ? tree->node.parent->node.data : NULL;
		}
		return NULL;
	}
	return NULL;
}

void xdg_surface_set_tree(struct wlr_xdg_surface *base, struct wlr_scene_tree *tree)
{
	/* The scene destroys the tree when the surface goes, so the surface is always
	 * still there for this. */
	struct tree_link *link = calloc(1, sizeof(*link));
	if (!link) {
		/* Without the link, data would outlive the tree. Its popups go without. */
		wl_resource_post_no_memory(base->resource);
		return;
	}
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
	struct wl_listener unmap;
	struct wl_listener destroy;
};

static struct view *view_from_tree(struct wlr_scene_tree *tree)
{
	struct wlr_scene_node *n = &tree->node;
	while (n && !n->data)
		n = n->parent ? &n->parent->node : NULL;
	return n ? n->data : NULL;
}

static int clamp_int(long long v)
{
	return v < INT_MIN ? INT_MIN : v > INT_MAX ? INT_MAX : (int)v;
}

static void popup_unconstrain(struct popup *popup)
{
	struct wlr_scene_tree *tree = popup->xdg_popup->base->data;
	struct view *view = tree ? view_from_tree(tree) : NULL;
	if (view) {
		/* On the output where it opens from, its anchor: for a window across two
		 * monitors, a menu opened on the right one stays there. */
		struct wlr_xdg_popup *xp = popup->xdg_popup;
		struct wlr_box ar = xp->scheduled.rules.anchor_rect;
		int ax = clamp_int((long long)ar.x + ar.width / 2);
		int ay = clamp_int((long long)ar.y + ar.height / 2);
		/* The anchor is in the parent's window geometry. wlroots adds the parent
		 * popups' positions up to the toplevel's surface: its origin is the window
		 * geometry's in the scene, with a small fullscreen window's centering, less
		 * the geometry's offset in the surface. */
		struct wlr_box *g = &view->xdg_toplevel->base->geometry;
		int tx, ty, ox, oy;
		wlr_xdg_popup_get_toplevel_coords(xp, ax, ay, &tx, &ty);
		wlr_scene_node_coords(&view->content_tree->node, &ox, &oy);
		long long sx = (long long)ox - g->x, sy = (long long)oy - g->y;
		double lx = (double)sx + tx;
		double ly = (double)sy + ty;
		/* Off every monitor, the window's corner decides, as before. */
		if (!wlr_output_layout_output_at(view->server->output_layout, lx, ly)) {
			lx = view->x + 1;
			ly = view->y + 1;
		}
		/* Beside the dock as far as its window may go. */
		struct wlr_box a = output_usable_area(view->server, lx, ly);
		struct output *o = output_at(view->server, lx, ly);
		struct wlr_box vb = { view->x, view->y, view->width + frame_extra_w(view),
			frame_height(view) };
		if (o && !output_dock_in_the_way(o, vb))
			a = o->open;
		a.x = clamp_int((long long)a.x - sx);
		a.y = clamp_int((long long)a.y - sy);
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

/* A menu closing under a pointer that doesn't move: what is below has to get the pointer
 * now, or the wheel goes nowhere until the next motion. */
static void popup_unmap(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, unmap);
	input_pointer_changed_soon(popup->server);
}

static void popup_destroy(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->reposition.link);
	wl_list_remove(&popup->unmap.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void new_xdg_popup(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_xdg_popup);
	struct wlr_xdg_popup *xdg_popup = data;
	struct popup *popup = calloc(1, sizeof(*popup));
	if (!popup) {
		wl_resource_post_no_memory(xdg_popup->resource);
		return;
	}
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
	popup->unmap.notify = popup_unmap;
	wl_signal_add(&xdg_popup->base->surface->events.unmap, &popup->unmap);
	popup->destroy.notify = popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ---- decorations -------------------------------------------------------- */

/* Whether it gets a frame can change at any time, GTK3 says so only after its first
 * configure: what fills an area has to make room for the frame or take the room back. */
static void view_decoration_changed(struct view *view)
{
	if (view->mapped) {
		view_update_properties(view);
		view_frame_changed(view);
	} else if (view->xdg_toplevel->base->initialized && view->maximized) {
		struct wlr_box a = output_usable_area(view->server, view->x + 1, view->y + 1);
		int w = a.width - frame_extra_w(view), h = a.height - frame_extra_h(view);
		view_apply_size_hints(view, &w, &h);
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
	if (!deco) {
		wl_resource_post_no_memory(wlr->resource);
		return;
	}
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
	if (!deco) {
		wl_resource_post_no_memory(wlr->resource);
		return;
	}
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
	server->kde_decorations = sdm;
	wlr_server_decoration_manager_set_default_mode(sdm,
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	server->new_server_decoration.notify = new_server_decoration;
	wl_signal_add(&sdm->events.new_decoration,
		&server->new_server_decoration);
}
