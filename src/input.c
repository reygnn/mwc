/*
 * Pointer and keyboard handling: what a press on a frame, an icon or the desktop
 * does, and the keys. A move or resize once begun is grab.c's; which key means what
 * is in keys.c.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "mwc.h"

#define DOUBLE_CLICK_MS 400
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
	/* The key that woke the monitors, while it is held; 0: none. Keys pressed with it
	 * only wake too: Super+Return in the dark must not open a terminal. */
	uint32_t waking_key;
	/* An arrow key held in a menu, a keyboard move or the icon box: wlroots repeats
	 * keys for clients only, so mwc does it for its own. */
	struct wl_event_source *repeat;
	uint32_t repeat_keycode;
	xkb_keysym_t repeat_sym;
};

void cursor_set_name(struct server *server, const char *name)
{
	server->client_shape = NULL;
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

/* ---- pointer ------------------------------------------------------------ */

static void process_cursor_motion(struct server *server, uint32_t time);

void input_pointer_changed(struct server *server)
{
	if (server->grab.mode == CURSOR_PASSTHROUGH)
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

bool input_pointer_on(struct server *server, struct wlr_surface *surface)
{
	struct wlr_surface *focus = server->seat->pointer_state.focused_surface;
	return surface && focus && wlr_surface_get_root_surface(focus) == surface;
}

static void process_cursor_motion(struct server *server, uint32_t time)
{
	/* Only while a drag runs: wlroots walks the whole scene for each move of a
	 * node, even of an empty tree. */
	if (server->seat->drag)
		wlr_scene_node_set_position(&server->layer_drag->node,
			lround(server->cursor->x), lround(server->cursor->y));
	switch (server->grab.mode) {
	case CURSOR_MOVE:
	case CURSOR_RESIZE:
		grab_motion(server);
		return;
	case CURSOR_MENU:
		menu_motion(server, server->cursor->x, server->cursor->y);
		return;
	case CURSOR_BUTTON: {
		struct view *v = server->grab.view;
		if (!v) {
			iconbox_button_motion(server->cursor->x, server->cursor->y);
			return;
		}
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
	 * wherever it goes, or a text selection ends at the window's edge. Drags and
	 * popups have grabs of their own, which have to see what is under the pointer. */
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
	outputs_wake(server);
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
	outputs_wake(server);
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
	/* A double click must not chain into a triple click, also not on the icon box,
	 * which goes by no view. */
	server->last_click_view = dbl ? NULL : view;
	server->last_click_part = dbl ? PART_NONE : part;
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

	/* The icon box's frame, scroll bar and logo; its icons are the views' own. Its
	 * title bar counts as a part of no window for double clicks. */
	if (!view && !surface && iconbox_contains(cx, cy)) {
		bool dbl = button == BTN_LEFT && is_double_click(server, NULL,
			iconbox_title_at(cx, cy) ? PART_TITLE : PART_NONE, time);
		if (iconbox_press(server, button, cx, cy, dbl))
			return true;
	}

	/* Between the docked windows: nothing to do there, and not the desktop. */
	if (!view && !surface && dock_contains(cx, cy))
		return true;

	if (!view) {
		/* A panel: not ours to handle. */
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
		iconbox_icon_pressed(view);
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
			if (server->grab.mode == CURSOR_RESIZE)
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
			if (server->grab.mode == CURSOR_MOVE) {
				struct view *one = tabs_nth(view, tab);
				grab_take_one(server, one ? one : view);
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
				if (server->grab.mode == CURSOR_MOVE)
					grab_click_shows_tab(server, tab);
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
		grab_hold_button(server, view);
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
	struct view *v = server->grab.view;
	enum frame_part part = v->pressed;
	bool inside = v->pressed_inside;
	v->pressed = PART_NONE;
	v->pressed_inside = false;
	grab_release_button(server);
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
	/* A click in the dark only wakes: it would hit what nobody sees, and so would its
	 * release, in an open menu. */
	if (pressed && outputs_wake(server)) {
		server->wake_button = event->button;
		return;
	}
	if (!pressed && server->wake_button && event->button == server->wake_button) {
		server->wake_button = 0;
		return;
	}
	double cx = server->cursor->x, cy = server->cursor->y;

	/* A client that saw the press gets the release, whatever else is going on. Only
	 * those: a press that went to a frame or binding stays unknown to clients. */
	bool client_saw_it = !pressed && forwarded_take(server, event->button);
	if (client_saw_it)
		forward_release(seat, event->time_msec, event->button);

	switch (server->grab.mode) {
	case CURSOR_MENU:
		menu_button(server, cx, cy, pressed, event->time_msec);
		if (server->grab.mode == CURSOR_PASSTHROUGH)
			process_cursor_motion(server, event->time_msec);
		return;
	case CURSOR_MOVE:
	case CURSOR_RESIZE:
		/* Any click ends what began without a button, only its own release a drag. */
		if (server->grab.until_click ? pressed :
				!pressed && event->button == server->grab.button) {
			grab_end(server);
			process_cursor_motion(server, event->time_msec);
		}
		return;
	case CURSOR_BUTTON:
		/* A frame button, or with no window one of the icon box's. */
		if (!pressed && event->button == server->grab.button) {
			if (server->grab.view)
				handle_button_release(server);
			else
				iconbox_button_release(server);
			/* Released off the button: the pointer is over something else now. */
			process_cursor_motion(server, event->time_msec);
		}
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
	if (wlr_seat_pointer_has_grab(seat) && !seat->drag &&
			!seat->pointer_state.focused_surface) {
		wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
			WL_POINTER_BUTTON_STATE_PRESSED);
		wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
			WL_POINTER_BUTTON_STATE_RELEASED);
		process_cursor_motion(server, event->time_msec);
	}

	/* A further button while a client has the pointer is the client's as well, not
	 * a click on whatever the pointer has been dragged over. So during its drag. */
	bool implicit_grab = seat->drag || (server->n_forwarded > 0 &&
		seat->pointer_state.focused_surface && !wlr_seat_pointer_has_grab(seat));
	grab_pressed(server, event->button);
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

/* Whole wheel steps from discrete values, which may come in fractions of one; the
 * rest waits in *acc for the next event. */
static int wheel_steps(int *acc, int32_t discrete)
{
	*acc += discrete;
	int steps = *acc / WLR_POINTER_AXIS_DISCRETE_STEP;
	*acc -= steps * WLR_POINTER_AXIS_DISCRETE_STEP;
	return steps;
}

static void cursor_axis(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	input_activity(server);
	outputs_wake(server);
	/* The wheel over the tabs of a group, one step at a time on any wheel. */
	if (server->grab.mode == CURSOR_PASSTHROUGH &&
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
		/* Over the icon box, its icons included, the wheel scrolls it. */
		if ((!view || on_icon) && !surface &&
				iconbox_scrollable_at(server->cursor->x, server->cursor->y)) {
			iconbox_scroll(wheel_steps(&server->icon_wheel, event->delta_discrete));
			return;
		}
		if (view && part == PART_TAB) {
			int steps = wheel_steps(&server->tab_wheel, event->delta_discrete);
			for (; steps > 0; steps--)
				tabs_step(view->group->shown, true);
			for (; steps < 0; steps++)
				tabs_step(view->group->shown, false);
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
		grab_cancel(server);
		process_cursor_motion(server, 0);
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		grab_end(server);
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

/* What keys.c says a key means, done. view: the focused window, or NULL. */
static bool run_key(struct server *server, struct key_command cmd, struct view *view)
{
	static const struct {
		const char *resource, *fallback;
	} shots[2][2] = {
		{ { "clipboardRectangleScreenshot", "screenShot -c -r" },
		  { "clipboardScreenshot", "screenShot -c" } },
		{ { "rectangleScreenshot", "screenShot -r" },
		  { "screenshot", "screenShot" } },
	};
	switch (cmd.action) {
	case KEY_NONE:
		return false;
	case KEY_SWITCH_VT:
		if (server->session)
			wlr_session_change_vt(server->session, cmd.arg);
		break;
	case KEY_QUIT:
		wl_display_terminate(server->display);
		break;
	case KEY_TERMINAL:
		spawn(terminal_command(server));
		break;
	case KEY_SCREEN_SAVER: {
		const char *saver = config_get("screenSaver");
		spawn(saver ? saver : "displayOff --stars");
		break;
	}
	case KEY_SCREENSHOT: {
		bool file = cmd.arg & KEY_SHOT_FILE, whole = cmd.arg & KEY_SHOT_WHOLE;
		const char *shot = config_get(shots[file][whole].resource);
		spawn(shot ? shot : shots[file][whole].fallback);
		break;
	}
	case KEY_TAB_JOIN:
		if (tabs_previous(view))
			tabs_join(view, tabs_previous(view));
		break;
	case KEY_TAB_DETACH:
		tabs_detach(view);
		break;
	case KEY_TAB_NEXT:
		tabs_step(view, true);
		break;
	case KEY_TAB_PREVIOUS:
		tabs_step(view, false);
		break;
	case KEY_TAB_NTH:
		tabs_show_nth(view, cmd.arg);
		break;
	case KEY_TILE_TOGGLE:
		tile_toggle(server);
		break;
	case KEY_SNAP:
		snap_step(view, cmd.arg);
		break;
	case KEY_EDGE:
		view_move_to_edge(view, cmd.arg);
		break;
	case KEY_WINDOW_MENU:
		if (view)
			menu_open_window(server, view, view->x + frame_border(view),
				view->y + frame_top(view));
		break;
	case KEY_CYCLE:
		view_cycle(server, false);
		break;
	case KEY_CYCLE_BACK:
		view_cycle(server, true);
		break;
	case KEY_MENU_ACTION:
		run_action(server, cmd.arg, view);
		break;
	case KEY_FULLSCREEN:
		if (view)
			view_set_fullscreen(view, !view->fullscreen);
		break;
	}
	return true;
}

static bool handle_keybinding(struct server *server, uint32_t mods,
	xkb_keysym_t sym)
{
	/* The VT keys and quitting work whatever else is going on, or there is no way
	 * out. They need no window. */
	struct key_command cmd = keys_lookup(mods, sym, false, false);
	if (cmd.always)
		return run_key(server, cmd, NULL);

	/* Its keys are its own, even with a menu or a grab from the mouse going on. Those
	 * still end with the mouse. */
	if (server->exclusive_layer)
		return false;
	if (server->menu.open)
		return menu_key(server, sym);
	if (server->grab.mode == CURSOR_MOVE ||
			server->grab.mode == CURSOR_RESIZE) {
		grab_key(server, mods, sym);
		return true;
	}
	/* A frame button is held down: no other action until it is released. */
	if (server->grab.mode == CURSOR_BUTTON)
		return false;
	if (iconbox_key(server, mods, sym))
		return true;

	struct view *view = view_focused(server);
	return run_key(server, keys_lookup(mods, sym, view != NULL, view && view->group), view);
}

static void keyboard_modifiers(struct wl_listener *listener, void *data)
{
	struct keyboard *kb = wl_container_of(listener, kb, modifiers);
	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(kb->server->seat,
		&kb->wlr_keyboard->modifiers);
}

static bool is_arrow(xkb_keysym_t sym)
{
	return sym == XKB_KEY_Left || sym == XKB_KEY_Right || sym == XKB_KEY_Up ||
		sym == XKB_KEY_Down;
}

/* Where mwc steers with the arrows itself. */
static bool navigating(struct server *server)
{
	return server->menu.open || server->grab.mode == CURSOR_MOVE ||
		server->grab.mode == CURSOR_RESIZE || iconbox_has_focus();
}

static void repeat_stop(struct keyboard *kb)
{
	kb->repeat_keycode = 0;
	if (kb->repeat)
		wl_event_source_timer_update(kb->repeat, 0);
}

static int repeat_fire(void *data)
{
	struct keyboard *kb = data;
	struct server *server = kb->server;
	struct wlr_keyboard *wlr_kb = kb->wlr_keyboard;
	/* Whatever the arrows steered may have closed meanwhile. */
	if (!kb->repeat_keycode || !navigating(server) || wlr_kb->repeat_info.rate <= 0 ||
			!handle_keybinding(server, wlr_keyboard_get_modifiers(wlr_kb),
				kb->repeat_sym)) {
		kb->repeat_keycode = 0;
		return 0;
	}
	wl_event_source_timer_update(kb->repeat, 1000 / wlr_kb->repeat_info.rate);
	return 0;
}

static void repeat_start(struct keyboard *kb, uint32_t keycode, xkb_keysym_t sym)
{
	struct wlr_keyboard *wlr_kb = kb->wlr_keyboard;
	if (!kb->repeat)
		kb->repeat = wl_event_loop_add_timer(kb->server->event_loop, repeat_fire, kb);
	if (!kb->repeat || wlr_kb->repeat_info.rate <= 0)
		return;
	kb->repeat_keycode = keycode;
	kb->repeat_sym = sym;
	wl_event_source_timer_update(kb->repeat, wlr_kb->repeat_info.delay);
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
	/* Any other key, or this one going up, ends a repeat. */
	if (kb->repeat_keycode && (event->state == WL_KEYBOARD_KEY_STATE_PRESSED ||
			event->keycode == kb->repeat_keycode))
		repeat_stop(kb);
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* Noted beforehand: a window that gets the focus from this very key must not
		 * be told it is down, or it would wait for a release it never gets. */
		bool room = kb->n_swallowed < MAX_SWALLOWED_KEYS;
		if (room)
			kb->swallowed[kb->n_swallowed++] = event->keycode;
		/* A key in the dark only wakes, its release with it. */
		handled = kb->waking_key != 0;
		if (!handled && outputs_wake(server)) {
			kb->waking_key = event->keycode;
			handled = true;
		}
		for (int i = 0; i < nsyms && !handled; i++) {
			bool steering = navigating(server) && is_arrow(syms[i]);
			handled = handle_keybinding(server, mods, syms[i]);
			if (handled && steering)
				repeat_start(kb, event->keycode, syms[i]);
		}
		if (room && !handled)
			kb->n_swallowed--;
	} else {
		if (event->keycode == kb->waking_key)
			kb->waking_key = 0;
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

static void update_capabilities(struct server *server)
{
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(server->seat, caps);
}

static void keyboard_destroy(struct wl_listener *listener, void *data)
{
	struct keyboard *kb = wl_container_of(listener, kb, destroy);
	struct server *server = kb->server;
	if (kb->repeat)
		wl_event_source_remove(kb->repeat);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);
	free(kb);
	/* The last one gone: clients must not count on a keyboard. */
	update_capabilities(server);
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

/* One keymap for every keyboard: wlroots compares keymaps by pointer, and each
 * switch between keyboard devices with different ones sends the keymap to every
 * client again. Built anew when its settings change. */
static struct xkb_keymap *shared_keymap;

static struct xkb_keymap *compile_keymap(void)
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
		return keymap;
	}
	/* A typo in the layout must not cost the keyboard, let alone the session.
	 * Without the environment this time, it may be what failed. */
	wlr_log(WLR_ERROR, "cannot compile the keymap, falling back to the us layout");
	const struct xkb_rule_names us = { .layout = "us" };
	ctx = xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
	keymap = xkb_keymap_new_from_names(ctx, &us, XKB_KEYMAP_COMPILE_NO_FLAGS);
	xkb_context_unref(ctx);
	return keymap;
}

static void apply_keymap(struct wlr_keyboard *wlr_kb)
{
	if (!shared_keymap)
		shared_keymap = compile_keymap();
	if (shared_keymap)
		wlr_keyboard_set_keymap(wlr_kb, shared_keymap);
}

/* Mwm*keyboardRepeatRate and keyboardRepeatDelay; rate 0 turns repeating off. A delay
 * of 0 would disarm the timer of mwc's own repeat, a rate over 1000 its interval. */
static int repeat_setting(const char *resource, long min, long max, int def)
{
	const char *v = config_get(resource);
	if (v && *v) {
		char *end;
		long n = strtol(v, &end, 10);
		if (*end == '\0' && n >= min && n <= max)
			return (int)n;
		wlr_log(WLR_ERROR, "Mwm*%s: expected %ld to %ld, not \"%s\"", resource, min,
			max, v);
	}
	return def;
}

/* Every keyboard, virtual ones too: the seat's tells the clients. */
static void apply_repeat(struct wlr_keyboard *wlr_kb)
{
	wlr_keyboard_set_repeat_info(wlr_kb, repeat_setting("keyboardRepeatRate", 0, 1000, 25),
		repeat_setting("keyboardRepeatDelay", 1, 10000, 600));
}

void input_config_changed(struct server *server)
{
	struct keyboard *kb;
	/* wlroots sends nothing when it stays the same. */
	wl_list_for_each(kb, &server->keyboards, link)
		apply_repeat(kb->wlr_keyboard);

	/* A new keymap resets Caps Lock, NumLock and the layout group, so a reload for
	 * the sake of a colour must not bring one. */
	char now[sizeof(keymap_settings)];
	keyboard_settings_text(now, sizeof(now));
	if (!strcmp(now, keymap_settings))
		return;
	wlr_log(WLR_INFO, "keymap settings changed: %s", now);
	xkb_keymap_unref(shared_keymap);
	shared_keymap = compile_keymap();
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

void input_end_client_grab(struct server *server)
{
	if (!server->seat->drag)
		wlr_seat_pointer_end_grab(server->seat);
}

/* The focus moved during a popup's grab or a drag, and only now can the keyboard go
 * there. */
static void keyboard_grab_end(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, keyboard_grab_end);
	if (!server->refocus_after_grab)
		return;
	server->refocus_after_grab = false;
	struct view *view = view_focused(server);
	if (server->focused_layer)
		input_keyboard_enter(server, server->focused_layer->surface);
	else if (view && view_surface(view))
		input_keyboard_enter(server, view_surface(view));
}

static void new_keyboard(struct server *server, struct wlr_input_device *dev)
{
	struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(dev);
	struct keyboard *kb = calloc(1, sizeof(*kb));
	if (!kb) {
		wlr_log(WLR_ERROR, "out of memory: keyboard %s not used", dev->name);
		return;
	}
	kb->server = server;
	kb->wlr_keyboard = wlr_kb;

	apply_keymap(wlr_kb);
	apply_repeat(wlr_kb);

	kb->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&wlr_kb->events.modifiers, &kb->modifiers);
	kb->key.notify = keyboard_key;
	wl_signal_add(&wlr_kb->events.key, &kb->key);
	kb->destroy.notify = keyboard_destroy;
	wl_signal_add(&dev->events.destroy, &kb->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_kb);
	wl_list_insert(&server->keyboards, &kb->link);
}

static void input_add_device(struct server *server, struct wlr_input_device *dev)
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
	update_capabilities(server);
}

static void new_input(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, new_input);
	input_add_device(server, data);
}

static void client_cursor_set(struct server *server, struct wlr_surface *surface)
{
	wl_list_remove(&server->client_cursor_destroy.link);
	wl_list_init(&server->client_cursor_destroy.link);
	server->client_cursor = surface;
	if (surface)
		wl_signal_add(&surface->events.destroy, &server->client_cursor_destroy);
}

static void client_cursor_destroy(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		client_cursor_destroy);
	client_cursor_set(server, NULL);
}

static void request_cursor(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (server->grab.mode != CURSOR_PASSTHROUGH)
		return;
	if (server->seat->pointer_state.focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
		client_cursor_set(server, event->surface);
		server->client_shape = NULL;
	}
}

/* cursor-shape-v1: a client names its pointer image, mwc takes it from the theme. As
 * with an image of its own: only the client under the pointer, which none is while mwc
 * moves, resizes or shows a menu. Tablets have no pointer of their own here. */
static void request_set_shape(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, request_set_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER ||
			server->seat->pointer_state.focused_client != event->seat_client)
		return;
	const char *name = wlr_cursor_shape_v1_name(event->shape);
	client_cursor_set(server, NULL);
	server->client_shape = name;
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, name);
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
	spawn_init(server);
	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	const char *size = getenv("XCURSOR_SIZE");
	server->cursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"),
		size ? atoi(size) : 24);
	/* The default cursor may be asked for before any output has needed it. */
	wlr_xcursor_manager_load(server->cursor_mgr, 1);

	grab_init(server);
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
	server->client_cursor_destroy.notify = client_cursor_destroy;
	wl_list_init(&server->client_cursor_destroy.link);
	wl_signal_add(&server->seat->events.request_set_cursor,
		&server->request_cursor);
	struct wlr_cursor_shape_manager_v1 *shapes =
		wlr_cursor_shape_manager_v1_create(server->display, 1);
	server->request_set_shape.notify = request_set_shape;
	wl_signal_add(&shapes->events.request_set_shape, &server->request_set_shape);
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

/* The listeners only: the cursor itself goes after the backend, see main(). */
void input_fini(struct server *server)
{
	/* A client's popup may have asked for this, and nothing will run it any more. */
	if (server->pointer_refresh)
		wl_event_source_remove(server->pointer_refresh);
	server->pointer_refresh = NULL;
	input_virtual_fini();
	wl_list_remove(&server->cursor_motion.link);
	wl_list_remove(&server->cursor_motion_absolute.link);
	wl_list_remove(&server->cursor_button.link);
	wl_list_remove(&server->cursor_axis.link);
	wl_list_remove(&server->cursor_frame.link);
	wl_list_remove(&server->new_input.link);
	wl_list_remove(&server->request_cursor.link);
	wl_list_remove(&server->request_set_shape.link);
	wl_list_remove(&server->client_cursor_destroy.link);
	wl_list_remove(&server->request_set_selection.link);
	wl_list_remove(&server->request_set_primary_selection.link);
	wl_list_remove(&server->request_start_drag.link);
	wl_list_remove(&server->start_drag.link);
	wl_list_remove(&server->keyboard_grab_end.link);
}

/* ---- virtual input --------------------------------------------------------- */

/* zwp_virtual_keyboard_v1 and zwlr_virtual_pointer_v1 let a client type and
 * click as the user: what a VNC server such as wayvnc needs, and the tests.
 * Off unless MWC_VIRTUAL_INPUT or MWC_TEST_STATE asks for them, because any
 * client could use them. */
static struct server *virtual_server;
static struct wl_listener new_virtual_keyboard, new_virtual_pointer;

static void on_new_virtual_keyboard(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_keyboard_v1 *kb = data;
	input_add_device(virtual_server, &kb->keyboard.base);
}

static void on_new_virtual_pointer(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	input_add_device(virtual_server, &event->new_pointer->pointer.base);
	/* Its absolute motion is in that output's coordinates (wayrdp sharing one). */
	if (event->suggested_output)
		wlr_cursor_map_input_to_output(virtual_server->cursor,
			&event->new_pointer->pointer.base, event->suggested_output);
}

void input_virtual_init(struct server *server)
{
	if (virtual_server)
		return;
	virtual_server = server;
	struct wlr_virtual_keyboard_manager_v1 *vk =
		wlr_virtual_keyboard_manager_v1_create(server->display);
	new_virtual_keyboard.notify = on_new_virtual_keyboard;
	wl_signal_add(&vk->events.new_virtual_keyboard, &new_virtual_keyboard);
	struct wlr_virtual_pointer_manager_v1 *vp =
		wlr_virtual_pointer_manager_v1_create(server->display);
	new_virtual_pointer.notify = on_new_virtual_pointer;
	wl_signal_add(&vp->events.new_virtual_pointer, &new_virtual_pointer);
	wlr_log(WLR_INFO, "virtual keyboard and pointer protocols on");
}

void input_virtual_fini(void)
{
	if (!virtual_server)
		return;
	wl_list_remove(&new_virtual_keyboard.link);
	wl_list_remove(&new_virtual_pointer.link);
	virtual_server = NULL;
}
