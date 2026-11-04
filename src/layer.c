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
#include "mwc.h"

struct layer_surface {
	struct wl_list link; /* output.layers[] */
	struct server *server;
	struct output *output;
	struct wlr_layer_surface_v1 *wlr;
	struct wlr_scene_layer_surface_v1 *scene;
	enum zwlr_layer_shell_v1_layer layer;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener new_popup;
};

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
		}
	}
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
	if (!wlr_box_equal(&usable, &output->usable)) {
		output->usable = usable;
		views_usable_area_changed(output->server);
	}
}

/* ---- keyboard focus ------------------------------------------------------- */

static void give_keyboard(struct layer_surface *ls)
{
	struct wlr_seat *seat = ls->server->seat;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	ls->server->focused_layer = ls->wlr;
	wlr_seat_keyboard_notify_enter(seat, ls->wlr->surface,
		kb ? kb->keycodes : NULL, kb ? kb->num_keycodes : 0,
		kb ? &kb->modifiers : NULL);
}

/* Back to the window that had the focus, or to nobody. */
static void take_keyboard_back(struct server *server)
{
	server->focused_layer = NULL;
	server->exclusive_layer = NULL;
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->activated && v->mapped) {
			view_focus(v);
			return;
		}
	}
	wlr_seat_keyboard_notify_clear_focus(server->seat);
}

/* Only the layers above the windows may take the keyboard on their own. */
static void update_keyboard(struct layer_surface *ls)
{
	struct server *server = ls->server;
	const struct wlr_layer_surface_v1_state *st = &ls->wlr->current;
	bool above = st->layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP ||
		st->layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	bool wants = ls->wlr->surface->mapped && above &&
		st->keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
	bool exclusive = wants && st->keyboard_interactive ==
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;

	if (wants && server->focused_layer != ls->wlr &&
			(!server->exclusive_layer || exclusive))
		give_keyboard(ls);
	if (exclusive)
		server->exclusive_layer = ls->wlr;
	else if (server->exclusive_layer == ls->wlr)
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

static void layer_map(struct wl_listener *listener, void *data)
{
	struct layer_surface *ls = wl_container_of(listener, ls, map);
	layer_arrange(ls->output);
	update_keyboard(ls);
}

static void layer_unmap(struct wl_listener *listener, void *data)
{
	struct layer_surface *ls = wl_container_of(listener, ls, unmap);
	update_keyboard(ls);
	layer_arrange(ls->output);
}

static void place_in_layer(struct layer_surface *ls,
	enum zwlr_layer_shell_v1_layer layer)
{
	ls->layer = layer;
	wl_list_remove(&ls->link);
	wl_list_insert(&ls->output->layers[layer], &ls->link);
	wlr_scene_node_reparent(&ls->scene->tree->node,
		ls->server->layer_shell[layer]);
}

static void layer_commit(struct wl_listener *listener, void *data)
{
	struct layer_surface *ls = wl_container_of(listener, ls, commit);
	struct wlr_layer_surface_v1 *wlr = ls->wlr;

	if (wlr->initial_commit) {
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
	if (wlr->current.layer != ls->layer)
		place_in_layer(ls, wlr->current.layer);
	layer_arrange(ls->output);
	update_keyboard(ls);
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
	ls->wlr->data = NULL;
	free(ls);
}

static void layer_new_popup(struct wl_listener *listener, void *data)
{
	struct layer_surface *ls = wl_container_of(listener, ls, new_popup);
	struct wlr_xdg_popup *popup = data;
	popup->base->data = wlr_scene_xdg_surface_create(ls->scene->tree,
		popup->base);
}

static void new_layer_surface(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		new_layer_surface);
	struct wlr_layer_surface_v1 *wlr = data;

	struct output *output = wlr->output ? wlr->output->data :
		output_at(server, server->cursor->x, server->cursor->y);
	if (!output) {
		wlr_log(WLR_INFO, "no output for layer surface %s", wlr->namespace);
		wlr_layer_surface_v1_destroy(wlr);
		return;
	}
	wlr->output = output->wlr_output;

	struct layer_surface *ls = calloc(1, sizeof(*ls));
	ls->server = server;
	ls->output = output;
	ls->wlr = wlr;
	ls->layer = wlr->pending.layer;
	wlr->data = ls;
	ls->scene = wlr_scene_layer_surface_v1_create(
		server->layer_shell[ls->layer], wlr);
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

void layer_output_gone(struct output *output)
{
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
