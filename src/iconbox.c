/*
 * The icon box, Mwm*useIconBox: the icons of the current desktop in one row or
 * column with a frame around it, instead of on the desktop. Unlike mwm's it is
 * as long as its icons need, up to Mwm*iconBoxSize icons, and scrolls from there.
 *
 *   +-------------------------------------+
 *   |  Icons                      | . | O |   <- title, minimize, maximize
 *   +-------------------------------------+
 *   | [icon] [icon] [icon] [icon] [icon]  |
 *   | < [=====slider=====]              > |   <- only when they don't all fit
 *   +-------------------------------------+
 *
 * - It stands where Mwm*iconPlacement puts the first icon, on the first output,
 *   and grows the way the icons would; "center" and "tight" as without it.
 * - Without icons, or minimized, it is a tile the size of an icon with the logo:
 *   data/logo.png, built in, or Mwm*iconBoxLogo. A click on the tile shows the box.
 * - Maximize switches between the full length and the length the icons need. It
 *   is forgotten on a desktop switch and a reload; minimized is kept per desktop.
 * - It can't be moved or resized and has no window menu.
 * - It takes the keyboard by a click or by Alt+Tab, as if it were a window in the
 *   stacking order: the arrows choose an icon, Enter restores its window.
 * - The icons stay the views' own scene buffers, placed into the box; only those
 *   in sight are enabled. Scrolling goes by whole icons.
 * - Its buttons work as a window's: CURSOR_BUTTON with no grabbed view, see input.c.
 */
#define _POSIX_C_SOURCE 200809L
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "mwc.h"
#include "logo.h"

#define LOGO_SIZE 64 /* the logo on its tile, which is ICON_W x ICON_H */
#define SB 15        /* scroll bar */

enum part {
	P_NONE,
	P_TITLE,
	P_MINIMIZE,
	P_MAXIMIZE,
	P_BACK,      /* scroll bar: the arrow towards the first icons */
	P_FORWARD,   /* and towards the last */
	P_PAGE_BACK, /* the trough between arrow and slider */
	P_PAGE_FORWARD,
	P_SLIDER,
	P_INSIDE,    /* between the icons */
};

enum arrow { ARROW_LEFT, ARROW_RIGHT, ARROW_UP, ARROW_DOWN };

struct desk {
	bool minimized; /* the logo stands for the box */
	int scroll;     /* the first icon in sight */
	int sel;        /* the chosen icon, for the keyboard */
};

static struct {
	struct server *server;
	bool enabled;    /* Mwm*useIconBox, as of the last load or reload */
	int title_w;     /* "Icons" in the theme's font */
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *chrome; /* frame, title bar, scroll bar */
	struct wlr_scene_buffer *logo;   /* the tile, drawn once per load */
	struct desk desk[MWC_DESKTOPS];
	bool maximized;
	bool focused;
	long seq;        /* its place among the windows, see iconbox_cycle_takes() */
	struct wl_event_source *refocus; /* pending, see refocus_idle() */

	/* The layout of the last arrange, for drawing and hit testing. */
	bool shown;      /* the box, not the logo */
	struct wlr_box box, logo_box;
	struct view **icons; /* in the order they became icons */
	int n, cap, visible;
	bool scrolls;
	enum part pressed;   /* minimize or maximize, held down */
	bool pressed_inside; /* and the pointer still on it */
} ib;

/* ---- settings ------------------------------------------------------------ */

bool iconbox_enabled(void)
{
	return ib.enabled;
}

static int max_icons(void)
{
	const char *v = config_get("iconBoxSize");
	if (v && *v) {
		char *end;
		long n = strtol(v, &end, 10);
		if (*end == '\0' && n >= 1 && n <= 1000)
			return (int)n;
		wlr_log(WLR_ERROR, "Mwm*iconBoxSize: expected a number of icons, not \"%s\"",
			v);
	}
	return 10;
}

struct png_reader {
	const unsigned char *data;
	size_t len, pos;
};

static cairo_status_t read_png(void *closure, unsigned char *out, unsigned int len)
{
	struct png_reader *r = closure;
	if (len > r->len - r->pos)
		return CAIRO_STATUS_READ_ERROR;
	memcpy(out, r->data + r->pos, len);
	r->pos += len;
	return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *builtin_logo(void)
{
	struct png_reader r = { logo_png, sizeof(logo_png), 0 };
	cairo_surface_t *image = cairo_image_surface_create_from_png_stream(read_png, &r);
	if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
		wlr_log(WLR_ERROR, "cannot read the built-in logo");
		cairo_surface_destroy(image);
		return NULL;
	}
	return image;
}

/* The part of the image that isn't transparent. */
static struct wlr_box visible_part(cairo_surface_t *image)
{
	int w = cairo_image_surface_get_width(image);
	int h = cairo_image_surface_get_height(image);
	struct wlr_box all = { 0, 0, w, h };
	if (cairo_image_surface_get_format(image) != CAIRO_FORMAT_ARGB32)
		return all;
	cairo_surface_flush(image);
	const unsigned char *data = cairo_image_surface_get_data(image);
	int stride = cairo_image_surface_get_stride(image);
	int x0 = w, y0 = h, x1 = -1, y1 = -1;
	for (int y = 0; y < h; y++) {
		const uint32_t *row = (const uint32_t *)(data + y * stride);
		for (int x = 0; x < w; x++) {
			if ((row[x] >> 24) == 0)
				continue;
			if (x < x0)
				x0 = x;
			if (x > x1)
				x1 = x;
			if (y < y0)
				y0 = y;
			if (y > y1)
				y1 = y;
		}
	}
	if (x1 < 0)
		return all;
	return (struct wlr_box){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
}

/* Mwm*iconBoxLogo, or the built-in one if it is unset or can't be read. */
static cairo_surface_t *read_logo(void)
{
	const char *value = config_get("iconBoxLogo");
	if (!value || !*value)
		return builtin_logo();
	char path[4096];
	config_expand_home(value, path, sizeof(path));
	cairo_surface_t *image = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
		wlr_log(WLR_ERROR, "cannot load Mwm*iconBoxLogo %s: %s", path,
			cairo_status_to_string(cairo_surface_status(image)));
		cairo_surface_destroy(image);
		return builtin_logo();
	}
	return image;
}

/* ---- geometry ------------------------------------------------------------- */

static int border(void)
{
	return theme.bevel + 1; /* a frame without resize handles */
}

static int pad(void)
{
	return icon_gap() / 2;
}

static struct desk *cur(void)
{
	return &ib.desk[ib.server->desktop];
}

static void forget_icons(void)
{
	free(ib.icons);
	ib.icons = NULL;
	ib.n = 0;
}

static void collect(void)
{
	free(ib.icons);
	ib.icons = icons_in_order(ib.server, &ib.n);
}

/* Along the row or column: the length of k icons, and of the frame around them. */
static int run_len(int k, bool horizontal)
{
	int size = horizontal ? ICON_W : ICON_H;
	return k * size + (k > 0 ? (k - 1) * icon_gap() : 0);
}

static int frame_along(bool horizontal)
{
	return 2 * border() + 2 * pad() + (horizontal ? 0 : theme.title_height);
}

/* Where something w x h goes in the area, as iconPlacement puts the first icon. */
static void anchor(struct wlr_box a, int w, int h, int *x, int *y)
{
	const struct config_icon_placement *p = config_icon_placement();
	*x = p->from_right ? a.x + a.width - ICON_MARGIN - w : a.x + ICON_MARGIN;
	*y = p->from_bottom ? a.y + a.height - ICON_MARGIN - h : a.y + ICON_MARGIN;
	if (p->center) {
		if (p->horizontal)
			*x = a.x + (a.width - w) / 2;
		else
			*y = a.y + (a.height - h) / 2;
	}
}

/* Slot k of those in sight, from the end the icons start at. */
static void slot_pos(int k, int *x, int *y)
{
	const struct config_icon_placement *p = config_icon_placement();
	int B = border(), P = pad(), T = theme.title_height, G = icon_gap();
	if (p->horizontal) {
		*y = ib.box.y + B + T + P;
		*x = p->from_right && !p->center ?
			ib.box.x + ib.box.width - B - P - ICON_W - k * (ICON_W + G) :
			ib.box.x + B + P + k * (ICON_W + G);
	} else {
		*x = ib.box.x + B + P;
		*y = p->from_bottom && !p->center ?
			ib.box.y + ib.box.height - B - P - ICON_H - k * (ICON_H + G) :
			ib.box.y + B + T + P + k * (ICON_H + G);
	}
}

/* Whether moving forward through the icons goes right or down on the screen. */
static bool forward_is_positive(void)
{
	const struct config_icon_placement *p = config_icon_placement();
	if (p->center)
		return true;
	return p->horizontal ? !p->from_right : !p->from_bottom;
}

/* ---- drawing ------------------------------------------------------------- */

static void draw_arrow(cairo_t *cr, double x, double y, double size, enum arrow dir,
	const struct mwc_shade *s)
{
	double m = size / 4, c = size / 2;
	set_source(cr, s->fg);
	switch (dir) {
	case ARROW_LEFT:
		cairo_move_to(cr, x + m, y + c);
		cairo_line_to(cr, x + size - m, y + m);
		cairo_line_to(cr, x + size - m, y + size - m);
		break;
	case ARROW_RIGHT:
		cairo_move_to(cr, x + size - m, y + c);
		cairo_line_to(cr, x + m, y + m);
		cairo_line_to(cr, x + m, y + size - m);
		break;
	case ARROW_UP:
		cairo_move_to(cr, x + c, y + m);
		cairo_line_to(cr, x + m, y + size - m);
		cairo_line_to(cr, x + size - m, y + size - m);
		break;
	case ARROW_DOWN:
		cairo_move_to(cr, x + c, y + size - m);
		cairo_line_to(cr, x + m, y + m);
		cairo_line_to(cr, x + size - m, y + m);
		break;
	}
	cairo_close_path(cr);
	cairo_fill(cr);
}

/* The parts of the scroll bar in box coordinates: trough, the two arrows, slider. */
struct sb_layout {
	struct wlr_box trough, low, high, slider; /* low: left or top */
};

static void sb_layout(struct sb_layout *l)
{
	const struct config_icon_placement *p = config_icon_placement();
	int B = border(), T = theme.title_height, W = ib.box.width, H = ib.box.height;
	if (p->horizontal)
		l->trough = (struct wlr_box){ B, H - B - SB, W - 2 * B, SB };
	else
		l->trough = (struct wlr_box){ W - B - SB, B + T, SB, H - 2 * B - T };
	struct wlr_box t = l->trough;
	int len = p->horizontal ? t.width : t.height;
	int track = len - 2 * SB;
	int range = ib.n - ib.visible;
	int sl = range > 0 ? track * ib.visible / ib.n : track;
	if (sl < SB)
		sl = SB;
	if (sl > track)
		sl = track;
	int pos = range > 0 ? (track - sl) * cur()->scroll / range : 0;
	if (!forward_is_positive())
		pos = track - sl - pos;
	if (p->horizontal) {
		l->low = (struct wlr_box){ t.x, t.y, SB, SB };
		l->high = (struct wlr_box){ t.x + t.width - SB, t.y, SB, SB };
		l->slider = (struct wlr_box){ t.x + SB + pos, t.y, sl, SB };
	} else {
		l->low = (struct wlr_box){ t.x, t.y, SB, SB };
		l->high = (struct wlr_box){ t.x, t.y + t.height - SB, SB, SB };
		l->slider = (struct wlr_box){ t.x, t.y + SB + pos, SB, sl };
	}
}

struct title_parts {
	struct wlr_box title, min, max;
};

static void title_parts(struct title_parts *tp)
{
	int B = border(), T = theme.title_height, W = ib.box.width;
	tp->max = (struct wlr_box){ W - B - T, B, T, T };
	tp->min = (struct wlr_box){ W - B - 2 * T, B, T, T };
	tp->title = (struct wlr_box){ B, B, W - 2 * B - 2 * T, T };
}

static void render_chrome(void)
{
	const struct config_icon_placement *p = config_icon_placement();
	int W = ib.box.width, H = ib.box.height, B = border(), T = theme.title_height;
	struct cairo_buffer *buf = cairo_buffer_create(W, H);
	if (!buf)
		return;
	cairo_t *cr = buf->cr;
	const struct mwc_shade *s = ib.focused ? &theme.active : &theme.inactive;
	set_source(cr, s->bg);
	cairo_paint(cr);
	draw_bevel(cr, 0, 0, W, H, theme.bevel < B ? theme.bevel : B, s, false);

	/* Title bar: title | minimize | maximize, as on a window without a menu. */
	struct title_parts tp;
	title_parts(&tp);
	draw_bevel(cr, tp.title.x, tp.title.y, tp.title.width, tp.title.height, 1, s,
		false);
	/* The whole word or nothing, too narrow a column has an empty title bar. */
	if (ib.title_w <= tp.title.width - 8)
		draw_text(cr, "Icons", theme.font, s->fg, tp.title.x + 4, tp.title.y + 1,
			tp.title.width - 8, T - 2, true);
	draw_title_button(cr, PART_MINIMIZE_BUTTON, tp.min.x, tp.min.y, T, s,
		ib.pressed == P_MINIMIZE && ib.pressed_inside, false);
	draw_title_button(cr, PART_MAXIMIZE_BUTTON, tp.max.x, tp.max.y, T, s,
		ib.pressed == P_MAXIMIZE && ib.pressed_inside, ib.maximized);

	/* The well the icons stand in. */
	int wx = B, wy = B + T, ww = W - 2 * B, wh = H - 2 * B - T;
	if (ib.scrolls) {
		if (p->horizontal)
			wh -= SB;
		else
			ww -= SB;
	}
	draw_bevel(cr, wx, wy, ww, wh, 1, s, true);

	if (ib.scrolls) {
		struct sb_layout l;
		sb_layout(&l);
		struct mwc_color trough = color_mix(s->bg, s->bottom, 0.5f);
		set_source(cr, trough);
		cairo_rectangle(cr, l.trough.x, l.trough.y, l.trough.width, l.trough.height);
		cairo_fill(cr);
		draw_bevel(cr, l.trough.x, l.trough.y, l.trough.width, l.trough.height, 1, s,
			true);
		set_source(cr, s->bg);
		cairo_rectangle(cr, l.low.x, l.low.y, SB, SB);
		cairo_rectangle(cr, l.high.x, l.high.y, SB, SB);
		cairo_rectangle(cr, l.slider.x, l.slider.y, l.slider.width, l.slider.height);
		cairo_fill(cr);
		draw_bevel(cr, l.low.x, l.low.y, SB, SB, 1, s, false);
		draw_bevel(cr, l.high.x, l.high.y, SB, SB, 1, s, false);
		draw_arrow(cr, l.low.x, l.low.y, SB, p->horizontal ? ARROW_LEFT : ARROW_UP, s);
		draw_arrow(cr, l.high.x, l.high.y, SB, p->horizontal ? ARROW_RIGHT : ARROW_DOWN,
			s);
		draw_bevel(cr, l.slider.x, l.slider.y, l.slider.width, l.slider.height, 1, s,
			false);
	}
	wlr_scene_buffer_set_buffer(ib.chrome, &buf->base);
	cairo_buffer_drop(buf);
	wlr_scene_node_set_position(&ib.chrome->node, ib.box.x, ib.box.y);
}

/* The tile with the logo's visible part as large as fits, in the middle. A margin in
 * the file would only make it smaller. image may be NULL: an empty tile. */
static void render_logo(cairo_surface_t *image)
{
	struct cairo_buffer *buf = cairo_buffer_create(ICON_W, ICON_H);
	if (!buf)
		return;
	cairo_t *cr = buf->cr;
	const struct mwc_shade *s = &theme.inactive;
	set_source(cr, s->bg);
	cairo_paint(cr);
	draw_bevel(cr, 0, 0, ICON_W, ICON_H, 2, s, false);
	double lx = (ICON_W - LOGO_SIZE) / 2.0, ly = (ICON_H - LOGO_SIZE) / 2.0;
	if (image) {
		struct wlr_box c = visible_part(image);
		double scale = (double)LOGO_SIZE / (c.width > c.height ? c.width : c.height);
		cairo_save(cr);
		cairo_translate(cr, lx + (LOGO_SIZE - c.width * scale) / 2,
			ly + (LOGO_SIZE - c.height * scale) / 2);
		cairo_scale(cr, scale, scale);
		cairo_rectangle(cr, 0, 0, c.width, c.height);
		cairo_clip(cr);
		cairo_set_source_surface(cr, image, -c.x, -c.y);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	wlr_scene_buffer_set_buffer(ib.logo, &buf->base);
	cairo_buffer_drop(buf);
}

/* ---- arranging ----------------------------------------------------------- */

/* A button held down on a box that changes under it: nothing comes of the release. */
static void cancel_button(void)
{
	if (ib.pressed == P_NONE)
		return;
	ib.pressed = P_NONE;
	struct server *server = ib.server;
	if (server->cursor_mode == CURSOR_BUTTON && !server->grabbed_view)
		server->cursor_mode = CURSOR_PASSTHROUGH;
}

static void hide_all(void)
{
	cancel_button();
	wlr_scene_node_set_enabled(&ib.chrome->node, false);
	wlr_scene_node_set_enabled(&ib.logo->node, false);
	ib.shown = false;
	ib.box = ib.logo_box = (struct wlr_box){ 0 };
}

/* The box had the keyboard and lost it by going away: once what made it go is through,
 * a window gets it, unless one has by then (a window restored from the box). */
static void refocus_idle(void *data)
{
	struct server *server = data;
	ib.refocus = NULL;
	if (!ib.focused && !view_focused(server) && !server->focused_layer)
		view_focus_top(server);
}

static void set_focused(bool focused);

static void lose_focus(void)
{
	if (!ib.focused)
		return;
	set_focused(false);
	if (!ib.refocus)
		ib.refocus = wl_event_loop_add_idle(ib.server->event_loop, refocus_idle,
			ib.server);
}

static void clamp_scroll(struct desk *d)
{
	int range = ib.n - ib.visible;
	if (d->scroll > range)
		d->scroll = range;
	if (d->scroll < 0)
		d->scroll = 0;
	if (d->sel >= ib.n)
		d->sel = ib.n - 1;
	if (d->sel < 0)
		d->sel = 0;
}

void iconbox_arrange(struct server *server)
{
	collect();
	struct desk *d = cur();
	struct view *v;
	/* Icons of other desktops, and those out of sight, stay off. */
	wl_list_for_each(v, &server->views, link)
		wlr_scene_node_set_enabled(&v->icon_node->node, false);
	struct output *o = output_first(server);
	if (!o) {
		hide_all();
		return;
	}
	struct wlr_box a = o->usable;
	const struct config_icon_placement *p = config_icon_placement();
	bool hz = p->horizontal;

	if (ib.n == 0 || d->minimized) {
		if (ib.n == 0)
			d->minimized = false;
		cancel_button();
		lose_focus();
		ib.shown = false;
		ib.box = (struct wlr_box){ 0 };
		wlr_scene_node_set_enabled(&ib.chrome->node, false);
		int x, y;
		anchor(a, ICON_W, ICON_H, &x, &y);
		ib.logo_box = (struct wlr_box){ x, y, ICON_W, ICON_H };
		wlr_scene_node_set_position(&ib.logo->node, x, y);
		wlr_scene_node_set_enabled(&ib.logo->node, true);
		return;
	}

	/* How many fit along the edge, with the margins. */
	int along = (hz ? a.width : a.height) - 2 * ICON_MARGIN - frame_along(hz);
	int size = hz ? ICON_W : ICON_H;
	int fit = (along + icon_gap()) / (size + icon_gap());
	if (fit < 1)
		fit = 1;
	ib.cap = max_icons() < fit ? max_icons() : fit;
	ib.visible = ib.maximized ? ib.cap : (ib.n < ib.cap ? ib.n : ib.cap);
	ib.scrolls = ib.n > ib.visible;
	clamp_scroll(d);

	int B = border(), P = pad(), T = theme.title_height;
	int w, h;
	if (hz) {
		w = 2 * B + 2 * P + run_len(ib.visible, true);
		h = 2 * B + T + 2 * P + ICON_H + (ib.scrolls ? SB : 0);
	} else {
		w = 2 * B + 2 * P + ICON_W + (ib.scrolls ? SB : 0);
		h = 2 * B + T + 2 * P + run_len(ib.visible, false);
	}
	/* Room for the buttons and some title, also with a large font. */
	if (w < 2 * B + 3 * T)
		w = 2 * B + 3 * T;
	int x, y;
	anchor(a, w, h, &x, &y);
	ib.box = (struct wlr_box){ x, y, w, h };
	ib.logo_box = (struct wlr_box){ 0 };
	ib.shown = true;
	wlr_scene_node_set_enabled(&ib.logo->node, false);
	render_chrome();
	wlr_scene_node_set_enabled(&ib.chrome->node, true);

	for (int i = 0; i < ib.n; i++) {
		v = ib.icons[i];
		int k = i - d->scroll;
		bool in_sight = k >= 0 && k < ib.visible;
		if (in_sight) {
			slot_pos(k, &v->icon_x, &v->icon_y);
			v->icon_placed = true;
			wlr_scene_node_set_position(&v->icon_node->node, v->icon_x, v->icon_y);
		}
		wlr_scene_node_set_enabled(&v->icon_node->node, in_sight);
	}
}

/* ---- state --------------------------------------------------------------- */

bool iconbox_selected(struct view *view)
{
	if (!ib.enabled || !ib.focused || !ib.shown)
		return false;
	struct desk *d = cur();
	return d->sel >= 0 && d->sel < ib.n && ib.icons[d->sel] == view;
}

static void set_focused(bool focused)
{
	if (ib.focused == focused)
		return;
	ib.focused = focused;
	if (ib.shown) {
		render_chrome();
		/* The chosen icon shows only while the box has the keyboard. */
		struct desk *d = cur();
		if (d->sel >= 0 && d->sel < ib.n)
			icon_render(ib.icons[d->sel]);
	}
}

static bool focusable(void)
{
	return ib.enabled && ib.shown && ib.n > 0;
}

bool iconbox_has_focus(void)
{
	return ib.focused;
}

void iconbox_focus(struct server *server)
{
	if (!focusable())
		return;
	struct view *prev = view_focused(server);
	if (prev)
		view_set_activated(prev, false);
	ib.seq = ++server->raise_hi;
	if (!server->exclusive_layer) {
		server->focused_layer = NULL;
		wlr_seat_keyboard_notify_clear_focus(server->seat);
	}
	set_focused(true);
}

void iconbox_blur(void)
{
	set_focused(false);
}

void iconbox_desktop_changed(void)
{
	ib.maximized = false;
	cancel_button();
	set_focused(false);
}

void iconbox_config_changed(struct server *server)
{
	bool was = ib.enabled;
	ib.enabled = config_bool("useIconBox", config_get("useIconBox")) == 1;
	ib.maximized = false;
	if (ib.enabled) {
		int h;
		text_extent(theme.font, "Icons", &ib.title_w, &h);
		cairo_surface_t *image = read_logo();
		render_logo(image);
		if (image)
			cairo_surface_destroy(image);
		return;
	}
	set_focused(false);
	hide_all();
	forget_icons();
	if (was) {
		/* The icons go back onto the desktop, those of this one in sight. */
		struct view *v;
		wl_list_for_each(v, &server->views, link)
			wlr_scene_node_set_enabled(&v->icon_node->node,
				v->mapped && v->minimized && !v->off_desktop);
	}
}

/* Alt+Tab: the box has a place among the windows, as if it were one. Forward, the
 * bottom one comes to the front: that is the box when it is below next. Backward,
 * the current one goes to the back and the top one gets the focus. */
bool iconbox_cycle_takes(struct view *next, bool backwards)
{
	if (ib.focused || !focusable())
		return false;
	if (!next)
		return true;
	return backwards ? ib.seq > next->raise_seq : ib.seq < next->raise_seq;
}

void iconbox_lowered(struct server *server)
{
	ib.seq = --server->raise_lo;
}

void iconbox_scroll(int steps)
{
	struct desk *d = cur();
	int before = d->scroll;
	d->scroll += steps;
	clamp_scroll(d);
	if (d->scroll != before)
		iconbox_arrange(ib.server);
}

static void select_icon(int i)
{
	struct desk *d = cur();
	if (i < 0 || i >= ib.n || i == d->sel)
		return;
	struct view *old = d->sel >= 0 && d->sel < ib.n ? ib.icons[d->sel] : NULL;
	d->sel = i;
	if (d->sel < d->scroll)
		d->scroll = d->sel;
	else if (d->sel >= d->scroll + ib.visible)
		d->scroll = d->sel - ib.visible + 1;
	iconbox_arrange(ib.server);
	if (old)
		icon_render(old);
	icon_render(ib.icons[d->sel]);
}

bool iconbox_key(struct server *server, uint32_t mods, uint32_t sym)
{
	/* Num Lock and Caps Lock don't count. */
	const uint32_t held = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT |
		WLR_MODIFIER_LOGO;
	if (!ib.focused || !ib.shown || (mods & held) || server->focused_layer)
		return false;
	const struct config_icon_placement *p = config_icon_placement();
	struct desk *d = cur();
	int dir = 0; /* +1: right or down on the screen */
	switch (sym) {
	case XKB_KEY_Left:
		dir = p->horizontal ? -1 : 0;
		break;
	case XKB_KEY_Right:
		dir = p->horizontal ? 1 : 0;
		break;
	case XKB_KEY_Up:
		dir = p->horizontal ? 0 : -1;
		break;
	case XKB_KEY_Down:
		dir = p->horizontal ? 0 : 1;
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (d->sel >= 0 && d->sel < ib.n)
			view_restore(ib.icons[d->sel]);
		return true;
	default:
		return false;
	}
	if (dir)
		select_icon(d->sel + (forward_is_positive() ? dir : -dir));
	return true;
}

/* ---- the pointer ----------------------------------------------------------- */

static bool in_box(struct wlr_box b, double lx, double ly)
{
	return b.width > 0 && lx >= b.x && lx < b.x + b.width && ly >= b.y &&
		ly < b.y + b.height;
}

static enum part part_at(double lx, double ly)
{
	if (!ib.shown || !in_box(ib.box, lx, ly))
		return P_NONE;
	int bx = (int)(lx - ib.box.x), by = (int)(ly - ib.box.y);
	struct title_parts tp;
	title_parts(&tp);
	if (in_box(tp.min, bx, by))
		return P_MINIMIZE;
	if (in_box(tp.max, bx, by))
		return P_MAXIMIZE;
	if (in_box(tp.title, bx, by))
		return P_TITLE;
	if (ib.scrolls) {
		struct sb_layout l;
		sb_layout(&l);
		bool fwd_high = forward_is_positive();
		if (in_box(l.low, bx, by))
			return fwd_high ? P_BACK : P_FORWARD;
		if (in_box(l.high, bx, by))
			return fwd_high ? P_FORWARD : P_BACK;
		if (in_box(l.slider, bx, by))
			return P_SLIDER;
		if (in_box(l.trough, bx, by)) {
			bool before = config_icon_placement()->horizontal ?
				bx < l.slider.x : by < l.slider.y;
			return before == fwd_high ? P_PAGE_BACK : P_PAGE_FORWARD;
		}
	}
	return P_INSIDE;
}

bool iconbox_contains(double lx, double ly)
{
	return ib.enabled && (in_box(ib.box, lx, ly) || in_box(ib.logo_box, lx, ly));
}

bool iconbox_title_at(double lx, double ly)
{
	return ib.enabled && part_at(lx, ly) == P_TITLE;
}

bool iconbox_scrollable_at(double lx, double ly)
{
	return ib.enabled && ib.shown && in_box(ib.box, lx, ly);
}

bool iconbox_press(struct server *server, uint32_t button, double lx, double ly,
	bool double_click)
{
	if (!ib.enabled)
		return false;
	if (!ib.shown && in_box(ib.logo_box, lx, ly)) {
		/* The tile shows the box again, if there is anything in it. */
		if (button == BTN_LEFT && ib.n > 0) {
			cur()->minimized = false;
			iconbox_arrange(server);
			iconbox_focus(server);
			tile_arrange(server); /* the box is wider than the tile */
		}
		return true;
	}
	enum part part = part_at(lx, ly);
	if (part == P_NONE)
		return false;
	iconbox_focus(server);
	if (button != BTN_LEFT)
		return true;
	switch (part) {
	case P_MINIMIZE:
	case P_MAXIMIZE:
		/* As on a window: it acts on the release, if that is still on it. */
		ib.pressed = part;
		ib.pressed_inside = true;
		server->cursor_mode = CURSOR_BUTTON;
		render_chrome();
		break;
	case P_TITLE:
		/* A double click maximizes, as on a window. */
		if (double_click) {
			ib.maximized = !ib.maximized;
			iconbox_arrange(server);
		}
		break;
	case P_BACK:
		iconbox_scroll(-1);
		break;
	case P_FORWARD:
		iconbox_scroll(1);
		break;
	case P_PAGE_BACK:
		iconbox_scroll(-ib.visible);
		break;
	case P_PAGE_FORWARD:
		iconbox_scroll(ib.visible);
		break;
	default:
		break;
	}
	return true;
}

void iconbox_button_motion(double lx, double ly)
{
	bool inside = part_at(lx, ly) == ib.pressed;
	if (inside != ib.pressed_inside) {
		ib.pressed_inside = inside;
		render_chrome();
	}
}

void iconbox_button_release(struct server *server)
{
	enum part part = ib.pressed;
	bool inside = ib.pressed_inside;
	ib.pressed = P_NONE;
	server->cursor_mode = CURSOR_PASSTHROUGH;
	if (!inside) {
		if (ib.shown)
			render_chrome();
		return;
	}
	if (part == P_MINIMIZE) {
		cur()->minimized = true;
		set_focused(false);
		/* The keyboard goes back to a window. */
		view_focus_top(server);
	} else if (part == P_MAXIMIZE) {
		ib.maximized = !ib.maximized;
	}
	iconbox_arrange(server);
	tile_arrange(server);
}

/* A click on one of its icons: it is the chosen one, and the box has the keyboard. */
void iconbox_icon_pressed(struct view *view)
{
	if (!ib.enabled || !ib.shown)
		return;
	struct server *server = view->server;
	for (int i = 0; i < ib.n; i++) {
		if (ib.icons[i] == view) {
			iconbox_focus(server);
			select_icon(i);
			return;
		}
	}
}

/* What the tiling layout leaves free for it, as for the icon row: the box or tile. */
bool iconbox_rect(struct wlr_box *out)
{
	if (!ib.enabled)
		return false;
	*out = ib.shown ? ib.box : ib.logo_box;
	return out->width > 0;
}

void iconbox_state(FILE *f)
{
	struct desk *d = cur();
	fprintf(f, "iconbox on=%d shown=%d logo=%d x=%d y=%d w=%d h=%d n=%d visible=%d "
		"scroll=%d sel=%d focused=%d max=%d min=%d\n", ib.enabled, ib.shown,
		!ib.shown && ib.logo_box.width > 0, ib.shown ? ib.box.x : ib.logo_box.x,
		ib.shown ? ib.box.y : ib.logo_box.y,
		ib.shown ? ib.box.width : ib.logo_box.width,
		ib.shown ? ib.box.height : ib.logo_box.height, ib.n, ib.shown ? ib.visible : 0,
		d->scroll, d->sel, ib.focused, ib.maximized, d->minimized);
}

void iconbox_init(struct server *server)
{
	ib.server = server;
	ib.tree = wlr_scene_tree_create(server->layer_icons);
	ib.chrome = wlr_scene_buffer_create(ib.tree, NULL);
	ib.logo = wlr_scene_buffer_create(ib.tree, NULL);
	wlr_scene_node_set_enabled(&ib.chrome->node, false);
	wlr_scene_node_set_enabled(&ib.logo->node, false);
	/* Below every window, as if it had been lowered: Alt+Tab reaches it last. */
	iconbox_lowered(server);
	iconbox_config_changed(server);
}
