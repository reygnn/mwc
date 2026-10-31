/*
 * XWayland: managed X11 windows become views with a Motif frame,
 * override-redirect windows (menus, tooltips) are shown as they are.
 * This is where real Motif applications (nedit, xmgrace, ...) end up.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>
#include <wlr/xwayland.h>
#include "mwc.h"

/* ---- managed windows -------------------------------------------------- */

static void xw_commit(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, commit);
	struct wlr_surface *s = view->xsurface->surface;
	if (s->current.width == view->width &&
			s->current.height == view->height)
		return;
	/* Reuse the resize anchoring of the xdg path. */
	struct server *server = view->server;
	if (server->cursor_mode == CURSOR_RESIZE &&
			server->grabbed_view == view) {
		struct wlr_box *g = &server->grab_geobox;
		if (server->resize_edges & WLR_EDGE_LEFT)
			view->x = g->x + g->width -
				(s->current.width + frame_extra_w(view));
		if (server->resize_edges & WLR_EDGE_TOP)
			view->y = g->y + g->height -
				(s->current.height + frame_extra_h(view));
		wlr_scene_node_set_position(&view->tree->node,
			view->x, view->y);
	}
	view->width = s->current.width;
	view->height = s->current.height;
	frame_render(view);
}

static void xw_map(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, map);
	struct wlr_xwayland_surface *xs = view->xsurface;

	view->content_tree = wlr_scene_subsurface_tree_create(view->tree,
		xs->surface);
	view->width = xs->surface->current.width ?
		xs->surface->current.width : xs->width;
	view->height = xs->surface->current.height ?
		xs->surface->current.height : xs->height;
	view->ssd = xs->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
	view->decorated = view->ssd && !view->fullscreen;

	view->commit.notify = xw_commit;
	wl_signal_add(&xs->surface->events.commit, &view->commit);
	view_handle_map(view);
}

static void xw_unmap(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, unmap);
	view_handle_unmap(view);
	wl_list_remove(&view->commit.link);
	wlr_scene_node_destroy(&view->content_tree->node);
	view->content_tree = NULL;
}

static void xw_associate(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, associate);
	struct wlr_surface *surface = view->xsurface->surface;
	view->map.notify = xw_map;
	wl_signal_add(&surface->events.map, &view->map);
	view->unmap.notify = xw_unmap;
	wl_signal_add(&surface->events.unmap, &view->unmap);
}

static void xw_dissociate(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, dissociate);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
}

static void xw_destroy(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, destroy);
	wl_list_remove(&view->associate.link);
	wl_list_remove(&view->dissociate.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_configure.link);
	wl_list_remove(&view->request_activate.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_decorations.link);
	view_handle_destroy(view);
}

static void xw_request_configure(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	if (!view->mapped) {
		/* Honour an explicit initial position, otherwise cascade. */
		if (ev->x != 0 || ev->y != 0) {
			view->placed = true;
			view->x = ev->x - frame_left(view);
			view->y = ev->y - frame_top(view);
		}
		wlr_xwayland_surface_configure(view->xsurface, ev->x, ev->y,
			ev->width, ev->height);
		return;
	}
	if (view->maximized || view->fullscreen) {
		view_send_size(view, view->width, view->height);
		return;
	}
	view_set_geometry(view, ev->x - frame_left(view),
		ev->y - frame_top(view), ev->width, ev->height);
}

static void xw_request_activate(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_activate);
	view_focus(view);
}

static void xw_request_move(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_move);
	view_begin_interactive(view, CURSOR_MOVE, 0, false);
}

static void xw_request_resize(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_resize);
	struct wlr_xwayland_resize_event *ev = data;
	view_begin_interactive(view, CURSOR_RESIZE, ev->edges, false);
}

static void xw_request_maximize(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_maximize);
	struct wlr_xwayland_surface *xs = view->xsurface;
	view_set_maximized(view, xs->maximized_horz || xs->maximized_vert);
}

static void xw_request_minimize(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_minimize);
	struct wlr_xwayland_minimize_event *ev = data;
	if (ev->minimize)
		view_minimize(view);
	else
		view_restore(view);
}

static void xw_request_fullscreen(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_fullscreen);
	view_set_fullscreen(view, view->xsurface->fullscreen);
}

static void xw_set_title(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_title);
	if (view->mapped)
		view_update_title(view);
}

static void xw_set_decorations(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_decorations);
	view->ssd = view->xsurface->decorations ==
		WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
	view->decorated = view->ssd && !view->fullscreen;
	if (view->mapped) {
		wlr_scene_node_set_position(&view->content_tree->node,
			frame_left(view), frame_top(view));
		frame_render(view);
		view_send_size(view, view->width, view->height);
	}
}

static void new_managed(struct server *server, struct wlr_xwayland_surface *xs)
{
	struct view *view = calloc(1, sizeof(*view));
	view->server = server;
	view->type = VIEW_XWAYLAND;
	view->xsurface = xs;
	view->ssd = true;
	view->decorated = true;
	view_init_common(view);

#define LISTEN(sig, field, fn) \
	do { view->field.notify = fn; wl_signal_add(&xs->events.sig, \
		&view->field); } while (0)
	LISTEN(associate, associate, xw_associate);
	LISTEN(dissociate, dissociate, xw_dissociate);
	LISTEN(destroy, destroy, xw_destroy);
	LISTEN(request_configure, request_configure, xw_request_configure);
	LISTEN(request_activate, request_activate, xw_request_activate);
	LISTEN(request_move, request_move, xw_request_move);
	LISTEN(request_resize, request_resize, xw_request_resize);
	LISTEN(request_maximize, request_maximize, xw_request_maximize);
	LISTEN(request_minimize, request_minimize, xw_request_minimize);
	LISTEN(request_fullscreen, request_fullscreen, xw_request_fullscreen);
	LISTEN(set_title, set_title, xw_set_title);
	LISTEN(set_decorations, set_decorations, xw_set_decorations);
#undef LISTEN
}

/* ---- unmanaged (override-redirect) windows ------------------------------ */

static void um_set_geometry(struct wl_listener *listener, void *data)
{
	struct unmanaged *um = wl_container_of(listener, um, set_geometry);
	if (um->tree)
		wlr_scene_node_set_position(&um->tree->node,
			um->xsurface->x, um->xsurface->y);
}

static void um_map(struct wl_listener *listener, void *data)
{
	struct unmanaged *um = wl_container_of(listener, um, map);
	struct wlr_xwayland_surface *xs = um->xsurface;
	um->tree = wlr_scene_subsurface_tree_create(
		um->server->layer_unmanaged, xs->surface);
	wlr_scene_node_set_position(&um->tree->node, xs->x, xs->y);
	um->set_geometry.notify = um_set_geometry;
	wl_signal_add(&xs->events.set_geometry, &um->set_geometry);
	if (wlr_xwayland_surface_override_redirect_wants_focus(xs)) {
		struct wlr_seat *seat = um->server->seat;
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
		wlr_seat_keyboard_notify_enter(seat, xs->surface,
			kb ? kb->keycodes : NULL, kb ? kb->num_keycodes : 0,
			kb ? &kb->modifiers : NULL);
	}
}

static void um_unmap(struct wl_listener *listener, void *data)
{
	struct unmanaged *um = wl_container_of(listener, um, unmap);
	struct wlr_seat *seat = um->server->seat;
	wl_list_remove(&um->set_geometry.link);
	wlr_scene_node_destroy(&um->tree->node);
	um->tree = NULL;
	/* Hand the keyboard back to the focused window. */
	if (seat->keyboard_state.focused_surface == um->xsurface->surface) {
		struct view *v;
		wl_list_for_each(v, &um->server->views, link) {
			if (v->activated) {
				v->activated = false;
				view_focus(v);
				break;
			}
		}
	}
}

static void um_associate(struct wl_listener *listener, void *data)
{
	struct unmanaged *um = wl_container_of(listener, um, associate);
	um->map.notify = um_map;
	wl_signal_add(&um->xsurface->surface->events.map, &um->map);
	um->unmap.notify = um_unmap;
	wl_signal_add(&um->xsurface->surface->events.unmap, &um->unmap);
}

static void um_dissociate(struct wl_listener *listener, void *data)
{
	struct unmanaged *um = wl_container_of(listener, um, dissociate);
	wl_list_remove(&um->map.link);
	wl_list_remove(&um->unmap.link);
}

static void um_request_configure(struct wl_listener *listener, void *data)
{
	struct unmanaged *um = wl_container_of(listener, um, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	wlr_xwayland_surface_configure(um->xsurface, ev->x, ev->y,
		ev->width, ev->height);
}

static void um_destroy(struct wl_listener *listener, void *data)
{
	struct unmanaged *um = wl_container_of(listener, um, destroy);
	wl_list_remove(&um->associate.link);
	wl_list_remove(&um->dissociate.link);
	wl_list_remove(&um->destroy.link);
	wl_list_remove(&um->request_configure.link);
	wl_list_remove(&um->link);
	free(um);
}

static void new_unmanaged(struct server *server,
	struct wlr_xwayland_surface *xs)
{
	struct unmanaged *um = calloc(1, sizeof(*um));
	um->server = server;
	um->xsurface = xs;
	wl_list_insert(&server->unmanaged, &um->link);
	um->associate.notify = um_associate;
	wl_signal_add(&xs->events.associate, &um->associate);
	um->dissociate.notify = um_dissociate;
	wl_signal_add(&xs->events.dissociate, &um->dissociate);
	um->destroy.notify = um_destroy;
	wl_signal_add(&xs->events.destroy, &um->destroy);
	um->request_configure.notify = um_request_configure;
	wl_signal_add(&xs->events.request_configure, &um->request_configure);
}

/* ---- setup -------------------------------------------------------------- */

static void new_surface(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		new_xwayland_surface);
	struct wlr_xwayland_surface *xs = data;
	if (xs->override_redirect)
		new_unmanaged(server, xs);
	else
		new_managed(server, xs);
}

static void xwayland_ready(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		xwayland_ready);
	wlr_xwayland_set_seat(server->xwayland, server->seat);

	/* X clients that never set a cursor get the default arrow. */
	struct wlr_xcursor *xc = wlr_xcursor_manager_get_xcursor(
		server->cursor_mgr, "default", 1);
	if (xc) {
		struct wlr_xcursor_image *img = xc->images[0];
		wlr_xwayland_set_cursor(server->xwayland, img->buffer,
			img->width * 4, img->width, img->height,
			img->hotspot_x, img->hotspot_y);
	}
}

void xwayland_init(struct server *server)
{
	wl_list_init(&server->unmanaged);
	server->xwayland = wlr_xwayland_create(server->display,
		server->compositor, true);
	if (!server->xwayland) {
		wlr_log(WLR_ERROR, "XWayland unavailable, X11 apps won't run");
		unsetenv("DISPLAY");
		return;
	}
	server->xwayland_ready.notify = xwayland_ready;
	wl_signal_add(&server->xwayland->events.ready, &server->xwayland_ready);
	server->new_xwayland_surface.notify = new_surface;
	wl_signal_add(&server->xwayland->events.new_surface,
		&server->new_xwayland_surface);
	setenv("DISPLAY", server->xwayland->display_name, true);
	wlr_log(WLR_INFO, "XWayland on DISPLAY=%s",
		server->xwayland->display_name);
}
