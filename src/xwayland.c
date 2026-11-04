/*
 * XWayland: managed X11 windows become views with a Motif frame,
 * override-redirect windows (menus, tooltips) are shown as they are.
 * This is where real Motif applications (nedit, xmgrace, ...) end up.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>
#include <wlr/xwayland.h>
#include <xcb/xcb_icccm.h>
#include "mwc.h"

static struct view *new_managed(struct server *server,
	struct wlr_xwayland_surface *xs);
static struct unmanaged *new_unmanaged(struct server *server,
	struct wlr_xwayland_surface *xs);
static void um_associate(struct wl_listener *listener, void *data);
static void um_dissociate(struct wl_listener *listener, void *data);
static void um_map(struct wl_listener *listener, void *data);
static void um_unmap(struct wl_listener *listener, void *data);
static void um_destroy(struct wl_listener *listener, void *data);

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

/* wlroots parses only the decoration part of _MOTIF_WM_HINTS. Reading it here
 * blocks on the X server, as wlroots' own property reads do. */
static void read_motif_hints(struct view *view)
{
	struct server *server = view->server;
	view->motif_flags = 0;
	xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server->xwayland);
	if (!conn || !server->atom_motif_hints)
		return;
	xcb_get_property_cookie_t cookie = xcb_get_property(conn, 0,
		view->xsurface->window_id, server->atom_motif_hints,
		XCB_GET_PROPERTY_TYPE_ANY, 0, 5);
	xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
	if (!reply)
		return;
	/* Toolkits disagree on the type, but not on the format. */
	if (reply->format == 32 && xcb_get_property_value_length(reply) >= 12) {
		const uint32_t *h = xcb_get_property_value(reply);
		view->motif_flags = h[0];
		view->motif_funcs = h[1];
		view->motif_decor = h[2];
	}
	free(reply);
}

/*
 * A client may set _NET_WM_STATE before wlroots has started watching its window.
 * That makes no PropertyNotify, so wlroots doesn't see it, and overwrites it when
 * the window maps. Reading it here, on creation, covers that side of the race;
 * wlroots hears about anything later.
 */
static void read_initial_state(struct view *view)
{
	struct server *server = view->server;
	struct wlr_xwayland_surface *xs = view->xsurface;
	xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server->xwayland);
	if (!conn || !server->atom_net_wm_state)
		return;
	xcb_get_property_cookie_t cookie = xcb_get_property(conn, 0, xs->window_id,
		server->atom_net_wm_state, XCB_ATOM_ATOM, 0, 32);
	xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
	if (!reply)
		return;
	if (reply->format == 32) {
		const xcb_atom_t *atoms = xcb_get_property_value(reply);
		int n = xcb_get_property_value_length(reply) / sizeof(*atoms);
		bool max_v = false, max_h = false;
		for (int i = 0; i < n; i++) {
			if (atoms[i] == server->atom_state_above)
				wlr_xwayland_surface_set_above(xs, true);
			else if (atoms[i] == server->atom_state_below)
				wlr_xwayland_surface_set_below(xs, true);
			else if (atoms[i] == server->atom_state_fullscreen)
				wlr_xwayland_surface_set_fullscreen(xs, true);
			else if (atoms[i] == server->atom_state_max_vert)
				max_v = true;
			else if (atoms[i] == server->atom_state_max_horz)
				max_h = true;
		}
		if (max_v || max_h)
			wlr_xwayland_surface_set_maximized(xs, max_h, max_v);
	}
	free(reply);
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
	view->ssd = true;
	view->decorated = !view->fullscreen;
	read_motif_hints(view);
	view_update_properties(view);

	/* As in mwm-x11: only a position the program or user insisted on counts,
	 * and panels know where they belong. */
	const xcb_size_hints_t *sh = xs->size_hints;
	uint32_t flags = sh ? sh->flags : 0;
	view->placed = view->wtype == WT_DOCK || view->wtype == WT_DESKTOP ||
		(flags & XCB_ICCCM_SIZE_HINT_US_POSITION) ||
		((flags & XCB_ICCCM_SIZE_HINT_P_POSITION) && (xs->x || xs->y));
	if (view->placed)
		view_gravity_to_frame(view, xs->x, xs->y, &view->x, &view->y);

	view->commit.notify = xw_commit;
	wl_signal_add(&xs->surface->events.commit, &view->commit);
	view_handle_map(view);

	/* States it asked for before it was mapped. */
	if (xs->fullscreen)
		view_set_fullscreen(view, true);
	else if (xs->maximized_horz || xs->maximized_vert)
		view_set_maximized(view, true);
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
	/* Xwayland may commit the content before it says which window the surface
	 * belongs to. wlroots would then wait for the next commit to map it, which
	 * a window that doesn't redraw never makes. */
	if (wlr_surface_has_buffer(surface) && !surface->mapped)
		wlr_surface_map(surface);
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
	wl_list_remove(&view->set_parent.link);
	wl_list_remove(&view->set_window_type.link);
	wl_list_remove(&view->set_strut_partial.link);
	wl_list_remove(&view->request_above.link);
	wl_list_remove(&view->request_below.link);
	wl_list_remove(&view->set_override_redirect.link);
	view_handle_destroy(view);
}

static void xw_request_configure(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	if (!view->mapped) {
		/* xw_map decides what the position means. */
		wlr_xwayland_surface_configure(view->xsurface, ev->x, ev->y,
			ev->width, ev->height);
		return;
	}
	/* Fullscreen, maximized, snapped and tiled windows keep their place. */
	if (view->maximized || view->fullscreen || view->snap != SNAP_NONE ||
			view->tiled) {
		view_send_size(view, view->xsurface->width, view->xsurface->height);
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
	if (!view->mapped)
		return; /* xw_map reads them */
	read_motif_hints(view);
	view_update_properties(view);
}

static void xw_set_parent(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_parent);
	view_update_properties(view);
}

static void xw_set_window_type(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_window_type);
	view_update_properties(view);
}

static void xw_set_strut_partial(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_strut_partial);
	layers_arrange_all(view->server);
}

static void xw_request_above(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_above);
	wlr_xwayland_surface_set_above(view->xsurface, view->xsurface->above);
	views_update_layers(view->server);
}

static void xw_request_below(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_below);
	wlr_xwayland_surface_set_below(view->xsurface, view->xsurface->below);
	views_update_layers(view->server);
}

/* A window can stop being managed: toolkits set override-redirect on a window that
 * already exists. Everything starts over as the other kind of window. */
static void xw_set_override_redirect(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_override_redirect);
	struct wlr_xwayland_surface *xs = view->xsurface;
	struct server *server = view->server;
	if (!xs->override_redirect)
		return;
	struct wlr_surface *surface = xs->surface;
	bool mapped = surface && surface->mapped;
	if (mapped)
		xw_unmap(&view->unmap, NULL);
	if (surface)
		xw_dissociate(&view->dissociate, NULL);
	xw_destroy(&view->destroy, NULL);

	struct unmanaged *um = new_unmanaged(server, xs);
	if (surface) {
		um_associate(&um->associate, NULL);
		if (mapped)
			um_map(&um->map, NULL);
	}
}

static struct view *new_managed(struct server *server, struct wlr_xwayland_surface *xs)
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
	LISTEN(set_parent, set_parent, xw_set_parent);
	LISTEN(set_window_type, set_window_type, xw_set_window_type);
	LISTEN(set_strut_partial, set_strut_partial, xw_set_strut_partial);
	LISTEN(request_above, request_above, xw_request_above);
	LISTEN(request_below, request_below, xw_request_below);
	LISTEN(set_override_redirect, set_override_redirect, xw_set_override_redirect);
#undef LISTEN
	read_initial_state(view);
	return view;
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
	/* See xw_associate(). */
	if (wlr_surface_has_buffer(um->xsurface->surface) &&
			!um->xsurface->surface->mapped)
		wlr_surface_map(um->xsurface->surface);
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
	wl_list_remove(&um->set_override_redirect.link);
	wl_list_remove(&um->link);
	free(um);
}

/* See xw_set_override_redirect(). */
static void um_set_override_redirect(struct wl_listener *listener, void *data)
{
	struct unmanaged *um = wl_container_of(listener, um, set_override_redirect);
	struct wlr_xwayland_surface *xs = um->xsurface;
	struct server *server = um->server;
	if (xs->override_redirect)
		return;
	struct wlr_surface *surface = xs->surface;
	bool mapped = surface && surface->mapped;
	if (mapped)
		um_unmap(&um->unmap, NULL);
	if (surface)
		um_dissociate(&um->dissociate, NULL);
	um_destroy(&um->destroy, NULL);

	struct view *view = new_managed(server, xs);
	if (surface) {
		xw_associate(&view->associate, NULL);
		if (mapped)
			xw_map(&view->map, NULL);
	}
}

static struct unmanaged *new_unmanaged(struct server *server,
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
	um->set_override_redirect.notify = um_set_override_redirect;
	wl_signal_add(&xs->events.set_override_redirect, &um->set_override_redirect);
	return um;
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

static uint32_t intern_atom(xcb_connection_t *conn, const char *name)
{
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn,
		xcb_intern_atom(conn, 0, strlen(name), name), NULL);
	uint32_t atom = reply ? reply->atom : 0;
	free(reply);
	return atom;
}

static void xwayland_ready(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		xwayland_ready);
	wlr_xwayland_set_seat(server->xwayland, server->seat);

	xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server->xwayland);
	server->atom_motif_hints = intern_atom(conn, "_MOTIF_WM_HINTS");
	server->atom_net_wm_state = intern_atom(conn, "_NET_WM_STATE");
	server->atom_state_above = intern_atom(conn, "_NET_WM_STATE_ABOVE");
	server->atom_state_below = intern_atom(conn, "_NET_WM_STATE_BELOW");
	server->atom_state_fullscreen = intern_atom(conn,
		"_NET_WM_STATE_FULLSCREEN");
	server->atom_state_max_vert = intern_atom(conn,
		"_NET_WM_STATE_MAXIMIZED_VERT");
	server->atom_state_max_horz = intern_atom(conn,
		"_NET_WM_STATE_MAXIMIZED_HORZ");
	layer_publish_workarea(server);

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
