/*
 * placement.c - what decides a window's size and place, and every change of it: free,
 * snapped to a half or quarter, maximized, a cell of the tiling layout, fullscreen;
 * what it goes back to; and filling its area again when outputs or panels change.
 *
 * The state has two layers. A window floats free, snapped or maximized (snap,
 * maximized: never both), and over that it may be tiled or fullscreen (never both),
 * which keeps the floating state for later. view_placement() in mwc.h says which one
 * rules. These fields, saved, fit_px, fit_py, fit_output and tile_back are written in
 * this file only: whoever else changes a window's state calls one of these functions.
 */
#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "mwc.h"

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

/* What the client should know about it: which of its edges touch something. A tab
 * taking over a snapped place hears it too. */
void view_publish_state(struct view *view)
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
	view_publish_state(view);
	frame_render(view);
}

/* As view_snap(), but what it goes back to later is back, not where it is now: a drag
 * to an edge ends where it began, a cancelled one keeps what it had. */
void view_snap_back_to(struct view *view, enum snap s, double px, double py,
	struct wlr_box back)
{
	view_snap(view, s, px, py);
	view->saved = back;
}

/* Resized by hand, a snapped window is free where it is, at the size asked of it. */
void view_unsnap_in_place(struct view *view, struct wlr_box now)
{
	view->saved = now;
	view->snap = SNAP_NONE;
	wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, WLR_EDGE_NONE);
}

void view_remember_size(struct view *view, int w, int h)
{
	view->saved.width = w;
	view->saved.height = h;
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
	view_publish_state(view);
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
	view_publish_state(view);
	if (!on && apply)
		view_apply_floating(view);
	frame_render(view);
}

void view_drop_tiled(struct view *view)
{
	view->tiled = false;
}

void view_untile_unmapped(struct view *view)
{
	view->tiled = false;
	view->tile_back = false;
}

void view_reset_state(struct view *view)
{
	view->maximized = false;
	view->fullscreen = false;
	view->snap = SNAP_NONE;
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

void view_set_fullscreen(struct view *view, bool fullscreen)
{
	view_fullscreen_at(view, fullscreen, false);
}

void view_fullscreen_at(struct view *view, bool fullscreen, bool asked_output)
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

/* The first configure: a state noted before it goes out now, with the size that
 * belongs to it and the place; otherwise the client chooses its size. */
void view_send_first_state(struct view *view)
{
	struct server *server = view->server;
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

/*
 * Everything that makes up a window's place on the screen goes from one tab to the
 * next. A new field of struct view that belongs to the place rather than the client
 * has to be added here.
 */
void view_hand_over_place(struct view *to, struct view *from)
{
	to->x = from->x;
	to->y = from->y;
	to->saved = from->saved;
	to->fit_px = from->fit_px;
	to->fit_py = from->fit_py;
	to->fit_output = from->fit_output;
	to->maximized = from->maximized;
	to->snap = from->snap;
	to->tiled = from->tiled;
	to->tile = from->tile;
	to->tile_main = from->tile_main;
	to->tile_seq = from->tile_seq;
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

/* Fill the usable area again after it, or the frame's size, has changed. */
void views_usable_area_changed(struct server *server)
{
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		v->icon_placed = false;
		/* Docked windows have no state; were one there, refitting it would
		 * change the dock, and so the usable area, without end. */
		if (v->mapped && view_fills_area(v) && !view_docked(v))
			view_refit(v);
	}
	icons_arrange(server);
	tile_arrange(server);
}

void views_refit_snapped(struct server *server)
{
	struct view *v;
	wl_list_for_each(v, &server->views, link)
		if (v->mapped && view_placement(v) == PLACE_SNAPPED && !view_docked(v))
			view_refit(v);
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
	if (server->grab.view && moved)
		grab_give_way(server->grab.view);
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
		switch (view_placement(v)) {
		case PLACE_TILED:
			break; /* tile_arrange() below */
		case PLACE_FULLSCREEN: {
			double px = a.x + 1, py = a.y + 1;
			if (reachable)
				view_state_point(v, &px, &py);
			struct wlr_box f = output_full_area(server, px, py);
			v->fit_output = output_at(server, px, py);
			view_set_geometry(v, f.x, f.y, f.width, f.height);
			break;
		}
		case PLACE_SNAPPED:
		case PLACE_MAXIMIZED:
			if (!reachable)
				view_fit_state(v, v->fit_px, v->fit_py);
			else
				view_refit(v);
			break;
		case PLACE_FREE:
			if (!reachable)
				view_set_position(v, a.x + 32, a.y + 32);
			break;
		}
	}
	dock_arrange(server);
	icons_arrange(server);
	tile_arrange(server);
	input_pointer_changed(server);
}
