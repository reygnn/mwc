/*
 * Pointer and keyboard handling: frame interaction, interactive move and
 * resize, mwm's default key bindings.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "mwc.h"

#define DOUBLE_CLICK_MS 400
#define DRAG_THRESHOLD 4

struct keyboard {
	struct wl_list link;
	struct server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

void spawn(const char *cmd)
{
	if (!cmd || !*cmd)
		return;
	pid_t pid = fork();
	if (pid == 0) {
		/* wl_event_loop_add_signal() blocks our handled signals, and
		 * programs would inherit that and ignore SIGTERM or SIGHUP. */
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		/* Double fork so we never collect zombies. */
		setsid();
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	} else if (pid > 0) {
		int status;
		waitpid(pid, &status, 0);
	}
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

static void resize_to(struct view *view, const struct wlr_box *b)
{
	/* An xdg client answers later, and its anchor keeps the opposite edge
	 * in place then. X windows get position and size in one configure. */
	if (view->type == VIEW_XWAYLAND)
		view_set_geometry(view, b->x, b->y, b->width, b->height);
	else
		view_send_size(view, b->width, b->height);
}

static void end_grab(struct server *server)
{
	struct view *view = server->grabbed_view;
	enum cursor_mode mode = server->cursor_mode;
	bool apply = server->grab_outline && server->grab_changed;
	bool changed = server->grab_changed;
	struct wlr_box target = server->grab_target, orig = server->grab_orig;
	enum snap zone = server->grab_zone, orig_snap = server->grab_orig_snap;
	bool orig_max = server->grab_orig_maximized;
	double px = server->cursor->x, py = server->cursor->y;
	stop_resizing_state(server);
	clear_grab(server);
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
		if (view->maximized || view->snap != SNAP_NONE)
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

void process_cursor_motion(struct server *server, uint32_t time)
{
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

static void cursor_motion(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
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
	view_focus(view);
	if (was_shaded && part != PART_MAXIMIZE_BUTTON)
		return true;

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
	case PART_TITLE:
		if (button == BTN_RIGHT) {
			menu_open_window(server, view, cx, cy);
		} else if (button == BTN_LEFT) {
			if (is_double_click(server, view, part, time)) {
				run_action(server, ACT_TOGGLE_MAXIMIZE, view);
			} else
				view_begin_interactive(view, CURSOR_MOVE, 0,
					false);
		} else if (button == BTN_MIDDLE) {
			view_lower(view);
		}
		return true;
	case PART_MENU_BUTTON:
		if (is_double_click(server, view, part, time)) {
			menu_close(server);
			if (view->funcs & FUNC_CLOSE)
				view_close(view);
		} else {
			menu_open_window(server, view,
				view->x + frame_border(view),
				view->y + frame_top(view));
		}
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

static void cursor_button(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
	double cx = server->cursor->x, cy = server->cursor->y;

	switch (server->cursor_mode) {
	case CURSOR_MENU:
		menu_button(server, cx, cy, pressed, event->time_msec);
		if (server->cursor_mode == CURSOR_PASSTHROUGH)
			process_cursor_motion(server, event->time_msec);
		return;
	case CURSOR_MOVE:
	case CURSOR_RESIZE:
		if (pressed == server->grab_until_click) {
			end_grab(server);
			process_cursor_motion(server, event->time_msec);
		}
		return;
	case CURSOR_BUTTON:
		if (!pressed)
			handle_button_release(server);
		return;
	case CURSOR_PASSTHROUGH:
		break;
	}

	if (pressed && handle_press(server, event->button, event->time_msec))
		return;
	wlr_seat_pointer_notify_button(server->seat, event->time_msec,
		event->button, event->state);
}

static void cursor_axis(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
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
	if (server->menu.open)
		return menu_key(server, sym);

	/* VT switching works regardless of modifiers. */
	if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
		if (server->session)
			wlr_session_change_vt(server->session,
				sym - XKB_KEY_XF86Switch_VT_1 + 1);
		return true;
	}
	if (server->cursor_mode == CURSOR_MOVE ||
			server->cursor_mode == CURSOR_RESIZE) {
		grab_key(server, mods, sym);
		return true;
	}

	struct view *view = focused_view(server);
	bool alt = mods & WLR_MODIFIER_ALT;
	bool ctrl = mods & WLR_MODIFIER_CTRL;
	bool shift = mods & WLR_MODIFIER_SHIFT;
	bool logo = mods & WLR_MODIFIER_LOGO;

	if (ctrl && alt && sym == XKB_KEY_BackSpace) {
		wl_display_terminate(server->display);
		return true;
	}
	if ((ctrl && alt && sym == XKB_KEY_Return) ||
			(logo && sym == XKB_KEY_Return)) {
		spawn(server->terminal);
		return true;
	}
	if (server->exclusive_layer)
		return false;
	if (logo && !alt && !ctrl) {
		enum snap_dir dir;
		switch (sym) {
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

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state,
		keycode, &syms);
	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms && !handled; i++)
			handled = handle_keybinding(server, mods, syms[i]);
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

static void new_keyboard(struct server *server, struct wlr_input_device *dev)
{
	struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(dev);
	struct keyboard *kb = calloc(1, sizeof(*kb));
	kb->server = server;
	kb->wlr_keyboard = wlr_kb;

	/* Layout from the usual XKB_DEFAULT_* environment variables. */
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_kb, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
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
	server->request_set_primary_selection.notify =
		request_set_primary_selection;
	wl_signal_add(&server->seat->events.request_set_primary_selection,
		&server->request_set_primary_selection);
}
