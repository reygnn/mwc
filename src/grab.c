/*
 * grab.c - what the pointer does other than pointing: moving and resizing a window,
 * by a drag or from the menu and the keyboard, taking one window out of its group,
 * a frame button held down, the menu. All of struct grab is written here; input.c
 * says what the mouse and the keys did, and the others ask through these functions.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "mwc.h"

#define DRAG_THRESHOLD 4

void grab_init(struct server *server)
{
	server->grab.mode = CURSOR_PASSTHROUGH;
	server->grab.tab = -1;
}

void grab_pressed(struct server *server, uint32_t button)
{
	server->grab.button = button;
}

void grab_hold_button(struct server *server, struct view *view)
{
	server->grab.view = view;
	server->grab.mode = CURSOR_BUTTON;
}

void grab_release_button(struct server *server)
{
	server->grab.mode = CURSOR_PASSTHROUGH;
	server->grab.view = NULL;
}

void grab_menu(struct server *server, bool on)
{
	server->grab.mode = on ? CURSOR_MENU : CURSOR_PASSTHROUGH;
}

static void update_feedback(struct server *server)
{
	struct view *view = server->grab.view;
	const struct wlr_box *t = &server->grab.target;
	char buf[64];
	if (server->grab.mode == CURSOR_MOVE && server->grab.zone != SNAP_NONE) {
		snprintf(buf, sizeof(buf), "%s", snap_name(server->grab.zone));
	} else if (server->grab.mode == CURSOR_MOVE) {
		snprintf(buf, sizeof(buf), "%+d %+d", t->x, t->y);
	} else {
		snprintf(buf, sizeof(buf), "%d x %d", t->width, t->height);
	}
	feedback_show(server, buf, view->x + 1, view->y + 1);
}

static void show_outline(struct server *server)
{
	struct view *view = server->grab.view;
	const struct wlr_box *t = &server->grab.target;
	outline_show(view, (struct wlr_box){ t->x, t->y,
		t->width + frame_extra_w(view), t->height + frame_extra_h(view) });
}

static void grab_started(struct server *server)
{
	/* From the menu or keyboard there is no press to drag from yet, so show
	 * where the window is. A drag shows it on the first motion. */
	if (server->grab.outline && server->grab.until_click)
		show_outline(server);
	update_feedback(server);
}

/* The grab is over; the caller decides what becomes of the window. */
static void clear_grab(struct server *server)
{
	struct view *pressed = server->grab.mode == CURSOR_BUTTON ?
		server->grab.view : NULL;
	if (pressed) {
		pressed->pressed = PART_NONE;
		pressed->pressed_inside = false;
		frame_render(pressed);
	}
	server->grab.mode = CURSOR_PASSTHROUGH;
	server->grab.view = NULL;
	server->grab.until_click = false;
	server->grab.tab = -1;
	server->grab.take = NULL;
	server->grab.drop = NULL;
	server->cursor_name = NULL;
	server->grab.zone = SNAP_NONE;
	server->grab.resize_waiting = false;
	outline_hide();
	feedback_hide();
	snap_preview_hide();
}

void grab_forget(struct server *server)
{
	clear_grab(server);
}

static void stop_resizing_state(struct server *server)
{
	struct view *view = server->grab.view;
	if (!server->grab.outline && server->grab.mode == CURSOR_RESIZE)
		wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, false);
}

void grab_give_way(struct view *view)
{
	struct server *server = view->server;
	if (server->grab.view != view || server->grab.detaching ||
			(server->grab.mode != CURSOR_MOVE &&
			server->grab.mode != CURSOR_RESIZE))
		return;
	stop_resizing_state(server);
	clear_grab(server);
}

static void resize_to(struct view *view, const struct wlr_box *b)
{
	/* The client answers later, and its anchor keeps the opposite edge until then,
	 * also for the size applied as the grab ends. */
	view_send_size(view, b->width, b->height);
}

static void finish_take(struct server *server, struct view *view, struct view *take,
	bool moved, struct wlr_box target, uint32_t button);

void grab_end(struct server *server)
{
	struct view *view = server->grab.view;
	struct view *resized = server->grab.mode == CURSOR_RESIZE ? view : NULL;
	int tab = server->grab.detached ? -1 : server->grab.tab;
	struct view *take = server->grab.take;
	bool moved = server->grab.detached;
	enum cursor_mode mode = server->grab.mode;
	/* Opaque: the size it got to while the client was still busy. */
	bool apply = (server->grab.outline || server->grab.resize_waiting) &&
		server->grab.changed;
	bool changed = server->grab.changed;
	struct wlr_box target = server->grab.target, orig = server->grab.orig;
	enum snap zone = server->grab.zone, orig_snap = server->grab.orig_snap;
	bool orig_max = server->grab.orig_maximized;
	double px = server->cursor->x, py = server->cursor->y;
	stop_resizing_state(server);
	clear_grab(server);
	/* Put somewhere else by hand: from now on it grows from there. */
	if (changed)
		view->gravity_edges = 0;
	if (take) {
		finish_take(server, view, take, moved, target, server->grab.button);
		return;
	}
	if (tab >= 0) {
		tabs_show_nth(view, tab);
		/* The second click of a double click lands on the view now shown. */
		if (view->group && server->last_click_view == view)
			server->last_click_view = view->group->shown;
		return;
	}
	if (zone != SNAP_NONE) {
		/* Going back later means to where it was before this drag, not to
		 * the screen edge it was dropped on. */
		struct wlr_box back = orig_snap != SNAP_NONE || orig_max ? view->saved : orig;
		view->x = orig.x;
		view->y = orig.y;
		view_snap_back_to(view, zone, px, py, back);
		return;
	}
	/* A resized window is no longer snapped. */
	if (mode == CURSOR_RESIZE && changed && view->snap != SNAP_NONE)
		view_unsnap_in_place(view, target);
	if (!apply)
		return;
	if (mode == CURSOR_MOVE)
		view_set_position(view, target.x, target.y);
	else
		resize_to(view, &target);
	if (resized)
		tabs_sync_sizes(resized);
}

void grab_cancel(struct server *server)
{
	struct view *view = server->grab.view;
	enum cursor_mode mode = server->grab.mode;
	bool opaque = !server->grab.outline;
	bool was_maximized = server->grab.orig_maximized;
	enum snap was_snapped = server->grab.orig_snap;
	struct wlr_box orig = server->grab.orig;
	stop_resizing_state(server);
	clear_grab(server);
	if ((was_maximized && !view->maximized) ||
			(was_snapped != SNAP_NONE && view->snap == SNAP_NONE)) {
		/* What it restores to must survive this round trip. */
		view_snap_back_to(view, was_maximized ? SNAP_MAXIMIZE : was_snapped,
			orig.x + orig.width / 2.0, orig.y + orig.height / 2.0, view->saved);
	} else if (opaque && mode == CURSOR_MOVE) {
		view_set_position(view, orig.x, orig.y);
	} else if (opaque) {
		resize_to(view, &orig);
	}
}

/* What is under the pointer. */
static struct view *view_under_pointer(struct server *server)
{
	struct wlr_surface *surface;
	double sx, sy;
	enum frame_part part;
	uint32_t edges;
	bool on_icon;
	struct view *view = view_at(server, server->cursor->x, server->cursor->y, &surface,
		&sx, &sy, &part, &edges, &on_icon);
	return on_icon ? NULL : view;
}

/* The window that one being taken along would join if dropped here, or NULL. */
static struct view *take_target(struct server *server, struct view *take)
{
	struct view *view = view_under_pointer(server);
	return view && tabs_fit_together(take, view) ? view : NULL;
}

/* A press on a title or tab that turned out to be a click or a drag of just that window:
 * onto another window it joins that one, within its own group nothing happens, and
 * anywhere else it goes there, on its own. */
static void finish_take(struct server *server, struct view *view, struct view *take,
	bool moved, struct wlr_box target, uint32_t button)
{
	if (!moved) {
		if (button == BTN_MIDDLE)
			view_lower(view);
		else
			tabs_show(take);
		return;
	}
	struct view *drop = take_target(server, take);
	if (drop) {
		tabs_join(take, drop);
		return;
	}
	struct view *under = view_under_pointer(server);
	if (take->group && under && under->group == take->group)
		return;
	if (take->group)
		tabs_detach(take);
	view_set_position(take, target.x, target.y);
}

static void process_move(struct server *server)
{
	struct view *view = server->grab.view;
	if (!server->grab.detached) {
		/* From the menu or keyboard every step counts, down to the 1 px
		 * of Shift+arrow. A drag needs to get going first: a click on the
		 * title must leave the window alone, or a double click could
		 * never un-maximize it. */
		double t = server->grab.until_click ? 1 : DRAG_THRESHOLD;
		if (fabs(server->cursor->x - server->grab.px) < t &&
				fabs(server->cursor->y - server->grab.py) < t)
			return;
		server->grab.detached = true;
		/* Taking one window along leaves the rest where it is. Without a group
		 * there is no rest: the window itself goes. */
		bool rest_stays = server->grab.take && server->grab.take->group;
		if (!rest_stays && (view->maximized || view->snap != SNAP_NONE))
			view_drag_free(view);
	}
	/* The pointer has sub-pixel positions: round, don't truncate, or the
	 * window ends up a pixel off and jumps when crossing zero. */
	server->grab.target.x = lround(server->cursor->x - server->grab.x);
	server->grab.target.y = lround(server->cursor->y - server->grab.y);
	server->grab.changed = true;
	if (server->grab.outline)
		show_outline(server);
	else
		view_set_position(view, server->grab.target.x, server->grab.target.y);

	if (server->grab.take) {
		struct view *drop = take_target(server, server->grab.take);
		if (drop != server->grab.drop) {
			server->grab.drop = drop;
			if (drop)
				snap_preview_box(view, (struct wlr_box){
					drop->x, drop->y,
					drop->width + frame_extra_w(drop), frame_height(drop) });
			else
				snap_preview_hide();
		}
		update_feedback(server);
		return;
	}

	enum snap zone = snap_zone_at(view, server->cursor->x, server->cursor->y);
	if (zone != server->grab.zone) {
		server->grab.zone = zone;
		snap_preview(view, zone, server->cursor->x, server->cursor->y);
	}
	update_feedback(server);
}

static void process_resize(struct server *server)
{
	struct view *view = server->grab.view;
	struct wlr_box *g = &server->grab.geobox;
	uint32_t edges = server->grab.edges;
	int bx = lround(server->cursor->x - server->grab.x);
	int by = lround(server->cursor->y - server->grab.y);

	int left = g->x, right = g->x + g->width;
	int top = g->y, bottom = g->y + g->height;
	if (edges & WLR_EDGE_LEFT)
		left = bx;
	else if (edges & WLR_EDGE_RIGHT)
		right = bx;
	if (edges & WLR_EDGE_TOP)
		top = by;
	else if (edges & WLR_EDGE_BOTTOM)
		bottom = by;

	/* Never so small that the frame's buttons don't fit, unless the client
	 * insists; then what the client asks for. The opposite edge stays. */
	int cw = right - left - frame_extra_w(view);
	int ch = bottom - top - frame_extra_h(view);
	int min_cw = frame_title(view) ? 3 * theme.title_height + 24 : 16;
	if (cw < min_cw)
		cw = min_cw;
	if (ch < 8)
		ch = 8;
	view_apply_size_hints(view, &cw, &ch);
	/* The axis not dragged keeps its size. */
	if (!(edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)))
		cw = server->grab.orig.width;
	if (!(edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)))
		ch = server->grab.orig.height;
	if (edges & WLR_EDGE_LEFT)
		left = right - cw - frame_extra_w(view);
	if (edges & WLR_EDGE_TOP)
		top = bottom - ch - frame_extra_h(view);

	struct wlr_box target = { left, top, cw, ch };
	if (wlr_box_equal(&target, &server->grab.target))
		return;
	server->grab.target = target;
	server->grab.changed = true;
	if (server->grab.outline)
		show_outline(server);
	else if (view->xdg_toplevel->base->current.configure_serial < view->anchor_serial)
		/* Still busy with the last size: a mouse sends far more events than a
		 * client can draw frames. This one goes out when it has acked it. */
		server->grab.resize_waiting = true;
	else
		resize_to(view, &target);
	update_feedback(server);
}

void grab_resize_caught_up(struct view *view)
{
	struct server *server = view->server;
	if (!server->grab.resize_waiting || server->grab.view != view ||
			server->grab.mode != CURSOR_RESIZE)
		return;
	server->grab.resize_waiting = false;
	resize_to(view, &server->grab.target);
}

void view_begin_interactive(struct view *view, enum cursor_mode mode,
	uint32_t edges, bool until_click)
{
	struct server *server = view->server;
	/* A client can ask for this at any time: before it is mapped, while minimized,
	 * or during another grab. Only something visible and free can be grabbed. */
	if (!view->mapped || view_hidden(view) ||
			server->grab.mode != CURSOR_PASSTHROUGH)
		return;
	if (view->fullscreen)
		return;
	if (mode == CURSOR_RESIZE &&
			(view->maximized || !(view->funcs & FUNC_RESIZE)))
		return;
	if (mode == CURSOR_MOVE && !(view->funcs & FUNC_MOVE))
		return;
	server->grab.view = view;
	server->grab.mode = mode;
	server->grab.until_click = until_click;
	server->grab.outline = outline_wanted();
	server->grab.detached = false;
	server->grab.changed = false;
	server->grab.px = server->cursor->x;
	server->grab.py = server->cursor->y;
	/* The size last asked for: the client may not have got there yet. */
	int w = view->requested_w, h = view->requested_h;
	server->grab.orig = (struct wlr_box){ view->x, view->y, w, h };
	server->grab.orig_maximized = view->maximized;
	server->grab.orig_snap = view->snap;
	server->grab.zone = SNAP_NONE;
	server->grab.target = server->grab.orig;
	server->grab.geobox = (struct wlr_box){ view->x, view->y,
		w + frame_extra_w(view), h + frame_extra_h(view) };

	if (mode == CURSOR_MOVE) {
		server->grab.x = server->cursor->x - view->x;
		server->grab.y = server->cursor->y - view->y;
		cursor_set_name(server, "move");
	} else {
		/* The edge where it is drawn stays under the pointer, also while the
		 * client has yet to get the size last sent. */
		double bx = view->x + ((edges & WLR_EDGE_RIGHT) ?
			view->width + frame_extra_w(view) : 0);
		double by = view->y + ((edges & WLR_EDGE_BOTTOM) ?
			view->height + frame_extra_h(view) : 0);
		server->grab.x = server->cursor->x - bx;
		server->grab.y = server->cursor->y - by;
		server->grab.edges = edges;
		view->anchor_edges = edges;
		view->anchor_right = view->x + server->grab.geobox.width;
		view->anchor_bottom = view->y + server->grab.geobox.height;
		if (!server->grab.outline)
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
	server->grab.detaching = true;
	view_snap(view, SNAP_NONE, 0, 0);
	server->grab.detaching = false;
	int nw = w + frame_extra_w(view);
	view_set_position(view, server->cursor->x - frac * nw,
		server->cursor->y - frame_border(view) - frame_title(view) / 2);
	server->grab.x = server->cursor->x - view->x;
	server->grab.y = server->cursor->y - view->y;
	/* The client hasn't committed its old size yet, so don't go by view->width. */
	server->grab.target.width = w;
	server->grab.target.height = h;
	server->grab.geobox.width = nw;
	server->grab.geobox.height = h + frame_extra_h(view);
}

void grab_take_one(struct server *server, struct view *take)
{
	server->grab.take = take;
	server->grab.outline = true;
	/* The outline shows what it will be on its own. */
	int own_w, own_h;
	tabs_own_size(take, &own_w, &own_h);
	if (own_w) {
		server->grab.target.width = own_w;
		server->grab.target.height = own_h;
	}
}

void grab_click_shows_tab(struct server *server, int tab)
{
	server->grab.tab = tab;
}

void grab_motion(struct server *server)
{
	if (server->grab.mode == CURSOR_MOVE)
		process_move(server);
	else if (server->grab.mode == CURSOR_RESIZE)
		process_resize(server);
}
