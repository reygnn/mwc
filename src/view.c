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

struct wlr_box output_usable_area(struct server *server, double lx, double ly)
{
	struct wlr_box box = { 0, 0, 1024, 768 };
	struct wlr_output *o = wlr_output_layout_output_at(server->output_layout,
		lx, ly);
	if (!o) {
		struct output *out;
		wl_list_for_each(out, &server->outputs, link) {
			o = out->wlr_output;
			break;
		}
	}
	if (o)
		wlr_output_layout_get_box(server->output_layout, o, &box);
	return box;
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

void view_set_activated(struct view *view, bool activated)
{
	view->activated = activated;
	if (view->type == VIEW_XDG) {
		wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
	} else {
		wlr_xwayland_surface_activate(view->xsurface, activated);
		if (activated)
			wlr_xwayland_surface_restack(view->xsurface, NULL,
				XCB_STACK_MODE_ABOVE);
	}
	frame_render(view);
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

	struct view *prev = focused_view(server);
	if (prev && prev != view)
		view_set_activated(prev, false);

	if (view->minimized) {
		view_restore(view);
		return;
	}

	wlr_scene_node_raise_to_top(&view->tree->node);
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);
	if (!view->activated)
		view_set_activated(view, true);

	struct wlr_surface *surface = view_surface(view);
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	if (surface && kb) {
		wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes,
			kb->num_keycodes, &kb->modifiers);
	} else if (surface) {
		wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
	}
}

/* Give the focus to the topmost remaining window, or to nobody. */
static void focus_next(struct server *server, struct view *except)
{
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v != except && v->mapped && !v->minimized) {
			view_focus(v);
			return;
		}
	}
	if (except && except->activated)
		view_set_activated(except, false);
	wlr_seat_keyboard_notify_clear_focus(server->seat);
}

void view_close(struct view *view)
{
	if (view->type == VIEW_XDG)
		wlr_xdg_toplevel_send_close(view->xdg_toplevel);
	else
		wlr_xwayland_surface_close(view->xsurface);
}

void view_lower(struct view *view)
{
	struct server *server = view->server;
	wlr_scene_node_lower_to_bottom(&view->tree->node);
	wl_list_remove(&view->link);
	wl_list_insert(server->views.prev, &view->link);
	if (view->activated)
		focus_next(server, view);
}

void view_set_maximized(struct view *view, bool maximized)
{
	if (view->maximized == maximized || view->fullscreen)
		goto ack;
	if (maximized) {
		view->saved = (struct wlr_box){ view->x, view->y,
			view->width, view->height };
		struct wlr_box a = output_usable_area(view->server,
			view->x + view->width / 2.0, view->y + view->height / 2.0);
		view->maximized = true;
		view_set_geometry(view, a.x, a.y,
			a.width - frame_extra_w(view),
			a.height - frame_extra_h(view));
	} else {
		view->maximized = false;
		view_set_geometry(view, view->saved.x, view->saved.y,
			view->saved.width, view->saved.height);
	}
	frame_render(view);
ack:
	if (view->type == VIEW_XDG) {
		if (view->xdg_toplevel->base->initialized)
			wlr_xdg_toplevel_set_maximized(view->xdg_toplevel,
				view->maximized);
	} else {
		wlr_xwayland_surface_set_maximized(view->xsurface,
			view->maximized, view->maximized);
	}
}

void view_set_fullscreen(struct view *view, bool fullscreen)
{
	if (view->fullscreen != fullscreen) {
		if (fullscreen) {
			if (!view->maximized)
				view->saved = (struct wlr_box){ view->x,
					view->y, view->width, view->height };
			struct wlr_box a = output_usable_area(view->server,
				view->x + view->width / 2.0,
				view->y + view->height / 2.0);
			view->fullscreen = true;
			/* Fullscreen windows lose their frame for the duration. */
			view->decorated = false;
			frame_render(view);
			view_set_geometry(view, a.x, a.y, a.width, a.height);
			view_focus(view);
		} else {
			view->fullscreen = false;
			view->decorated = view->ssd;
			if (view->maximized) {
				view->maximized = false;
				view_set_maximized(view, true);
			} else {
				view_set_geometry(view, view->saved.x,
					view->saved.y, view->saved.width,
					view->saved.height);
			}
			frame_render(view);
		}
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

void view_minimize(struct view *view)
{
	if (view->minimized || !view->mapped)
		return;
	struct server *server = view->server;
	view->minimized = true;
	wlr_scene_node_set_enabled(&view->tree->node, false);
	icon_render(view);
	icons_arrange(server);
	wlr_scene_node_set_enabled(&view->icon_node->node, true);
	if (view->type == VIEW_XWAYLAND)
		wlr_xwayland_surface_set_minimized(view->xsurface, true);
	if (view->activated)
		focus_next(server, view);
}

void view_restore(struct view *view)
{
	if (view->minimized) {
		view->minimized = false;
		wlr_scene_node_set_enabled(&view->icon_node->node, false);
		wlr_scene_node_set_enabled(&view->tree->node, true);
		if (view->type == VIEW_XWAYLAND)
			wlr_xwayland_surface_set_minimized(view->xsurface,
				false);
		view_focus(view);
	} else if (view->fullscreen) {
		view_set_fullscreen(view, false);
	} else if (view->maximized) {
		view_set_maximized(view, false);
	}
}

void view_update_title(struct view *view)
{
	frame_render(view);
	if (view->minimized)
		icon_render(view);
}

void view_theme_changed(struct view *view)
{
	config_view_shades(view);
	if (!view->mapped)
		return;
	/* The title bar grows with the font. The client keeps its size and
	 * the frame around it changes, except where the frame's size is fixed. */
	view_update_content_pos(view);
	if (view->maximized && !view->fullscreen) {
		struct wlr_box a = output_usable_area(view->server,
			view->x + 1, view->y + 1);
		view_set_geometry(view, a.x, a.y, a.width - frame_extra_w(view),
			a.height - frame_extra_h(view));
	} else {
		xwayland_configure(view);
	}
	frame_render(view);
	if (view->minimized)
		icon_render(view);
}

void view_cycle(struct server *server, bool backwards)
{
	struct view *top = NULL, *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->mapped && !v->minimized) {
			top = v;
			break;
		}
	}
	if (!top)
		return;
	if (backwards) {
		/* Push the current window to the back, focus the next one. */
		view_lower(top);
		return;
	}
	wl_list_for_each_reverse(v, &server->views, link) {
		if (v->mapped && !v->minimized) {
			view_focus(v);
			return;
		}
	}
}

void view_begin_interactive(struct view *view, enum cursor_mode mode,
	uint32_t edges, bool until_click)
{
	struct server *server = view->server;
	if (view->fullscreen)
		return;
	if (view->maximized && mode == CURSOR_RESIZE)
		return;
	server->grabbed_view = view;
	server->cursor_mode = mode;
	server->grab_until_click = until_click;

	if (mode == CURSOR_MOVE) {
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
		cursor_set_name(server, "move");
	} else {
		server->grab_geobox = (struct wlr_box){ view->x, view->y,
			view->width + frame_extra_w(view),
			view->height + frame_extra_h(view) };
		double bx = view->x + ((edges & WLR_EDGE_RIGHT) ?
			server->grab_geobox.width : 0);
		double by = view->y + ((edges & WLR_EDGE_BOTTOM) ?
			server->grab_geobox.height : 0);
		server->grab_x = server->cursor->x - bx;
		server->grab_y = server->cursor->y - by;
		server->resize_edges = edges;
		if (view->type == VIEW_XDG) {
			wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, true);
			view->anchor_edges = edges;
			view->anchor_right = view->x + server->grab_geobox.width;
			view->anchor_bottom = view->y + server->grab_geobox.height;
			view->anchor_serial = view->xdg_toplevel->base->
				scheduled_serial;
		}
	}
	wlr_seat_pointer_notify_clear_focus(server->seat);
}

void view_drag_unmaximize(struct view *view)
{
	struct server *server = view->server;
	double frac = (server->cursor->x - view->x) /
		(double)(view->width + frame_extra_w(view));
	view_set_maximized(view, false);
	int nw = view->saved.width + frame_extra_w(view);
	view_set_position(view, server->cursor->x - frac * nw,
		server->cursor->y - theme.border - theme.title_height / 2);
	server->grab_x = server->cursor->x - view->x;
	server->grab_y = server->cursor->y - view->y;
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
}

/* Cascade new windows from the top-left of the output under the pointer. */
static void view_place(struct view *view)
{
	struct server *server = view->server;
	struct wlr_box a = output_usable_area(server, server->cursor->x,
		server->cursor->y);
	int w = view->width + frame_extra_w(view);
	int h = view->height + frame_extra_h(view);
	int step = theme.title_height + theme.border;
	int x = a.x + 32 + server->cascade * step;
	int y = a.y + 32 + server->cascade * step;
	if (x + w > a.x + a.width || y + h > a.y + a.height) {
		server->cascade = 0;
		x = a.x + (a.width - w) / 2;
		y = a.y + (a.height - h) / 2;
		if (x < a.x)
			x = a.x;
		if (y < a.y)
			y = a.y;
	} else {
		server->cascade = (server->cascade + 1) % 10;
	}
	view_set_position(view, x, y);
}

void view_handle_map(struct view *view)
{
	view->mapped = true;
	config_view_shades(view); /* the class or app_id is known by now */
	wlr_scene_node_set_enabled(&view->tree->node, true);
	view_update_content_pos(view);
	if (view->placed)
		view_set_position(view, view->x, view->y);
	else if (!view->maximized && !view->fullscreen)
		view_place(view);
	frame_render(view);
	view_focus(view);
}

void view_handle_unmap(struct view *view)
{
	struct server *server = view->server;
	if (server->grabbed_view == view) {
		server->cursor_mode = CURSOR_PASSTHROUGH;
		server->grabbed_view = NULL;
	}
	if (server->menu.open && server->menu.target == view)
		menu_close(server);
	if (server->last_click_view == view)
		server->last_click_view = NULL;
	bool was_active = view->activated;
	view->mapped = false;
	view->activated = false;
	view->minimized = false;
	wlr_scene_node_set_enabled(&view->tree->node, false);
	wlr_scene_node_set_enabled(&view->icon_node->node, false);
	if (was_active)
		focus_next(server, view);
}

void view_handle_destroy(struct view *view)
{
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
		wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
		return;
	}
	if (!view->mapped)
		return;
	view_apply_size(view, base->geometry.width, base->geometry.height);

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
	view->width = g->width;
	view->height = g->height;
	view_handle_map(view);
}

static void xdg_unmap(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, unmap);
	view_handle_unmap(view);
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
	view_handle_destroy(view);
}

static void xdg_request_move(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_move);
	view_begin_interactive(view, CURSOR_MOVE, 0, false);
}

static void xdg_request_resize(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct view *view = wl_container_of(listener, view, request_resize);
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
	struct server *s = view->server;
	menu_open_window(s, view, s->cursor->x, s->cursor->y);
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
	toplevel->base->data = view->content_tree;

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
}

/* ---- popups ------------------------------------------------------------- */

struct popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static struct view *view_from_tree(struct wlr_scene_tree *tree)
{
	struct wlr_scene_node *n = &tree->node;
	while (n && !n->data)
		n = n->parent ? &n->parent->node : NULL;
	return n ? n->data : NULL;
}

static void popup_commit(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, commit);
	if (!popup->xdg_popup->base->initial_commit)
		return;

	/* Keep the popup on the output its toplevel is on. */
	struct wlr_scene_tree *tree = popup->xdg_popup->base->data;
	struct view *view = tree ? view_from_tree(tree) : NULL;
	if (view && view->type == VIEW_XDG) {
		struct wlr_box a = output_usable_area(view->server,
			view->x + 1, view->y + 1);
		struct wlr_box *g = &view->xdg_toplevel->base->geometry;
		a.x -= view->x + frame_left(view) - g->x;
		a.y -= view->y + frame_top(view) - g->y;
		/* The box is relative to the popup's parent toplevel only
		 * for first-level popups; nested ones are close enough. */
		wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &a);
	}
	wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
}

static void popup_destroy(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void new_xdg_popup(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_popup *xdg_popup = data;
	struct popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (!parent || !parent->data) {
		wlr_log(WLR_ERROR, "popup without a known xdg parent");
		free(popup);
		return;
	}
	struct wlr_scene_tree *parent_tree = parent->data;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree,
		xdg_popup->base);

	popup->commit.notify = popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ---- decorations -------------------------------------------------------- */

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
		deco->view->decorated = false;
		if (deco->view->mapped) {
			frame_render(deco->view);
			view_update_content_pos(deco->view);
		}
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
	view->decorated = !view->fullscreen;
	if (wlr->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(wlr,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	if (view->mapped) {
		view_update_content_pos(view);
		frame_render(view);
	}
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
	view->decorated = view->ssd && !view->fullscreen;
	if (view->mapped) {
		view_update_content_pos(view);
		frame_render(view);
	}
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
