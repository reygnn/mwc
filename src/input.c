/*
 * Pointer and keyboard handling: frame interaction, interactive move and
 * resize, mwm's default key bindings.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "mwc.h"

#define DOUBLE_CLICK_MS 400
#define DRAG_THRESHOLD 4
#define MAX_SWALLOWED_KEYS 32

struct keyboard {
	struct wl_list link;
	struct server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
	/* Keys whose press went to a binding: their release must not reach a client,
	 * which never saw them go down. */
	uint32_t swallowed[MAX_SWALLOWED_KEYS];
	int n_swallowed;
};

static void spawn_argv(const char *const argv[])
{
	pid_t pid = fork();
	if (pid == 0) {
		/* wl_event_loop_add_signal() blocks our handled signals, and
		 * programs would inherit that and ignore SIGTERM or SIGHUP. */
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL); /* an ignored signal stays ignored across exec */
		/* Double fork so we never collect zombies. */
		setsid();
		if (fork() == 0) {
			execv(argv[0], (char *const *)argv);
			/* A script without a #! line, as a shell would run it. */
			if (errno == ENOEXEC) {
				const char *sh[] = { "/bin/sh", argv[0], NULL };
				execv(sh[0], (char *const *)sh);
			}
			fprintf(stderr, "mwc: cannot run %s: %s\n", argv[0], strerror(errno));
			_exit(127);
		}
		_exit(0);
	} else if (pid > 0) {
		int status;
		waitpid(pid, &status, 0);
	}
}

void spawn(const char *cmd)
{
	if (!cmd || !*cmd)
		return;
	const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
	spawn_argv(argv);
}

void spawn_file(const char *path)
{
	const char *argv[] = { path, NULL };
	spawn_argv(argv);
}

void cursor_set_name(struct server *server, const char *name)
{
	if (server->cursor_name == name)
		return;
	server->cursor_name = name;
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, name);
}

static const char *edge_cursor(uint32_t edges)
{
	switch (edges) {
	case WLR_EDGE_TOP: return "n-resize";
	case WLR_EDGE_BOTTOM: return "s-resize";
	case WLR_EDGE_LEFT: return "w-resize";
	case WLR_EDGE_RIGHT: return "e-resize";
	case WLR_EDGE_TOP | WLR_EDGE_LEFT: return "nw-resize";
	case WLR_EDGE_TOP | WLR_EDGE_RIGHT: return "ne-resize";
	case WLR_EDGE_BOTTOM | WLR_EDGE_LEFT: return "sw-resize";
	case WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT: return "se-resize";
	default: return "default";
	}
}

static struct view *focused_view(struct server *server)
{
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->activated && v->mapped)
			return v;
	}
	return NULL;
}

/* ---- interactive move / resize ---------------------------------------- */

static void update_feedback(struct server *server)
{
	struct view *view = server->grabbed_view;
	const struct wlr_box *t = &server->grab_target;
	char buf[64];
	if (server->cursor_mode == CURSOR_MOVE && server->grab_zone != SNAP_NONE) {
		snprintf(buf, sizeof(buf), "%s", snap_name(server->grab_zone));
	} else if (server->cursor_mode == CURSOR_MOVE) {
		snprintf(buf, sizeof(buf), "%+d %+d", t->x, t->y);
	} else {
		/* Sizes in the client's own units, e.g. 80 x 24 for xterm. */
		int w = t->width, h = t->height;
		if (view->inc_w > 1)
			w = (w - view->base_w) / view->inc_w;
		if (view->inc_h > 1)
			h = (h - view->base_h) / view->inc_h;
		snprintf(buf, sizeof(buf), "%d x %d", w, h);
	}
	feedback_show(server, buf, view->x + 1, view->y + 1);
}

static void show_outline(struct server *server)
{
	struct view *view = server->grabbed_view;
	const struct wlr_box *t = &server->grab_target;
	outline_show(view, (struct wlr_box){ t->x, t->y,
		t->width + frame_extra_w(view), t->height + frame_extra_h(view) });
}

void grab_started(struct server *server)
{
	/* From the menu or keyboard there is no press to drag from yet, so show
	 * where the window is. A drag shows it on the first motion. */
	if (server->grab_outline && server->grab_until_click)
		show_outline(server);
	update_feedback(server);
}

/* The grab is over; the caller decides what becomes of the window. */
static void clear_grab(struct server *server)
{
	server->cursor_mode = CURSOR_PASSTHROUGH;
	server->grabbed_view = NULL;
	server->grab_until_click = false;
	server->grab_tab = -1;
	server->grab_take = NULL;
	server->grab_drop = NULL;
	server->cursor_name = NULL;
	server->grab_zone = SNAP_NONE;
	outline_hide();
	feedback_hide();
	snap_preview_hide();
}

/* No more configures for a window going away: its surface may be half gone. */
void grab_forget(struct server *server)
{
	clear_grab(server);
}

static void stop_resizing_state(struct server *server)
{
	struct view *view = server->grabbed_view;
	if (!server->grab_outline && server->cursor_mode == CURSOR_RESIZE &&
			view->type == VIEW_XDG)
		wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, false);
}

void grab_give_way(struct view *view)
{
	struct server *server = view->server;
	if (server->grabbed_view != view || server->grab_detaching ||
			(server->cursor_mode != CURSOR_MOVE &&
			server->cursor_mode != CURSOR_RESIZE))
		return;
	stop_resizing_state(server);
	clear_grab(server);
}

static void resize_to(struct view *view, const struct wlr_box *b)
{
	/* An xdg client answers later, and its anchor keeps the opposite edge
	 * in place then. X windows get position and size in one configure. */
	if (view->type == VIEW_XWAYLAND)
		view_set_geometry(view, b->x, b->y, b->width, b->height);
	else
		view_send_size(view, b->width, b->height);
}

static void finish_take(struct server *server, struct view *view, struct view *take,
	bool moved, struct wlr_box target, uint32_t button);

static void end_grab(struct server *server)
{
	struct view *view = server->grabbed_view;
	struct view *resized = server->cursor_mode == CURSOR_RESIZE ? view : NULL;
	int tab = server->grab_detached ? -1 : server->grab_tab;
	struct view *take = server->grab_take;
	bool moved = server->grab_detached;
	enum cursor_mode mode = server->cursor_mode;
	bool apply = server->grab_outline && server->grab_changed;
	bool changed = server->grab_changed;
	struct wlr_box target = server->grab_target, orig = server->grab_orig;
	enum snap zone = server->grab_zone, orig_snap = server->grab_orig_snap;
	bool orig_max = server->grab_orig_maximized;
	double px = server->cursor->x, py = server->cursor->y;
	stop_resizing_state(server);
	clear_grab(server);
	if (take) {
		finish_take(server, view, take, moved, target, server->grab_button);
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
		struct wlr_box before = view->saved;
		view->x = orig.x;
		view->y = orig.y;
		view_snap(view, zone, px, py);
		view->saved = orig_snap != SNAP_NONE || orig_max ? before : orig;
		return;
	}
	/* A resized window is no longer snapped. */
	if (mode == CURSOR_RESIZE && changed && view->snap != SNAP_NONE) {
		struct wlr_box current = target;
		view->saved = current;
		view->snap = SNAP_NONE;
		if (view->type == VIEW_XWAYLAND)
			wlr_xwayland_surface_set_maximized(view->xsurface, false, false);
		else
			wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, WLR_EDGE_NONE);
	}
	if (!apply)
		return;
	if (mode == CURSOR_MOVE)
		view_set_position(view, target.x, target.y);
	else
		resize_to(view, &target);
	if (resized)
		tabs_sync_sizes(resized);
}

/* Escape: back to where it was, including out of a maximized state. */
static void cancel_grab(struct server *server)
{
	struct view *view = server->grabbed_view;
	enum cursor_mode mode = server->cursor_mode;
	bool opaque = !server->grab_outline;
	bool was_maximized = server->grab_orig_maximized;
	enum snap was_snapped = server->grab_orig_snap;
	struct wlr_box orig = server->grab_orig;
	stop_resizing_state(server);
	clear_grab(server);
	if ((was_maximized && !view->maximized) ||
			(was_snapped != SNAP_NONE && view->snap == SNAP_NONE)) {
		/* What it restores to must survive this round trip. */
		struct wlr_box saved = view->saved;
		view_snap(view, was_maximized ? SNAP_MAXIMIZE : was_snapped,
			orig.x + orig.width / 2.0, orig.y + orig.height / 2.0);
		view->saved = saved;
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
	struct view *view = server->grabbed_view;
	if (!server->grab_detached) {
		/* From the menu or keyboard every step counts, down to the 1 px
		 * of Shift+arrow. A drag needs to get going first: a click on the
		 * title must leave the window alone, or a double click could
		 * never un-maximize it. */
		double t = server->grab_until_click ? 1 : DRAG_THRESHOLD;
		if (fabs(server->cursor->x - server->grab_px) < t &&
				fabs(server->cursor->y - server->grab_py) < t)
			return;
		server->grab_detached = true;
		/* Taking one window along leaves the rest where it is. */
		if (!server->grab_take && (view->maximized || view->snap != SNAP_NONE))
			view_drag_free(view);
	}
	/* The pointer has sub-pixel positions: round, don't truncate, or the
	 * window ends up a pixel off and jumps when crossing zero. */
	server->grab_target.x = lround(server->cursor->x - server->grab_x);
	server->grab_target.y = lround(server->cursor->y - server->grab_y);
	server->grab_changed = true;
	if (server->grab_outline)
		show_outline(server);
	else
		view_set_position(view, server->grab_target.x, server->grab_target.y);

	if (server->grab_take) {
		struct view *drop = take_target(server, server->grab_take);
		if (drop != server->grab_drop) {
			server->grab_drop = drop;
			if (drop)
				snap_preview_box(view, (struct wlr_box){ drop->x, drop->y,
					drop->width + frame_extra_w(drop), frame_height(drop) });
			else
				snap_preview_hide();
		}
		return;
	}

	enum snap zone = snap_zone_at(view, server->cursor->x, server->cursor->y);
	if (zone != server->grab_zone) {
		server->grab_zone = zone;
		snap_preview(view, zone, server->cursor->x, server->cursor->y);
	}
	update_feedback(server);
}

static void process_resize(struct server *server)
{
	struct view *view = server->grabbed_view;
	struct wlr_box *g = &server->grab_geobox;
	uint32_t edges = server->resize_edges;
	int bx = lround(server->cursor->x - server->grab_x);
	int by = lround(server->cursor->y - server->grab_y);

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
	view_apply_size_hints(view, &cw, &ch, true);
	/* The axis not being dragged keeps its size: rounding it to the client's
	 * steps would change it for no reason. */
	if (!(edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)))
		cw = server->grab_orig.width;
	if (!(edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)))
		ch = server->grab_orig.height;
	if (edges & WLR_EDGE_LEFT)
		left = right - cw - frame_extra_w(view);
	if (edges & WLR_EDGE_TOP)
		top = bottom - ch - frame_extra_h(view);

	struct wlr_box target = { left, top, cw, ch };
	if (wlr_box_equal(&target, &server->grab_target))
		return;
	server->grab_target = target;
	server->grab_changed = true;
	if (server->grab_outline)
		show_outline(server);
	else
		resize_to(view, &target);
	update_feedback(server);
}

/* ---- pointer ------------------------------------------------------------ */

static uint32_t now_msec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void input_pointer_changed(struct server *server)
{
	if (server->cursor_mode == CURSOR_PASSTHROUGH)
		process_cursor_motion(server, now_msec());
}

static void pointer_refresh_idle(void *data)
{
	struct server *server = data;
	server->pointer_refresh = NULL;
	input_pointer_changed(server);
}

void input_pointer_changed_soon(struct server *server)
{
	if (!server->pointer_refresh)
		server->pointer_refresh = wl_event_loop_add_idle(server->event_loop,
			pointer_refresh_idle, server);
}

bool input_button_held(struct server *server)
{
	return server->n_forwarded > 0;
}

void process_cursor_motion(struct server *server, uint32_t time)
{
	wlr_scene_node_set_position(&server->layer_drag->node,
		lround(server->cursor->x), lround(server->cursor->y));
	switch (server->cursor_mode) {
	case CURSOR_MOVE:
		process_move(server);
		return;
	case CURSOR_RESIZE:
		process_resize(server);
		return;
	case CURSOR_MENU:
		menu_motion(server, server->cursor->x, server->cursor->y);
		return;
	case CURSOR_BUTTON: {
		struct view *v = server->grabbed_view;
		uint32_t edges;
		bool inside = frame_hit(v, server->cursor->x - v->x,
			server->cursor->y - v->y, &edges) == v->pressed;
		if (inside != v->pressed_inside) {
			v->pressed_inside = inside;
			frame_render(v);
		}
		return;
	}
	case CURSOR_PASSTHROUGH:
		break;
	}

	/* While a button is down the pointer stays with the client that got the press,
	 * wherever it goes, or a text selection ends at the window's edge and X11 sees a
	 * release that didn't happen. Drags and popups have grabs of their own, which
	 * have to see what is under the pointer. */
	struct wlr_seat *seat = server->seat;
	if (server->n_forwarded > 0 && !wlr_seat_pointer_has_grab(seat)) {
		if (seat->pointer_state.focused_surface) {
			wlr_seat_pointer_notify_motion(seat, time,
				server->cursor->x - server->grab_surface_x,
				server->cursor->y - server->grab_surface_y);
			return;
		}
		/* The client that saw the press is gone: nobody waits for the release. */
		server->n_forwarded = 0;
	}

	struct wlr_surface *surface;
	double sx = 0, sy = 0;
	enum frame_part part;
	uint32_t edges;
	bool on_icon;
	struct view *view = view_at(server, server->cursor->x,
		server->cursor->y, &surface, &sx, &sy, &part, &edges, &on_icon);

	if (!surface) {
		if (view && part == PART_RESIZE)
			cursor_set_name(server, edge_cursor(edges));
		else
			cursor_set_name(server, "default");
		wlr_seat_pointer_notify_clear_focus(server->seat);
		return;
	}
	/* Over a client: it chooses its own cursor through the seat. */
	server->cursor_name = NULL;
	wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
}

/* What the idle notifier counts as somebody being there. */
static void input_activity(struct server *server)
{
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
}

static void cursor_motion(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	input_activity(server);
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	input_activity(server);
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static bool is_double_click(struct server *server, struct view *view,
	enum frame_part part, uint32_t time)
{
	bool dbl = server->last_click_view == view &&
		server->last_click_part == part &&
		time - server->last_click_msec < DOUBLE_CLICK_MS;
	/* A double click must not chain into a triple click. */
	server->last_click_view = dbl ? NULL : view;
	server->last_click_part = part;
	server->last_click_msec = time;
	return dbl;
}

static bool ctrl_held(struct server *server)
{
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	return kb && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_CTRL);
}

static bool alt_held(struct server *server)
{
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	return kb && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_ALT);
}

/* Returns true if the press was handled by the window manager. */
static bool handle_press(struct server *server, uint32_t button,
	uint32_t time)
{
	double cx = server->cursor->x, cy = server->cursor->y;
	struct wlr_surface *surface;
	double sx, sy;
	enum frame_part part;
	uint32_t edges;
	bool on_icon;
	struct view *view = view_at(server, cx, cy, &surface, &sx, &sy,
		&part, &edges, &on_icon);

	if (!view) {
		/* A panel or an X11 popup menu: not ours to handle. */
		if (surface) {
			layer_handle_click(server, surface);
			return false;
		}
		/* Desktop: the root menu, as mwm binds it to buttons 1 and 3 */
		if (button == BTN_LEFT || button == BTN_RIGHT) {
			menu_open_root(server, cx, cy);
			return true;
		}
		if (button == BTN_MIDDLE) {
			const char *run = config_get("runDialog");
			spawn(run ? run : "mrun");
			return true;
		}
		return false;
	}

	if (on_icon) {
		/* Double-click restores, button 3 posts the window menu. */
		if (button == BTN_LEFT && is_double_click(server, view,
				PART_NONE, time))
			view_restore(view);
		else if (button == BTN_RIGHT)
			menu_open_window(server, view, cx, cy);
		return true;
	}

	/* A click on a shaded window unrolls it (focusing does), and does no more, so
	 * its minimize button doesn't roll it right up again; only maximize goes on to
	 * make it the main window. */
	bool was_shaded = view->shaded;
	int tab = part == PART_TAB ? frame_tab_at(view, cx - view->x) : -1;
	view_focus(view);
	if (was_shaded && part != PART_MAXIMIZE_BUTTON) {
		if (tab >= 0)
			tabs_show_nth(view, tab);
		return true;
	}

	if (alt_held(server) && part != PART_NONE) {
		if (button == BTN_LEFT) {
			view_begin_interactive(view, CURSOR_MOVE, 0, false);
			return true;
		}
		if (button == BTN_RIGHT && !view->maximized) {
			/* Resize from the nearest corner. */
			double mx = view->x + (view->width +
				frame_extra_w(view)) / 2.0;
			double my = view->y + (view->height +
				frame_extra_h(view)) / 2.0;
			uint32_t e = (cx < mx ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
				(cy < my ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
			view_begin_interactive(view, CURSOR_RESIZE, e, false);
			if (server->cursor_mode == CURSOR_RESIZE)
				cursor_set_name(server, edge_cursor(e));
			return true;
		}
		if (button == BTN_MIDDLE) {
			view_lower(view);
			return true;
		}
	}

	switch (part) {
	case PART_CLIENT:
	case PART_NONE:
		return false;
	case PART_TAB:
	case PART_TITLE:
		/* The middle button, or Ctrl on a touchpad without one: this one window. */
		if (button == BTN_MIDDLE || (button == BTN_LEFT && ctrl_held(server))) {
			view_begin_interactive(view, CURSOR_MOVE, 0, false);
			if (server->cursor_mode == CURSOR_MOVE) {
				struct view *one = tabs_nth(view, tab);
				server->grab_take = one ? one : view;
				server->grab_outline = true;
			} else if (button == BTN_MIDDLE) {
				view_lower(view);
			} else if (tab >= 0) {
				tabs_show_nth(view, tab);
			}
			return true;
		}
		if (button == BTN_RIGHT) {
			menu_open_window(server, view, cx, cy);
		} else if (button == BTN_LEFT) {
			if (is_double_click(server, view, part, time)) {
				run_action(server, ACT_TOGGLE_MAXIMIZE, view);
			} else {
				view_begin_interactive(view, CURSOR_MOVE, 0,
					false);
				/* A tab is shown on release, unless this becomes a drag. Where
				 * nothing can be moved, there is no drag to wait for. */
				if (server->cursor_mode == CURSOR_MOVE)
					server->grab_tab = tab;
				else if (tab >= 0)
					tabs_show_nth(view, tab);
			}
		} else if (button == BTN_MIDDLE) {
			view_lower(view);
		}
		return true;
	case PART_MENU_BUTTON:
		if (button != BTN_LEFT && button != BTN_RIGHT)
			return true;
		/* The menu takes the grab; its own press handling sees the second click
		 * of a double click and closes the window. */
		menu_open_window(server, view, view->x + frame_border(view),
			view->y + frame_top(view));
		server->menu.from_button = true;
		return true;
	case PART_MINIMIZE_BUTTON:
	case PART_MAXIMIZE_BUTTON:
		if (button != BTN_LEFT)
			return true;
		view->pressed = part;
		view->pressed_inside = true;
		server->grabbed_view = view;
		server->cursor_mode = CURSOR_BUTTON;
		frame_render(view);
		return true;
	case PART_RESIZE:
		if (button == BTN_LEFT)
			view_begin_interactive(view, CURSOR_RESIZE, edges, false);
		return true;
	}
	return false;
}

static void handle_button_release(struct server *server)
{
	struct view *v = server->grabbed_view;
	enum frame_part part = v->pressed;
	bool inside = v->pressed_inside;
	v->pressed = PART_NONE;
	v->pressed_inside = false;
	server->cursor_mode = CURSOR_PASSTHROUGH;
	server->grabbed_view = NULL;
	frame_render(v);
	if (!inside)
		return;
	if (part == PART_MINIMIZE_BUTTON)
		run_action(server, ACT_MINIMIZE, v);
	else if (part == PART_MAXIMIZE_BUTTON)
		run_action(server, ACT_TOGGLE_MAXIMIZE, v);
}

static bool forwarded_take(struct server *server, uint32_t button)
{
	for (int i = 0; i < server->n_forwarded; i++) {
		if (server->forwarded_buttons[i] == button) {
			server->forwarded_buttons[i] =
				server->forwarded_buttons[--server->n_forwarded];
			return true;
		}
	}
	return false;
}

static void forward_release(struct wlr_seat *seat, uint32_t time, uint32_t button)
{
	struct wlr_seat_pointer_state *st = &seat->pointer_state;
	for (size_t i = 0; i < st->button_count; i++) {
		if (st->buttons[i].button == button) {
			wlr_seat_pointer_notify_button(seat, time, button,
				WL_POINTER_BUTTON_STATE_RELEASED);
			return;
		}
	}
	/* A popup's grab lets the pointer change surfaces, and wlroots forgets the
	 * buttons on every focus change, so notify_button would drop this. Press, drag
	 * onto the menu, release has to work. Through the grab rather than straight to
	 * the client, because the grab is what closes the menu on a release outside. */
	st->grab->interface->button(st->grab, time, button,
		WL_POINTER_BUTTON_STATE_RELEASED);
}

static void cursor_button(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	struct wlr_seat *seat = server->seat;
	input_activity(server);
	bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
	double cx = server->cursor->x, cy = server->cursor->y;

	/* A client that saw the press gets the release, whatever else is going on. Only
	 * those: a press that went to a frame or binding stays unknown to clients. */
	bool client_saw_it = !pressed && forwarded_take(server, event->button);
	if (client_saw_it)
		forward_release(seat, event->time_msec, event->button);

	switch (server->cursor_mode) {
	case CURSOR_MENU:
		menu_button(server, cx, cy, pressed, event->time_msec);
		if (server->cursor_mode == CURSOR_PASSTHROUGH)
			process_cursor_motion(server, event->time_msec);
		return;
	case CURSOR_MOVE:
	case CURSOR_RESIZE:
		/* Any click ends what began without a button, only its own release a drag. */
		if (server->grab_until_click ? pressed :
				!pressed && event->button == server->grab_button) {
			end_grab(server);
			process_cursor_motion(server, event->time_msec);
		}
		return;
	case CURSOR_BUTTON:
		if (!pressed && event->button == server->grab_button)
			handle_button_release(server);
		return;
	case CURSOR_PASSTHROUGH:
		break;
	}

	if (!pressed) {
		/* The implicit grab is over, the pointer may be somewhere else by now. */
		if (client_saw_it && server->n_forwarded == 0)
			process_cursor_motion(server, event->time_msec);
		return;
	}

	/* The seat's idea of what is under the pointer only changes with motion. */
	process_cursor_motion(server, event->time_msec);

	/* A click outside a client's open menu closes it, also if the click then does
	 * something of its own. That only happens through the seat. */
	if (wlr_seat_pointer_has_grab(seat) && !seat->pointer_state.focused_surface) {
		wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
			WL_POINTER_BUTTON_STATE_PRESSED);
		wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
			WL_POINTER_BUTTON_STATE_RELEASED);
		process_cursor_motion(server, event->time_msec);
	}

	/* A further button while a client has the pointer is the client's as well, not
	 * a click on whatever the pointer has been dragged over. */
	bool implicit_grab = server->n_forwarded > 0 &&
		seat->pointer_state.focused_surface && !wlr_seat_pointer_has_grab(seat);
	server->grab_button = event->button;
	if (!implicit_grab && handle_press(server, event->button, event->time_msec))
		return;
	if (server->n_forwarded < (int)(sizeof(server->forwarded_buttons) /
			sizeof(*server->forwarded_buttons)))
		server->forwarded_buttons[server->n_forwarded++] = event->button;
	server->grab_surface_x = cx - seat->pointer_state.sx;
	server->grab_surface_y = cy - seat->pointer_state.sy;
	wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
		event->state);
}

static void cursor_axis(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	input_activity(server);
	/* The wheel over the tabs of a group, one step at a time on any wheel. */
	if (server->cursor_mode == CURSOR_PASSTHROUGH &&
			!server->seat->pointer_state.focused_surface &&
			event->source == WL_POINTER_AXIS_SOURCE_WHEEL &&
			event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		struct wlr_surface *surface;
		double sx, sy;
		enum frame_part part;
		uint32_t edges;
		bool on_icon;
		struct view *view = view_at(server, server->cursor->x, server->cursor->y,
			&surface, &sx, &sy, &part, &edges, &on_icon);
		if (view && part == PART_TAB) {
			server->tab_wheel += event->delta_discrete;
			while (server->tab_wheel >= WLR_POINTER_AXIS_DISCRETE_STEP) {
				server->tab_wheel -= WLR_POINTER_AXIS_DISCRETE_STEP;
				tabs_step(view->group->shown, true);
			}
			while (server->tab_wheel <= -WLR_POINTER_AXIS_DISCRETE_STEP) {
				server->tab_wheel += WLR_POINTER_AXIS_DISCRETE_STEP;
				tabs_step(view->group->shown, false);
			}
			return;
		}
	}
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete,
		event->source, event->relative_direction);
}

static void cursor_frame(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* ---- keyboard ----------------------------------------------------------- */

/* During a move or resize: arrows nudge the pointer, and so the window. */
static void grab_key(struct server *server, uint32_t mods, xkb_keysym_t sym)
{
	int step = (mods & WLR_MODIFIER_SHIFT) ? 1 : 10;
	int dx = 0, dy = 0;
	switch (sym) {
	case XKB_KEY_Escape:
		cancel_grab(server);
		process_cursor_motion(server, 0);
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		end_grab(server);
		process_cursor_motion(server, 0);
		return;
	case XKB_KEY_Left:
		dx = -step;
		break;
	case XKB_KEY_Right:
		dx = step;
		break;
	case XKB_KEY_Up:
		dy = -step;
		break;
	case XKB_KEY_Down:
		dy = step;
		break;
	default:
		return;
	}
	wlr_cursor_warp(server->cursor, NULL, server->cursor->x + dx,
		server->cursor->y + dy);
	process_cursor_motion(server, 0);
}

static bool handle_keybinding(struct server *server, uint32_t mods,
	xkb_keysym_t sym)
{
	/* These two work whatever else is going on, or there is no way out. */
	if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
		if (server->session)
			wlr_session_change_vt(server->session,
				sym - XKB_KEY_XF86Switch_VT_1 + 1);
		return true;
	}
	if ((mods & WLR_MODIFIER_CTRL) && (mods & WLR_MODIFIER_ALT) &&
			sym == XKB_KEY_BackSpace) {
		wl_display_terminate(server->display);
		return true;
	}

	if (server->menu.open)
		return menu_key(server, sym);
	if (server->cursor_mode == CURSOR_MOVE ||
			server->cursor_mode == CURSOR_RESIZE) {
		grab_key(server, mods, sym);
		return true;
	}
	/* A frame button is held down: no other action until it is released. */
	if (server->cursor_mode == CURSOR_BUTTON)
		return false;
	if (server->exclusive_layer)
		return false;

	struct view *view = focused_view(server);
	bool alt = mods & WLR_MODIFIER_ALT;
	bool ctrl = mods & WLR_MODIFIER_CTRL;
	bool shift = mods & WLR_MODIFIER_SHIFT;
	bool logo = mods & WLR_MODIFIER_LOGO;

	if ((ctrl && alt && sym == XKB_KEY_Return) ||
			(logo && sym == XKB_KEY_Return)) {
		spawn(server->terminal);
		return true;
	}
	if (logo && !alt && !ctrl && !shift && sym == XKB_KEY_Escape) {
		const char *saver = config_get("screenSaver");
		spawn(saver ? saver : "displayOff --stars");
		return true;
	}
	if (logo && !alt && !ctrl) {
		enum snap_dir dir;
		/* Tabs, while there are any: otherwise these are the client's. */
		if (view && xkb_keysym_to_lower(sym) == XKB_KEY_g) {
			if (shift)
				tabs_detach(view);
			else if (tabs_previous(view))
				tabs_join(view, tabs_previous(view));
			return true;
		}
		if (view && view->group && sym == XKB_KEY_Tab) {
			tabs_step(view, true);
			return true;
		}
		if (view && view->group && sym == XKB_KEY_ISO_Left_Tab) {
			tabs_step(view, false);
			return true;
		}
		if (view && view->group && !shift && sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
			tabs_show_nth(view, sym - XKB_KEY_1);
			return true;
		}
		/* Letters by their lower case, or Caps Lock would turn them off. */
		switch (xkb_keysym_to_lower(sym)) {
		case XKB_KEY_t:
			if (shift)
				return false;
			tile_toggle(server);
			return true;
		case XKB_KEY_Left:
			dir = SNAP_DIR_LEFT;
			break;
		case XKB_KEY_Right:
			dir = SNAP_DIR_RIGHT;
			break;
		case XKB_KEY_Up:
			dir = SNAP_DIR_UP;
			break;
		case XKB_KEY_Down:
			dir = SNAP_DIR_DOWN;
			break;
		default:
			return false;
		}
		if (shift)
			view_move_to_edge(view, dir);
		else
			snap_step(view, dir);
		return true;
	}
	if (shift && !alt && !ctrl && sym == XKB_KEY_Escape && view) {
		/* mwm: Shift+Escape posts the window menu */
		menu_open_window(server, view, view->x + frame_border(view),
			view->y + frame_top(view));
		return true;
	}
	if (!alt || ctrl)
		return false;

	switch (sym) {
	case XKB_KEY_Tab:
		view_cycle(server, false);
		return true;
	case XKB_KEY_ISO_Left_Tab:
		view_cycle(server, true);
		return true;
	case XKB_KEY_Escape:
		view_cycle(server, true);
		return true;
	case XKB_KEY_space:
		if (view)
			menu_open_window(server, view,
				view->x + frame_border(view),
				view->y + frame_top(view));
		return true;
	case XKB_KEY_F3:
		run_action(server, ACT_LOWER, view);
		return true;
	case XKB_KEY_F4:
		run_action(server, ACT_CLOSE, view);
		return true;
	case XKB_KEY_F5:
		run_action(server, ACT_RESTORE, view);
		return true;
	case XKB_KEY_F7:
		run_action(server, ACT_MOVE, view);
		return true;
	case XKB_KEY_F8:
		run_action(server, ACT_SIZE, view);
		return true;
	case XKB_KEY_F9:
		run_action(server, ACT_MINIMIZE, view);
		return true;
	case XKB_KEY_F10:
		run_action(server, ACT_TOGGLE_MAXIMIZE, view);
		return true;
	case XKB_KEY_F11:
		if (view)
			view_set_fullscreen(view, !view->fullscreen);
		return true;
	}
	return false;
}

static void keyboard_modifiers(struct wl_listener *listener, void *data)
{
	struct keyboard *kb = wl_container_of(listener, kb, modifiers);
	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(kb->server->seat,
		&kb->wlr_keyboard->modifiers);
}

static void keyboard_key(struct wl_listener *listener, void *data)
{
	struct keyboard *kb = wl_container_of(listener, kb, key);
	struct server *server = kb->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;
	input_activity(server);

	/* No keymap at all, not even the fallback: nothing can be looked up. */
	if (!kb->wlr_keyboard->xkb_state)
		return;

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state,
		keycode, &syms);
	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* Noted beforehand: a window that gets the focus from this very key must not
		 * be told it is down, or it would wait for a release it never gets. */
		bool room = kb->n_swallowed < MAX_SWALLOWED_KEYS;
		if (room)
			kb->swallowed[kb->n_swallowed++] = event->keycode;
		for (int i = 0; i < nsyms && !handled; i++)
			handled = handle_keybinding(server, mods, syms[i]);
		if (room && !handled)
			kb->n_swallowed--;
	} else {
		for (int i = 0; i < kb->n_swallowed; i++) {
			if (kb->swallowed[i] == event->keycode) {
				kb->swallowed[i] = kb->swallowed[--kb->n_swallowed];
				handled = true;
				break;
			}
		}
	}
	if (!handled) {
		wlr_seat_set_keyboard(seat, kb->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_destroy(struct wl_listener *listener, void *data)
{
	struct keyboard *kb = wl_container_of(listener, kb, destroy);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);
	free(kb);
}

/* The environment over mwmrc, as for the other settings. */
static const char *keyboard_setting(const char *env, const char *resource)
{
	const char *value = getenv(env);
	return value && *value ? value : config_get(resource);
}

static struct xkb_rule_names keyboard_settings(void)
{
	/* Each from wherever it is set, and handed over together: xkbcommon drops a
	 * variant that comes without a layout, and looks at XKB_DEFAULT_VARIANT only when
	 * it also takes the layout from the environment. */
	const char *layout = keyboard_setting("XKB_DEFAULT_LAYOUT", "keyboardLayout");
	return (struct xkb_rule_names){
		.model = keyboard_setting("XKB_DEFAULT_MODEL", "keyboardModel"),
		.layout = layout && *layout ? layout : "us",
		.variant = keyboard_setting("XKB_DEFAULT_VARIANT", "keyboardVariant"),
		.options = keyboard_setting("XKB_DEFAULT_OPTIONS", "keyboardOptions"),
	};
}

/* What the keymaps were last made from. */
static char keymap_settings[1024];

static void keyboard_settings_text(char *buf, size_t size)
{
	const struct xkb_rule_names n = keyboard_settings();
	snprintf(buf, size, "%s|%s|%s|%s", n.model ? n.model : "", n.layout,
		n.variant ? n.variant : "", n.options ? n.options : "");
}

static void apply_keymap(struct wlr_keyboard *wlr_kb)
{
	const struct xkb_rule_names names = keyboard_settings();
	keyboard_settings_text(keymap_settings, sizeof(keymap_settings));
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &names,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	xkb_context_unref(ctx);
	if (keymap) {
		wlr_log(WLR_INFO, "keymap compiled, layout %s",
			xkb_keymap_layout_get_name(keymap, 0));
	} else {
		/* A typo in the layout must not cost the keyboard, let alone the
		 * session. Without the environment this time, it may be what failed. */
		wlr_log(WLR_ERROR, "cannot compile the keymap, falling back to the "
			"us layout");
		const struct xkb_rule_names us = { .layout = "us" };
		ctx = xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
		keymap = xkb_keymap_new_from_names(ctx, &us,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		xkb_context_unref(ctx);
	}
	if (keymap) {
		wlr_keyboard_set_keymap(wlr_kb, keymap);
		xkb_keymap_unref(keymap);
	}
}

void input_config_changed(struct server *server)
{
	/* A new keymap resets Caps Lock, NumLock and the layout group, so a reload for
	 * the sake of a colour must not bring one. */
	char now[sizeof(keymap_settings)];
	keyboard_settings_text(now, sizeof(now));
	if (!strcmp(now, keymap_settings))
		return;
	wlr_log(WLR_INFO, "keymap settings changed: %s", now);
	snprintf(keymap_settings, sizeof(keymap_settings), "%s", now);
	struct keyboard *kb;
	wl_list_for_each(kb, &server->keyboards, link) {
		/* A virtual keyboard brought its own keymap. */
		if (!wlr_input_device_get_virtual_keyboard(&kb->wlr_keyboard->base))
			apply_keymap(kb->wlr_keyboard);
	}
}

static struct keyboard *keyboard_of(struct server *server, struct wlr_keyboard *wlr_kb)
{
	struct keyboard *kb;
	wl_list_for_each(kb, &server->keyboards, link) {
		if (kb->wlr_keyboard == wlr_kb)
			return kb;
	}
	return NULL;
}

void input_keyboard_enter(struct server *server, struct wlr_surface *surface)
{
	struct wlr_seat *seat = server->seat;
	/* The grab swallows this, so it has to be made up for afterwards. */
	if (wlr_seat_keyboard_has_grab(seat))
		server->refocus_after_grab = true;
	struct wlr_keyboard *wlr_kb = wlr_seat_get_keyboard(seat);
	if (!wlr_kb) {
		wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
		return;
	}
	struct keyboard *kb = keyboard_of(server, wlr_kb);
	uint32_t keycodes[WLR_KEYBOARD_KEYS_CAP];
	size_t n = 0;
	for (size_t i = 0; i < wlr_kb->num_keycodes; i++) {
		bool swallowed = false;
		for (int j = 0; kb && j < kb->n_swallowed; j++)
			swallowed |= kb->swallowed[j] == wlr_kb->keycodes[i];
		if (!swallowed)
			keycodes[n++] = wlr_kb->keycodes[i];
	}
	wlr_seat_keyboard_notify_enter(seat, surface, keycodes, n, &wlr_kb->modifiers);
}

/* The focus moved during a popup's grab or a drag, and only now can the keyboard go
 * there. */
static void keyboard_grab_end(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, keyboard_grab_end);
	if (!server->refocus_after_grab)
		return;
	server->refocus_after_grab = false;
	struct view *view = focused_view(server);
	if (server->focused_layer)
		input_keyboard_enter(server, server->focused_layer->surface);
	else if (view && view_surface(view))
		input_keyboard_enter(server, view_surface(view));
}

static void new_keyboard(struct server *server, struct wlr_input_device *dev)
{
	struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(dev);
	struct keyboard *kb = calloc(1, sizeof(*kb));
	kb->server = server;
	kb->wlr_keyboard = wlr_kb;

	apply_keymap(wlr_kb);
	wlr_keyboard_set_repeat_info(wlr_kb, 25, 600);

	kb->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&wlr_kb->events.modifiers, &kb->modifiers);
	kb->key.notify = keyboard_key;
	wl_signal_add(&wlr_kb->events.key, &kb->key);
	kb->destroy.notify = keyboard_destroy;
	wl_signal_add(&dev->events.destroy, &kb->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_kb);
	wl_list_insert(&server->keyboards, &kb->link);
}

void input_add_device(struct server *server, struct wlr_input_device *dev)
{
	switch (dev->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		new_keyboard(server, dev);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, dev);
		break;
	default:
		break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(server->seat, caps);
}

static void new_input(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_input);
	input_add_device(server, data);
}

static void request_cursor(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (server->cursor_mode != CURSOR_PASSTHROUGH)
		return;
	if (server->seat->pointer_state.focused_client == event->seat_client)
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
}

static void request_set_selection(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void request_start_drag(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;
	if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin,
			event->serial))
		wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

static void start_drag(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, start_drag);
	struct wlr_drag *drag = data;
	/* The scene removes the icon with the drag. */
	if (drag->icon)
		wlr_scene_drag_icon_create(server->layer_drag, drag->icon);
	wlr_scene_node_set_position(&server->layer_drag->node,
		lround(server->cursor->x), lround(server->cursor->y));
}

static void request_set_primary_selection(struct wl_listener *listener,
	void *data)
{
	struct server *server = wl_container_of(listener, server,
		request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source,
		event->serial);
}

void input_init(struct server *server)
{
	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	const char *size = getenv("XCURSOR_SIZE");
	server->cursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"),
		size ? atoi(size) : 24);
	/* Xwayland asks for the default cursor before any output has needed it. */
	wlr_xcursor_manager_load(server->cursor_mgr, 1);

	server->cursor_mode = CURSOR_PASSTHROUGH;
	server->cursor_motion.notify = cursor_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute,
		&server->cursor_motion_absolute);
	server->cursor_button.notify = cursor_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = cursor_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = cursor_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	server->grab_tab = -1;
	wl_list_init(&server->keyboards);
	server->new_input.notify = new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);
	server->seat = wlr_seat_create(server->display, "seat0");
	server->request_cursor.notify = request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor,
		&server->request_cursor);
	server->request_set_selection.notify = request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection,
		&server->request_set_selection);
	server->request_start_drag.notify = request_start_drag;
	wl_signal_add(&server->seat->events.request_start_drag,
		&server->request_start_drag);
	server->start_drag.notify = start_drag;
	wl_signal_add(&server->seat->events.start_drag, &server->start_drag);
	server->keyboard_grab_end.notify = keyboard_grab_end;
	wl_signal_add(&server->seat->events.keyboard_grab_end, &server->keyboard_grab_end);
	server->request_set_primary_selection.notify =
		request_set_primary_selection;
	wl_signal_add(&server->seat->events.request_set_primary_selection,
		&server->request_set_primary_selection);
}
