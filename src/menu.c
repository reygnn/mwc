/*
 * Motif-style popup menus: the window menu (from the frame's menu button,
 * Alt+Space or a client request) and the root menu on the desktop.
 *
 * Armed items get a raised shadow rather than a colour change, mnemonics
 * are underlined and accelerators sit right-aligned, as in mwm.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <string.h>
#include <time.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include "mwc.h"

#define PAD_X 10
#define SEP_H 8
#define ACCEL_GAP 28

enum menu_action_private {
	ACT_QUIT_CONFIRM = ACT_QUIT + 1,
	ACT_RESTART_CONFIRM,
};

static PangoLayout *make_layout(cairo_t *cr, const char *text, char mnemonic)
{
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc =
		pango_font_description_from_string(theme.font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	const char *u = mnemonic ? strchr(text, mnemonic) : NULL;
	if (u) {
		char *pre = g_markup_escape_text(text, u - text);
		char *post = g_markup_escape_text(u + 1, -1);
		char *mid = g_markup_escape_text(u, 1);
		char *markup = g_strdup_printf("%s<u>%s</u>%s", pre, mid, post);
		pango_layout_set_markup(layout, markup, -1);
		g_free(pre);
		g_free(post);
		g_free(mid);
		g_free(markup);
	} else {
		pango_layout_set_text(layout, text, -1);
	}
	return layout;
}

static int text_width(cairo_t *cr, const char *text)
{
	PangoLayout *l = make_layout(cr, text, 0);
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	g_object_unref(l);
	return w;
}

static void layout_menu(struct menu *m)
{
	cairo_surface_t *tmp = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(tmp);

	int label_w = 0, accel_w = 0;
	if (m->title)
		label_w = text_width(cr, m->title);
	int y = theme.bevel + 2;
	if (m->title)
		y += theme.menu_item_height + 8;
	for (int i = 0; i < m->n_items; i++) {
		struct menu_item *it = &m->items[i];
		m->item_y[i] = y;
		if (!it->label) {
			m->item_h[i] = SEP_H;
		} else {
			m->item_h[i] = theme.menu_item_height;
			int w = text_width(cr, it->label);
			if (w > label_w)
				label_w = w;
			if (it->accel) {
				w = text_width(cr, it->accel);
				if (w > accel_w)
					accel_w = w;
			}
		}
		y += m->item_h[i];
	}
	m->height = y + theme.bevel + 2;
	m->width = 2 * theme.bevel + 2 * PAD_X + label_w +
		(accel_w ? ACCEL_GAP + accel_w : 0);
	if (m->width < 120)
		m->width = 120;

	cairo_destroy(cr);
	cairo_surface_destroy(tmp);
}

static void render_menu(struct server *server)
{
	struct menu *m = &server->menu;
	const struct mwc_shade *s = &theme.menu;
	struct cairo_buffer *buf = cairo_buffer_create(m->width, m->height);
	if (!buf)
		return;
	cairo_t *cr = buf->cr;

	set_source(cr, s->bg);
	cairo_paint(cr);
	draw_bevel(cr, 0, 0, m->width, m->height, theme.bevel, s, false);

	if (m->title) {
		int ty = theme.bevel + 2;
		draw_text(cr, m->title, theme.font, s->fg, theme.bevel,
			ty, m->width - 2 * theme.bevel, theme.menu_item_height, true);
		/* mwm puts a double separator under the menu title */
		draw_groove_h(cr, theme.bevel, ty + theme.menu_item_height,
			m->width - 2 * theme.bevel, s);
		draw_groove_h(cr, theme.bevel, ty + theme.menu_item_height + 3,
			m->width - 2 * theme.bevel, s);
	}

	struct mwc_color dim = {
		(s->bg.r + s->bottom.r) / 2, (s->bg.g + s->bottom.g) / 2,
		(s->bg.b + s->bottom.b) / 2,
	};
	for (int i = 0; i < m->n_items; i++) {
		struct menu_item *it = &m->items[i];
		int y = m->item_y[i];
		if (!it->label) {
			draw_groove_h(cr, theme.bevel, y + SEP_H / 2 - 1,
				m->width - 2 * theme.bevel, s);
			continue;
		}
		if (i == m->armed)
			draw_bevel(cr, theme.bevel, y,
				m->width - 2 * theme.bevel, theme.menu_item_height, 2, s, false);

		struct mwc_color fg = it->sensitive ? s->fg : dim;
		cairo_save(cr);
		cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
		set_source(cr, fg);
		PangoLayout *l = make_layout(cr, it->label, it->mnemonic);
		int tw, th;
		pango_layout_get_pixel_size(l, &tw, &th);
		cairo_move_to(cr, theme.bevel + PAD_X, y + (theme.menu_item_height - th) / 2);
		pango_cairo_show_layout(cr, l);
		g_object_unref(l);
		if (it->accel) {
			l = make_layout(cr, it->accel, 0);
			pango_layout_get_pixel_size(l, &tw, &th);
			cairo_move_to(cr, m->width - theme.bevel - PAD_X - tw,
				y + (theme.menu_item_height - th) / 2);
			pango_cairo_show_layout(cr, l);
			g_object_unref(l);
		}
		cairo_restore(cr);
	}

	wlr_scene_buffer_set_buffer(m->node, &buf->base);
	cairo_buffer_drop(buf);
}

static void add(struct menu *m, const char *label, char mnemonic,
	const char *accel, int action, bool sensitive)
{
	if (m->n_items >= MENU_MAX_ITEMS)
		return;
	m->items[m->n_items++] = (struct menu_item){
		label, mnemonic, accel, (enum menu_action)action, sensitive,
	};
}

static uint32_t now_msec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void open_at(struct server *server, int x, int y)
{
	struct menu *m = &server->menu;
	m->opened_msec = now_msec();
	m->open_x = server->cursor->x;
	m->open_y = server->cursor->y;
	layout_menu(m);
	struct wlr_box a = output_usable_area(server, x, y);
	if (x + m->width > a.x + a.width)
		x = a.x + a.width - m->width;
	if (y + m->height > a.y + a.height)
		y = a.y + a.height - m->height;
	if (x < a.x)
		x = a.x;
	if (y < a.y)
		y = a.y;
	m->x = x;
	m->y = y;
	m->armed = -1;
	m->open = true;
	if (!m->node)
		m->node = wlr_scene_buffer_create(server->layer_menu, NULL);
	wlr_scene_node_set_position(&m->node->node, x, y);
	wlr_scene_node_set_enabled(&m->node->node, true);
	render_menu(server);

	server->cursor_mode = CURSOR_MENU;
	cursor_set_name(server, "default");
	wlr_seat_pointer_notify_clear_focus(server->seat);
}

void menu_open_window(struct server *server, struct view *view, int x, int y)
{
	struct menu *m = &server->menu;
	/* A client can ask for this at any time, also while a grab is going on. */
	if (!view->mapped || (server->cursor_mode != CURSOR_PASSTHROUGH &&
			server->cursor_mode != CURSOR_MENU))
		return;
	if (m->open)
		menu_close(server);
	m->title = NULL;
	m->n_items = 0;
	m->target = view;
	m->from_button = false;
	view_update_properties(view);
	bool normal = !view->minimized && !view->maximized && !view->fullscreen &&
		view->snap == SNAP_NONE;
	unsigned f = view->funcs;
	if (view->tiled) {
		/* In the layout: Minimize moves the main window into the stack or
		 * shades a stack window, Maximize makes a stack window the main one,
		 * Restore unshades. */
		bool is_main = view->tile_main;
		add(m, "Restore", 'R', "Alt+F5", ACT_RESTORE, view->shaded);
		add(m, "Move", 'M', "Alt+F7", ACT_MOVE, false);
		add(m, "Size", 'S', "Alt+F8", ACT_SIZE, false);
		add(m, "Minimize", 'n', "Alt+F9", ACT_MINIMIZE, (f & FUNC_MINIMIZE) &&
			(is_main ? tile_has_stack(server) : tile_can_shade(view)));
		add(m, "Maximize", 'x', "Alt+F10", ACT_MAXIMIZE,
			(f & FUNC_MAXIMIZE) && !is_main);
		add(m, "Lower", 'L', "Alt+F3", ACT_LOWER, true);
		add(m, NULL, 0, NULL, ACT_NONE, false);
		add(m, "Close", 'C', "Alt+F4", ACT_CLOSE, f & FUNC_CLOSE);
		open_at(server, x, y);
		return;
	}
	add(m, "Restore", 'R', "Alt+F5", ACT_RESTORE, !normal);
	add(m, "Move", 'M', "Alt+F7", ACT_MOVE,
		(f & FUNC_MOVE) && !view->fullscreen);
	add(m, "Size", 'S', "Alt+F8", ACT_SIZE, (f & FUNC_RESIZE) && normal);
	add(m, "Minimize", 'n', "Alt+F9", ACT_MINIMIZE,
		(f & FUNC_MINIMIZE) && !view->minimized);
	add(m, "Maximize", 'x', "Alt+F10", ACT_MAXIMIZE,
		(f & FUNC_MAXIMIZE) && !view->maximized && !view->fullscreen);
	add(m, "Lower", 'L', "Alt+F3", ACT_LOWER, true);
	add(m, NULL, 0, NULL, ACT_NONE, false);
	add(m, "Close", 'C', "Alt+F4", ACT_CLOSE, f & FUNC_CLOSE);
	open_at(server, x, y);
}

void menu_open_root(struct server *server, int x, int y)
{
	struct menu *m = &server->menu;
	if (m->open)
		menu_close(server);
	m->title = "Root Menu";
	m->n_items = 0;
	m->target = NULL;
	m->from_button = false;
	add(m, "New Window", 'N', NULL, ACT_NEW_WINDOW, true);
	add(m, "Shuffle Up", 'U', NULL, ACT_SHUFFLE_UP, true);
	add(m, "Shuffle Down", 'D', NULL, ACT_SHUFFLE_DOWN, true);
	add(m, server->tiling ? "Untile Windows" : "Tile Windows",
		server->tiling ? 't' : 'T', "Super+T", ACT_TOGGLE_TILING, true);
	if (server->tiling) {
		int top = tile_stacking(server);
		if (top == 1)
			add(m, "Lower Tiled Windows", 'L', NULL, ACT_LOWER_TILED, true);
		else
			add(m, "Raise Tiled Windows", 'a', NULL, ACT_RAISE_TILED, top == 0);
	}
	add(m, NULL, 0, NULL, ACT_NONE, false);
	add(m, "Restart...", 'R', NULL, ACT_RESTART_CONFIRM, true);
	add(m, "Quit...", 'Q', NULL, ACT_QUIT_CONFIRM, true);
	open_at(server, x, y);
}

/* mwm asks before f.restart and f.quit_mwm. */
static void open_confirm(struct server *server, const char *title,
	enum menu_action action, int x, int y)
{
	struct menu *m = &server->menu;
	m->title = title;
	m->n_items = 0;
	m->target = NULL;
	m->from_button = false;
	add(m, "OK", 'O', NULL, action, true);
	add(m, "Cancel", 'C', NULL, ACT_NONE, true);
	open_at(server, x, y);
	m->armed = 1;
	render_menu(server);
}

void menu_close(struct server *server)
{
	struct menu *m = &server->menu;
	if (!m->open)
		return;
	m->open = false;
	m->target = NULL;
	wlr_scene_node_set_enabled(&m->node->node, false);
	if (server->cursor_mode == CURSOR_MENU) {
		server->cursor_mode = CURSOR_PASSTHROUGH;
		input_pointer_changed(server);
	}
}

static int item_at(struct menu *m, double lx, double ly)
{
	int x = (int)lx - m->x, y = (int)ly - m->y;
	if (x < 0 || x >= m->width || y < 0 || y >= m->height)
		return -2; /* outside the menu */
	for (int i = 0; i < m->n_items; i++) {
		if (y >= m->item_y[i] && y < m->item_y[i] + m->item_h[i])
			return (m->items[i].label && m->items[i].sensitive) ?
				i : -1;
	}
	return -1;
}

static void arm(struct server *server, int idx)
{
	if (server->menu.armed == idx)
		return;
	server->menu.armed = idx;
	render_menu(server);
}

static void activate(struct server *server, int idx, uint32_t time_msec)
{
	struct menu *m = &server->menu;
	enum menu_action action = m->items[idx].action;
	struct view *target = m->target;
	int x = m->x, y = m->y;
	menu_close(server);
	if ((int)action == ACT_QUIT_CONFIRM)
		open_confirm(server, "QUIT Mwc?", ACT_QUIT, x, y);
	else if ((int)action == ACT_RESTART_CONFIRM)
		open_confirm(server, "Restart Mwc?", ACT_RESTART, x, y);
	else
		run_action(server, action, target);
	(void)time_msec;
}

void menu_motion(struct server *server, double lx, double ly)
{
	int idx = item_at(&server->menu, lx, ly);
	arm(server, idx < 0 ? -1 : idx);
}

bool menu_button(struct server *server, double lx, double ly, bool pressed,
	uint32_t time_msec)
{
	struct menu *m = &server->menu;
	if (!m->open)
		return false;
	int idx = item_at(m, lx, ly);
	/* Signed: an event is stamped before we get to handle it, so it can
	 * be slightly older than the moment the menu opened. */
	int32_t age = time_msec - m->opened_msec;
	if (pressed) {
		if (idx != -2) {
			arm(server, idx);
			return true;
		}
		/* mwm: double-clicking the menu button closes the window. */
		struct view *t = m->target;
		uint32_t edges;
		bool close = m->from_button && t && age < 400 &&
			(t->funcs & FUNC_CLOSE) && frame_hit(t, (int)lx - t->x,
			(int)ly - t->y, &edges) == PART_MENU_BUTTON;
		menu_close(server);
		if (close)
			view_close(t);
		return true;
	}
	if (idx >= 0) {
		/* The release of the click that opened the menu: when the menu
		 * had to shift to fit, an item is now under the pointer. */
		if (age < 400 && fabs(lx - m->open_x) < 4 &&
				fabs(ly - m->open_y) < 4)
			return true;
		activate(server, idx, time_msec);
	} else if (idx == -2 && age > 400) {
		/* Press-drag-release that ended outside the menu. */
		menu_close(server);
	}
	return true;
}

static int step(struct menu *m, int from, int dir)
{
	for (int n = 0, i = from; n < m->n_items; n++) {
		i = (i + dir + m->n_items) % m->n_items;
		if (m->items[i].label && m->items[i].sensitive)
			return i;
	}
	return from;
}

bool menu_key(struct server *server, uint32_t sym)
{
	struct menu *m = &server->menu;
	if (!m->open)
		return false;
	switch (sym) {
	case XKB_KEY_Escape:
		menu_close(server);
		break;
	case XKB_KEY_Down:
		arm(server, step(m, m->armed < 0 ? -1 : m->armed, 1));
		break;
	case XKB_KEY_Up:
		arm(server, step(m, m->armed < 0 ? 0 : m->armed, -1));
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		if (m->armed >= 0)
			activate(server, m->armed, 0);
		break;
	default:
		for (int i = 0; i < m->n_items; i++) {
			struct menu_item *it = &m->items[i];
			if (it->label && it->sensitive && it->mnemonic &&
					xkb_keysym_to_lower(sym) ==
					xkb_keysym_to_lower(
					xkb_utf32_to_keysym(it->mnemonic))) {
				activate(server, i, 0);
				break;
			}
		}
		break;
	}
	return true;
}

/* ---- actions ------------------------------------------------------------ */

void run_action(struct server *server, enum menu_action action,
	struct view *view)
{
	struct wlr_cursor *c = server->cursor;
	switch (action) {
	case ACT_NONE:
		break;
	case ACT_RESTORE:
		if (view)
			view_restore(view);
		break;
	case ACT_MOVE:
		if (!view)
			break;
		if (view->minimized)
			view_restore(view);
		/* The pointer only jumps if there will be a grab to go with it. Restoring
		 * above may have put it into the layout, without these functions. */
		if (!(view->funcs & FUNC_MOVE) || view->fullscreen ||
				server->cursor_mode != CURSOR_PASSTHROUGH)
			break;
		/* Like f.move from a menu: the pointer jumps to the title and
		 * the window follows it until the next click. */
		wlr_cursor_warp(c, NULL, view->x + (view->width +
			frame_extra_w(view)) / 2.0,
			view->y + frame_border(view) + frame_title(view) / 2.0);
		view_begin_interactive(view, CURSOR_MOVE, 0, true);
		break;
	case ACT_SIZE:
		if (!view)
			break;
		if (view->minimized)
			view_restore(view);
		if (!(view->funcs & FUNC_RESIZE) || view->fullscreen || view->maximized ||
				server->cursor_mode != CURSOR_PASSTHROUGH)
			break;
		wlr_cursor_warp(c, NULL,
			view->x + view->width + frame_extra_w(view) - 1,
			view->y + view->height + frame_extra_h(view) - 1);
		view_begin_interactive(view, CURSOR_RESIZE,
			WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT, true);
		break;
	case ACT_MINIMIZE:
		if (view && view->tiled && (view->funcs & FUNC_MINIMIZE))
			tile_iconify(view);
		else if (view && (view->funcs & FUNC_MINIMIZE))
			view_minimize(view);
		break;
	case ACT_MAXIMIZE:
		if (view && view->tiled && (view->funcs & FUNC_MAXIMIZE)) {
			tile_promote(view);
		} else if (view && (view->funcs & FUNC_MAXIMIZE)) {
			if (view->minimized)
				view_restore(view);
			view_set_maximized(view, true);
		}
		break;
	case ACT_TOGGLE_MAXIMIZE:
		if (view && view->tiled && (view->funcs & FUNC_MAXIMIZE))
			tile_promote(view);
		else if (view && !view->tiled && !view->minimized &&
				(view->maximized || (view->funcs & FUNC_MAXIMIZE)))
			view_set_maximized(view, !view->maximized);
		break;
	case ACT_TOGGLE_TILING:
		tile_toggle(server);
		break;
	case ACT_RAISE_TILED:
	case ACT_LOWER_TILED:
		tile_stack(server, action == ACT_RAISE_TILED);
		break;
	case ACT_LOWER:
		if (view)
			view_lower(view);
		break;
	case ACT_CLOSE:
		if (view && (view->funcs & FUNC_CLOSE))
			view_close(view);
		break;
	case ACT_NEW_WINDOW:
		spawn(server->terminal);
		break;
	case ACT_SHUFFLE_UP:
		view_cycle(server, false);
		break;
	case ACT_SHUFFLE_DOWN:
		view_cycle(server, true);
		break;
	case ACT_RESTART:
		config_reload(server);
		break;
	case ACT_QUIT:
		wl_display_terminate(server->display);
		break;
	}
}
