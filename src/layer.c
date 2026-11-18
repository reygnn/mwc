/*
 * wlr-layer-shell: panels, docks, launchers and wallpaper tools. This is the
 * Wayland counterpart of mwm-x11's _NET_WM_STRUT handling: a surface with an
 * exclusive zone shrinks the output's usable area, which maximized windows,
 * new windows, icons and menus then stay inside.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xcb/xcb_ewmh.h>
#include "mwc.h"

struct layer_surface {
	struct wl_list link; /* output.layers[] */
	struct server *server;
	struct output *output;
	struct wlr_layer_surface_v1 *wlr;
	struct wlr_scene_layer_surface_v1 *scene;
	struct wlr_scene_tree *popups; /* where the surface is, see popups_parent() */
	enum zwlr_layer_shell_v1_layer layer;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener new_popup;
	/* What update_keyboard() last acted on. Other commits are no reason to take the
	 * keyboard back from a window the user clicked on since. */
	enum zwlr_layer_surface_v1_keyboard_interactivity keyboard_interactive;
};

/* A popup is there to be seen, also if its surface is below the windows. */
static struct wlr_scene_tree *popups_parent(struct server *server,
	enum zwlr_layer_shell_v1_layer layer)
{
	return server->layer_shell[layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP ?
		ZWLR_LAYER_SHELL_V1_LAYER_TOP : layer];
}

static void arrange_pass(struct output *output, const struct wlr_box *full,
	struct wlr_box *usable, bool exclusive)
{
	for (int layer = MWC_SHELL_LAYERS - 1; layer >= 0; layer--) {
		struct layer_surface *ls;
		wl_list_for_each(ls, &output->layers[layer], link) {
			if (!ls->wlr->initialized ||
					(ls->wlr->current.exclusive_zone > 0) != exclusive)
				continue;
			wlr_scene_layer_surface_v1_configure(ls->scene, full, usable);
			wlr_scene_node_set_position(&ls->popups->node,
				ls->scene->tree->node.x, ls->scene->tree->node.y);
		}
	}
}

/* What a strut has to leave of a monitor, in either direction, to count there. */
#define MIN_ROOM 100

static bool ranges_overlap(long a0, long a1, long b0, long b1)
{
	return a0 <= b1 && b0 <= a1;
}

/* _NET_WM_STRUT_PARTIAL of X11 panels. Its right and bottom values count from
 * the far edges of the X screen, which spans all outputs. */
static void apply_struts(struct output *output, struct wlr_box *usable)
{
	struct server *server = output->server;
	struct wlr_box m, all;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &m);
	wlr_output_layout_get_box(server->output_layout, NULL, &all);
	int sw = all.x + all.width, sh = all.y + all.height;
	int left = usable->x, right = usable->x + usable->width;
	int top = usable->y, bottom = usable->y + usable->height;

	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->type != VIEW_XWAYLAND || !v->mapped || view_hidden(v) ||
				!v->xsurface->strut_partial)
			continue;
		const xcb_ewmh_wm_strut_partial_t *s = v->xsurface->strut_partial;
		/* Past the whole screen it means nothing, and the sums below would
		 * overflow. */
		if ((long)s->left > sw || (long)s->right > sw || (long)s->top > sh ||
				(long)s->bottom > sh)
			continue;
		/* A strut counts from the edge of the whole screen, so that of a panel on
		 * an inner edge reaches over the monitors before it. There it leaves too
		 * little room and means nothing, and neither does a nonsensical one. The
		 * other struts still count. */
		int edge = sw - (int)s->right;
		if ((int)s->left > m.x && ranges_overlap(s->left_start_y,
				s->left_end_y, m.y, m.y + m.height - 1) &&
				(int)s->left > left && right - (int)s->left >= MIN_ROOM)
			left = s->left;
		if (s->right && edge < m.x + m.width &&
				ranges_overlap(s->right_start_y, s->right_end_y, m.y,
				m.y + m.height - 1) && edge < right && edge - left >= MIN_ROOM)
			right = edge;
		edge = sh - (int)s->bottom;
		if ((int)s->top > m.y && ranges_overlap(s->top_start_x, s->top_end_x,
				m.x, m.x + m.width - 1) && (int)s->top > top &&
				bottom - (int)s->top >= MIN_ROOM)
			top = s->top;
		if (s->bottom && edge < m.y + m.height &&
				ranges_overlap(s->bottom_start_x, s->bottom_end_x, m.x,
				m.x + m.width - 1) && edge < bottom && edge - top >= MIN_ROOM)
			bottom = edge;
	}
	*usable = (struct wlr_box){ left, top, right - left, bottom - top };
}

/* _NET_WORKAREA, for X11 programs placing their own windows. There is one per
 * desktop rather than per output, so the first output's has to do. */
static void publish_workarea(struct server *server)
{
	/* Xwayland exits by itself when idle, and may not have started yet. */
	if (!server->xwayland || !wlr_xwayland_get_xwm_connection(server->xwayland) ||
			wl_list_empty(&server->outputs))
		return;
	/* One a client switched off has no area: the oldest one in the layout. */
	struct output *first = output_first(server);
	if (!first)
		return;
	wlr_xwayland_set_workareas(server->xwayland, &first->usable, 1);
	/* wlroots leaves it in the queue, and idle clients would never hear of it. */
	xcb_flush(wlr_xwayland_get_xwm_connection(server->xwayland));
}

void layer_publish_workarea(struct server *server)
{
	publish_workarea(server);
}

void layer_arrange(struct output *output)
{
	struct wlr_box full;
	wlr_output_layout_get_box(output->server->output_layout,
		output->wlr_output, &full);
	struct wlr_box usable = full;
	/* Surfaces that reserve space first, so the others fit around them. */
	arrange_pass(output, &full, &usable, true);
	arrange_pass(output, &full, &usable, false);
	apply_struts(output, &usable);
	struct wlr_box open = usable;
	dock_reserve(output, &usable);
	/* A panel may reserve more than there is, or all but a sliver. Then there is no
	 * use in believing it: windows would have nowhere to go. X11 struts keep the same
	 * room, see apply_struts(). */
	if (!wlr_box_intersection(&open, &open, &full) || open.width < MIN_ROOM ||
			open.height < MIN_ROOM)
		open = full;
	/* open always holds usable: given up for the dock, both are the whole output. */
	if (!wlr_box_intersection(&usable, &usable, &full) || usable.width < MIN_ROOM ||
			usable.height < MIN_ROOM)
		usable = open = full;
	struct wlr_box dock = dock_box(output);
	bool areas = !wlr_box_equal(&usable, &output->usable) ||
		!wlr_box_equal(&open, &output->open);
	bool moved = !wlr_box_equal(&dock, &output->dock);
	output->usable = usable;
	output->open = open;
	output->dock = dock;
	if (areas) {
		views_usable_area_changed(output->server);
		publish_workarea(output->server);
	} else if (moved) {
		/* A tool changing its width moves the dock within its strip: only which
		 * windows are beside it changes, not the areas, the icons or the tiling. */
		views_refit_snapped(output->server);
	}
	if (moved)
		views_clear_dock(output);
}

/* Each arrange that changes the usable area refits every maximized and snapped
 * window and the tiling layout: a panel changing its zone with every commit would
 * keep the session busy. One goes at once, then at most one per ARRANGE_MS, about a
 * refresh, with what was committed last. */
#define ARRANGE_MS 16

static void arrange_now(void *data)
{
	layer_arrange(data);
}

void layer_arrange_soon(struct output *output)
{
	output->arrange.ms = ARRANGE_MS;
	output->arrange.run = arrange_now;
	output->arrange.data = output;
	throttle_call(&output->arrange, output->server->event_loop);
}

void layers_arrange_all_soon(struct server *server)
{
	struct output *output;
	wl_list_for_each(output, &server->outputs, link)
		layer_arrange_soon(output);
}

void layers_arrange_all(struct server *server)
{
	struct output *output;
	wl_list_for_each(output, &server->outputs, link)
		layer_arrange(output);
}

/* ---- keyboard focus ------------------------------------------------------- */

/* The top-layer surface with the keyboard, or NULL. */
static struct layer_surface *top_with_keyboard(struct server *server)
{
	struct wlr_layer_surface_v1 *wlr = server->focused_layer;
	struct layer_surface *ls = wlr ? wlr->data : NULL;
	/* Where its nodes are, which layer_raise_keyboard() relies on. */
	return ls && ls->layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP ? ls : NULL;
}

struct output *layer_keyboard_output(struct server *server)
{
	struct layer_surface *ls = top_with_keyboard(server);
	return ls ? ls->output : NULL;
}

void layer_raise_keyboard(struct server *server)
{
	struct layer_surface *ls = top_with_keyboard(server);
	if (!ls)
		return;
	/* Both steps leave nodes already in place alone: no damage on each call. */
	wlr_scene_node_raise_to_top(&ls->popups->node);
	wlr_scene_node_place_below(&ls->scene->tree->node, &ls->popups->node);
}

static void give_keyboard(struct layer_surface *ls)
{
	ls->server->focused_layer = ls->wlr;
	/* A window's open menu would keep the keys, a password typed here among them. */
	input_end_client_grab(ls->server);
	iconbox_blur();
	input_keyboard_enter(ls->server, ls->wlr->surface);
	/* A focused fullscreen window would hide it: the keys would go where no one sees. */
	views_update_layers(ls->server);
	/* What lies under the pointer may have changed. */
	input_pointer_changed_soon(ls->server);
}

/* One that insists on the keyboard and hasn't got it: a topmost one, if there are more. */
static struct layer_surface *waiting_exclusive(struct server *server)
{
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
				layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP; layer--) {
			const enum zwlr_layer_surface_v1_keyboard_interactivity exclusive =
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
			struct layer_surface *ls;
			wl_list_for_each(ls, &output->layers[layer], link) {
				if (ls->wlr->surface->mapped &&
						ls->wlr->current.keyboard_interactive == exclusive)
					return ls;
			}
		}
	}
	return NULL;
}

/* Back to the window that had the focus, or to nobody. */
static void take_keyboard_back(struct server *server)
{
	struct layer_surface *waiting = waiting_exclusive(server);
	if (waiting) {
		server->exclusive_layer = waiting->wlr;
		give_keyboard(waiting);
		return;
	}
	server->focused_layer = NULL;
	server->exclusive_layer = NULL;
	views_update_layers(server);
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->activated && v->mapped) {
			view_focus(v);
			return;
		}
	}
	input_end_client_grab(server);
	wlr_seat_keyboard_notify_clear_focus(server->seat);
}

/* Only the layers above the windows may take the keyboard on their own: an exclusive
 * one whenever it asks, unless one in a higher layer holds it, as layer-shell wants the
 * top-most of them to have it. An on-demand one only as it appears; after that by a
 * click, or a surface turning on-demand on and off could pull the keys away from
 * whatever the user types into. */
static void update_keyboard(struct layer_surface *ls, bool appearing)
{
	struct server *server = ls->server;
	const struct wlr_layer_surface_v1_state *st = &ls->wlr->current;
	bool above = st->layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP ||
		st->layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	bool wants = ls->wlr->surface->mapped && above &&
		st->keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
	bool exclusive = wants && st->keyboard_interactive ==
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
	struct wlr_layer_surface_v1 *holder = server->exclusive_layer;
	bool outranks = exclusive && (!holder || holder == ls->wlr ||
		st->layer >= holder->current.layer);
	bool may = exclusive ? outranks : appearing && !holder;

	if (wants && may && server->focused_layer != ls->wlr)
		give_keyboard(ls);
	if (outranks)
		server->exclusive_layer = ls->wlr;
	else if (!exclusive && holder == ls->wlr)
		server->exclusive_layer = NULL;
	if (!wants && server->focused_layer == ls->wlr)
		take_keyboard_back(server);
}

void layer_handle_click(struct server *server, struct wlr_surface *surface)
{
	struct wlr_layer_surface_v1 *wlr = wlr_layer_surface_v1_try_from_wlr_surface(
		wlr_surface_get_root_surface(surface));
	if (!wlr || !wlr->data || server->exclusive_layer ||
			wlr->current.keyboard_interactive ==
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
		return;
	if (server->focused_layer != wlr)
		give_keyboard(wlr->data);
}

/* A click on it counts as a click on the desktop, so a wallpaper tool doesn't
 * cost the root menu. */
bool layer_surface_is_desktop(struct wlr_surface *surface)
{
	struct wlr_layer_surface_v1 *wlr = wlr_layer_surface_v1_try_from_wlr_surface(
		wlr_surface_get_root_surface(surface));
	return wlr && wlr->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
}

/* ---- surface lifecycle ---------------------------------------------------- */

bool layer_keyboard_back(struct server *server)
{
	if (!server->focused_layer)
		return false;
	give_keyboard(server->focused_layer->data);
	return true;
}

bool layer_root_coords(struct wlr_surface *surface, int *lx, int *ly)
{
	/* Up through the popups to the surface they belong to. */
	struct wlr_xdg_surface *xdg;
	while (surface && (xdg = wlr_xdg_surface_try_from_wlr_surface(surface)) &&
			xdg->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg->popup)
		surface = xdg->popup->parent;
	struct wlr_layer_surface_v1 *wlr = surface ?
		wlr_layer_surface_v1_try_from_wlr_surface(surface) : NULL;
	if (!wlr || !wlr->data)
		return false;
	struct layer_surface *ls = wlr->data;
	return wlr_scene_node_coords(&ls->scene->tree->node, lx, ly);
}

static void layer_map(struct wl_listener *listener, void *data)
{
	struct layer_surface *ls = wl_container_of(listener, ls, map);
	layer_arrange(ls->output);
	ls->keyboard_interactive = ls->wlr->current.keyboard_interactive;
	update_keyboard(ls, true);
	/* Soon: the scene gets its buffer only after this signal, and a pointer resting
	 * here would see nothing yet. */
	input_pointer_changed_soon(ls->server);
}

static void layer_unmap(struct wl_listener *listener, void *data)
{
	struct layer_surface *ls = wl_container_of(listener, ls, unmap);
	update_keyboard(ls, false);
	layer_arrange(ls->output);
	input_pointer_changed(ls->server);
}

static void place_in_layer(struct layer_surface *ls,
	enum zwlr_layer_shell_v1_layer layer)
{
	ls->layer = layer;
	wl_list_remove(&ls->link);
	wl_list_insert(&ls->output->layers[layer], &ls->link);
	wlr_scene_node_reparent(&ls->scene->tree->node,
		ls->server->layer_shell[layer]);
	wlr_scene_node_reparent(&ls->popups->node, popups_parent(ls->server, layer));
}

static void layer_commit(struct wl_listener *listener, void *data)
{
	struct layer_surface *ls = wl_container_of(listener, ls, commit);
	struct wlr_layer_surface_v1 *wlr = ls->wlr;

	if (wlr->initial_commit) {
		/* It may have changed its mind about the layer already. */
		if (wlr->pending.layer != ls->layer)
			place_in_layer(ls, wlr->pending.layer);
		/* Arrange it as if the requested state were already in force:
		 * this sends the first configure. */
		struct wlr_layer_surface_v1_state old = wlr->current;
		wlr->current = wlr->pending;
		layer_arrange(ls->output);
		wlr->current = old;
		return;
	}
	if (!wlr->current.committed)
		return;
	bool layer_changed = wlr->current.layer != ls->layer;
	if (layer_changed)
		place_in_layer(ls, wlr->current.layer);
	layer_arrange_soon(ls->output);
	if (layer_changed || wlr->current.keyboard_interactive != ls->keyboard_interactive) {
		ls->keyboard_interactive = wlr->current.keyboard_interactive;
		update_keyboard(ls, false);
	}
	/* Between top and overlay it keeps the keys, so update_keyboard() did nothing;
	 * whether a fullscreen window may cover the top panels changes all the same. */
	if (layer_changed && ls->server->focused_layer == wlr) {
		views_update_layers(ls->server);
		input_pointer_changed_soon(ls->server);
	}
}

static void layer_destroy(struct wl_listener *listener, void *data)
{
	struct layer_surface *ls = wl_container_of(listener, ls, destroy);
	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->new_popup.link);
	wl_list_remove(&ls->link);
	wlr_scene_node_destroy(&ls->popups->node);
	ls->wlr->data = NULL;
	free(ls);
}

static void layer_new_popup(struct wl_listener *listener, void *data)
{
	struct layer_surface *ls = wl_container_of(listener, ls, new_popup);
	struct wlr_xdg_popup *popup = data;
	xdg_surface_set_tree(popup->base, wlr_scene_xdg_surface_create(
		ls->popups, popup->base));
	wlr_scene_node_raise_to_top(&ls->popups->node);
}

static void new_layer_surface(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		new_layer_surface);
	struct wlr_layer_surface_v1 *wlr = data;

	struct output *output = wlr->output ? wlr->output->data :
		output_at(server, server->cursor->x, server->cursor->y);
	/* One a client switched off has no place for it. */
	if (!output || !output_in_layout(output)) {
		wlr_log(WLR_INFO, "no output for layer surface %s", wlr->namespace);
		wlr_layer_surface_v1_destroy(wlr);
		return;
	}
	wlr->output = output->wlr_output;

	struct layer_surface *ls = calloc(1, sizeof(*ls));
	if (!ls) {
		wl_resource_post_no_memory(wlr->resource);
		return;
	}
	ls->server = server;
	ls->output = output;
	ls->wlr = wlr;
	ls->layer = wlr->pending.layer;
	wlr->data = ls;
	ls->scene = wlr_scene_layer_surface_v1_create(
		server->layer_shell[ls->layer], wlr);
	ls->popups = wlr_scene_tree_create(popups_parent(server, ls->layer));
	wl_list_insert(&output->layers[ls->layer], &ls->link);

	ls->map.notify = layer_map;
	wl_signal_add(&wlr->surface->events.map, &ls->map);
	ls->unmap.notify = layer_unmap;
	wl_signal_add(&wlr->surface->events.unmap, &ls->unmap);
	ls->commit.notify = layer_commit;
	wl_signal_add(&wlr->surface->events.commit, &ls->commit);
	ls->destroy.notify = layer_destroy;
	wl_signal_add(&wlr->events.destroy, &ls->destroy);
	ls->new_popup.notify = layer_new_popup;
	wl_signal_add(&wlr->events.new_popup, &ls->new_popup);
}

/* Both ways an output goes, unplugged or switched off by a client, pass here. */
void layer_output_gone(struct output *output)
{
	dock_output_gone(output);
	/* The output may come back: its next arrange_soon() arms a new timer. */
	throttle_cancel(&output->arrange);
	for (int layer = 0; layer < MWC_SHELL_LAYERS; layer++) {
		struct layer_surface *ls, *tmp;
		wl_list_for_each_safe(ls, tmp, &output->layers[layer], link)
			wlr_layer_surface_v1_destroy(ls->wlr);
	}
}

void layer_init(struct server *server)
{
	struct wlr_layer_shell_v1 *shell = wlr_layer_shell_v1_create(
		server->display, 4);
	server->new_layer_surface.notify = new_layer_surface;
	wl_signal_add(&shell->events.new_surface, &server->new_layer_surface);
}
