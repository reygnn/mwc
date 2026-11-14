/*
 * Motif window frame and icons, drawn with cairo into scene buffers.
 *
 *   +--+------------------------------------+--+
 *   |  |                                    |  |  <- resize border with
 *   |  +---+------------------------+---+---+  |     etched corner grooves
 *   |  | = |         Title          | . | O |  |  <- menu, title, min, max
 *   |  +---+------------------------+---+---+  |
 *   |  |                                    |  |
 *   |  |             client                 |  |
 *   ...
 */
#define _POSIX_C_SOURCE 200809L
#include <pango/pangocairo.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "mwc.h"

static unsigned frame_decor(struct view *view)
{
	return view->decorated ? view->decor : 0;
}

int frame_border(struct view *view)
{
	unsigned d = frame_decor(view);
	if (d & DECOR_RESIZEH)
		return theme.border;
	if (d & DECOR_BORDER)
		return theme.bevel + 1;
	return 0;
}

int frame_title(struct view *view)
{
	return (frame_decor(view) & DECOR_TITLE) ? theme.title_height : 0;
}

int frame_left(struct view *view)
{
	return frame_border(view);
}

int frame_top(struct view *view)
{
	return frame_border(view) + frame_title(view);
}

int frame_extra_w(struct view *view)
{
	return 2 * frame_border(view);
}

int frame_extra_h(struct view *view)
{
	return 2 * frame_border(view) + frame_title(view);
}

int frame_height(struct view *view)
{
	return view->shaded && view->tiled && frame_title(view) ? frame_extra_h(view) :
		view->height + frame_extra_h(view);
}

struct title_layout {
	int menu_x, title_x, title_w, min_x, max_x;
};

static void title_layout(struct view *view, struct title_layout *tl)
{
	unsigned d = frame_decor(view);
	int B = frame_border(view), T = theme.title_height;
	int left = B, right = B + view->width;
	tl->menu_x = tl->min_x = tl->max_x = -1;

	/* Drop the buttons a narrow window has no room for, minimize first, so
	 * drawing and hit testing never see them overlap. */
	static const unsigned order[] = {
		DECOR_MINIMIZE, DECOR_MAXIMIZE, DECOR_MENU,
	};
	int n = !!(d & DECOR_MENU) + !!(d & DECOR_MINIMIZE) +
		!!(d & DECOR_MAXIMIZE);
	for (int i = 0; i < 3 && n * T > view->width; i++) {
		if (d & order[i]) {
			d &= ~order[i];
			n--;
		}
	}
	if (d & DECOR_MENU) {
		tl->menu_x = left;
		left += T;
	}
	if (d & DECOR_MAXIMIZE) {
		right -= T;
		tl->max_x = right;
	}
	if (d & DECOR_MINIMIZE) {
		right -= T;
		tl->min_x = right;
	}
	tl->title_x = left;
	tl->title_w = right - left;
}

/* The middle of the title bar between the buttons, from the frame's top-left corner.
 * Without a title bar, the middle of the frame. */
void frame_title_center(struct view *view, int *x, int *y)
{
	if (!frame_title(view)) {
		*x = (view->width + frame_extra_w(view)) / 2;
		*y = (view->height + frame_extra_h(view)) / 2;
		return;
	}
	struct title_layout tl;
	title_layout(view, &tl);
	*x = tl.title_x + tl.title_w / 2;
	*y = frame_border(view) + theme.title_height / 2;
}

/* How far the resize corners reach along each side of a W x H frame: B+T, as mwm's
 * CornerWidth() and CornerHeight(), and like them at most a third of the frame, so
 * a piece of side is always left between two corners. Hit testing and the grooves
 * agree. */
static void corner_reach(int W, int H, int *kx, int *ky)
{
	int k = theme.border + theme.title_height;
	*kx = 3 * k > W ? W / 3 : k;
	*ky = 3 * k > H ? H / 3 : k;
}

void draw_text(cairo_t *cr, const char *text, const char *font,
	struct mwc_color color, double x, double y, double w, double h,
	bool center)
{
	if (!text || !*text || w <= 0)
		return;
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);

	/* WM_NAME is Latin-1 by the book, and wlroots passes on its bytes. */
	char *converted = NULL;
	if (!g_utf8_validate(text, -1, NULL)) {
		converted = g_convert(text, -1, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
		if (converted)
			text = converted;
	}

	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	g_free(converted);
	pango_layout_set_single_paragraph_mode(layout, true);
	pango_layout_set_width(layout, (int)(w * PANGO_SCALE));
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	double tx = center ? x + (w - tw) / 2 : x;
	set_source(cr, color);
	cairo_move_to(cr, tx, y + (h - th) / 2);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	cairo_restore(cr);
}

/* ---- tabs ---------------------------------------------------------------- */

struct tab_strip {
	int x, w;        /* where the first drawn tab starts, the width of each */
	int first, fit;  /* the tabs drawn */
	int arrow;       /* width of the marks for those that aren't, or 0 */
};

/* Evenly over the title's area. When they don't all fit at a useful width, as many as
 * do, moved along so that the shown one is among them. */
static void tab_strip_layout(struct view *view, const struct title_layout *tl,
	struct tab_strip *st)
{
	struct tab_group *g = view->group;
	const int n = wl_list_length(&g->members), min = 3 * theme.title_height;
	*st = (struct tab_strip){ .x = tl->title_x, .fit = n };
	if (n * min > tl->title_w) {
		st->arrow = theme.title_height / 2;
		st->x += st->arrow;
		st->fit = (tl->title_w - 2 * st->arrow) / min;
		if (st->fit < 1)
			st->fit = 1;
		int shown = tabs_index(g->shown);
		if (g->first_visible > shown)
			g->first_visible = shown;
		if (g->first_visible + st->fit <= shown)
			g->first_visible = shown - st->fit + 1;
		if (g->first_visible > n - st->fit)
			g->first_visible = n - st->fit;
		st->first = g->first_visible;
	}
	st->w = (tl->title_w - 2 * st->arrow) / st->fit;
}

int frame_tab_at(struct view *view, int fx)
{
	if (!view->group)
		return -1;
	struct title_layout tl;
	title_layout(view, &tl);
	struct tab_strip st;
	tab_strip_layout(view, &tl, &st);
	if (fx < st.x || fx >= st.x + st.fit * st.w)
		return -1;
	return st.first + (fx - st.x) / st.w;
}

static void draw_mark(cairo_t *cr, double x, double y, double size, bool left,
	const struct mwc_shade *s)
{
	set_source(cr, s->fg);
	cairo_move_to(cr, left ? x + size : x, y);
	cairo_line_to(cr, left ? x : x + size, y + size);
	cairo_line_to(cr, left ? x + size : x, y + 2 * size);
	cairo_close_path(cr);
	cairo_fill(cr);
}

/* The shown one sunken, in the colours of the frame. The others raised, each in its own
 * unfocused colours, so a window stays recognisable in a group of any colour. */
static void draw_tabs(cairo_t *cr, struct view *view, const struct title_layout *tl,
	int y, int T, const struct mwc_shade *s)
{
	struct tab_group *g = view->group;
	struct tab_strip st;
	tab_strip_layout(view, tl, &st);
	draw_bevel(cr, tl->title_x, y, tl->title_w, T, 1, s, false);
	int i = 0;
	struct view *m;
	wl_list_for_each(m, &g->members, tab_link) {
		int slot = i++ - st.first;
		if (slot < 0 || slot >= st.fit)
			continue;
		bool shown = m == g->shown;
		const struct mwc_shade *ms = shown ? s : &m->shade_inactive;
		int x = st.x + slot * st.w;
		set_source(cr, ms->bg);
		cairo_rectangle(cr, x, y, st.w, T);
		cairo_fill(cr);
		draw_bevel(cr, x, y, st.w, T, 1, ms, shown);
		draw_text(cr, view_title(m), theme.font, ms->fg, x + 4, y + 1, st.w - 8, T - 2,
			true);
	}
	if (st.arrow) {
		const double size = T / 4.0, ym = y + T / 2.0 - size;
		if (st.first > 0)
			draw_mark(cr, tl->title_x + 1, ym, size, true, s);
		if (st.first + st.fit < wl_list_length(&g->members))
			draw_mark(cr, tl->title_x + tl->title_w - size - 1, ym, size, false, s);
	}
}

/* ---- the frame ------------------------------------------------------------ */

static void draw_button(cairo_t *cr, struct view *view, enum frame_part part,
	int x, int y, int size, const struct mwc_shade *s)
{
	bool down = view->pressed == part && view->pressed_inside;
	draw_bevel(cr, x, y, size, size, 1, s, down);

	/* Glyphs are themselves little raised bevels, as in mwm. */
	int g;
	switch (part) {
	case PART_MENU_BUTTON: {
		int gw = size * 11 / 20, gh = 4;
		draw_bevel(cr, x + (size - gw) / 2, y + (size - gh) / 2,
			gw, gh, 1, s, false);
		break;
	}
	case PART_MINIMIZE_BUTTON:
		g = 5;
		draw_bevel(cr, x + (size - g) / 2, y + (size - g) / 2,
			g, g, 1, s, false);
		break;
	case PART_MAXIMIZE_BUTTON:
		g = size * 11 / 20;
		draw_bevel(cr, x + (size - g) / 2, y + (size - g) / 2,
			g, g, 1, s, view->maximized && !view->tiled);
		break;
	default:
		break;
	}
}

/* The whole W x H frame, in frame coordinates: each strip draws all of it, and cairo
 * keeps what falls on the strip. The title bar is only on the top one. */
static void draw_frame(cairo_t *cr, struct view *view, int W, int H, bool title)
{
	const int B = frame_border(view), T = title ? frame_title(view) : 0;
	const int bv = theme.bevel < B ? theme.bevel : B;
	const bool full = B == theme.border;
	const struct mwc_shade *s = view->activated ?
		&view->shade_active : &view->shade_inactive;

	set_source(cr, s->bg);
	cairo_paint(cr);

	if (B) {
		/* Outer raised edge, inner sunken edge around title + client. */
		draw_bevel(cr, 0, 0, W, H, bv, s, false);
		if (full)
			draw_bevel(cr, B - 1, B - 1, W - 2 * B + 2,
				H - 2 * B + 2, 1, s, true);
	}

	/* Resize corner grooves, cutting the border at title-bar distance. As in mwm's
	 * StretcherCorner(), each corner ends in its own shadow and the side piece starts
	 * with its light edge: dark on k - 1, light on k. That is where the menu and the
	 * maximize button meet the title, and the title bar meets the window, so the
	 * grooves go on from those lines. Tiled windows keep them: the decoration stays,
	 * inactive. */
	if (full && ((view->funcs & FUNC_RESIZE) || view->tiled) &&
			(!view->maximized || view->tiled)) {
		int kx, ky;
		corner_reach(W, H, &kx, &ky);
		draw_groove_v(cr, kx - 1, bv, B - bv - 1, s);
		draw_groove_v(cr, W - kx - 1, bv, B - bv - 1, s);
		draw_groove_v(cr, kx - 1, H - B + 1, B - bv - 1, s);
		draw_groove_v(cr, W - kx - 1, H - B + 1, B - bv - 1, s);
		draw_groove_h(cr, bv, ky - 1, B - bv - 1, s);
		draw_groove_h(cr, W - B + 1, ky - 1, B - bv - 1, s);
		draw_groove_h(cr, bv, H - ky - 1, B - bv - 1, s);
		draw_groove_h(cr, W - B + 1, H - ky - 1, B - bv - 1, s);
	}

	/* Title bar: menu button | title | minimize | maximize */
	if (T) {
		struct title_layout tl;
		title_layout(view, &tl);
		if (tl.menu_x >= 0)
			draw_button(cr, view, PART_MENU_BUTTON, tl.menu_x, B, T, s);
		if (view->group) {
			draw_tabs(cr, view, &tl, B, T, s);
		} else {
			draw_bevel(cr, tl.title_x, B, tl.title_w, T, 1, s,
				view->pressed == PART_TITLE);
			draw_text(cr, view_title(view), theme.font, s->fg,
				tl.title_x + 4, B + 1, tl.title_w - 8, T - 2, true);
		}
		if (tl.min_x >= 0)
			draw_button(cr, view, PART_MINIMIZE_BUTTON, tl.min_x, B, T, s);
		if (tl.max_x >= 0)
			draw_button(cr, view, PART_MAXIMIZE_BUTTON, tl.max_x, B, T, s);
	}
}

void frame_render(struct view *view)
{
	const int B = frame_border(view), T = frame_title(view);
	const int W = view->width + 2 * B, H = frame_height(view);
	/* Top with the title bar, bottom, left, right; the sides between the two. A
	 * shaded window has no sides, and one without a frame no strips at all. */
	const struct wlr_box strips[4] = {
		{ 0, 0, W, B + T },
		{ 0, H - B, W, B },
		{ 0, B + T, B, H - 2 * B - T },
		{ W - B, B + T, B, H - 2 * B - T },
	};
	for (int i = 0; i < 4; i++) {
		struct wlr_scene_buffer *node = view->frame_nodes[i];
		const struct wlr_box *r = &strips[i];
		struct cairo_buffer *buf = r->width > 0 && r->height > 0 ?
			cairo_buffer_create(r->width, r->height) : NULL;
		if (!buf) {
			wlr_scene_node_set_enabled(&node->node, false);
			continue;
		}
		cairo_translate(buf->cr, -r->x, -r->y);
		draw_frame(buf->cr, view, W, H, i == 0);
		wlr_scene_buffer_set_buffer(node, &buf->base);
		cairo_buffer_drop(buf);
		wlr_scene_node_set_position(&node->node, r->x, r->y);
		wlr_scene_node_set_enabled(&node->node, true);
	}
	/* Between the strips, behind the client. */
	const int fw = W - 2 * B, fh = H - 2 * B - T;
	struct wlr_scene_rect *fill = view->frame_fill;
	if ((!B && !T) || fw <= 0 || fh <= 0) {
		wlr_scene_node_set_enabled(&fill->node, false);
		return;
	}
	const struct mwc_color bg = (view->activated ?
		&view->shade_active : &view->shade_inactive)->bg;
	wlr_scene_rect_set_color(fill, (const float[4]){ bg.r, bg.g, bg.b, 1 });
	wlr_scene_rect_set_size(fill, fw, fh);
	wlr_scene_node_set_position(&fill->node, B, B + T);
	wlr_scene_node_set_enabled(&fill->node, true);
}

enum frame_part frame_hit(struct view *view, int fx, int fy, uint32_t *edges)
{
	*edges = 0;
	const int B = frame_border(view), T = frame_title(view);
	if (!B && !T)
		return PART_CLIENT;
	const int W = view->width + 2 * B;
	const int H = frame_height(view);

	if (fx < 0 || fy < 0 || fx >= W || fy >= H)
		return PART_NONE;
	if (fx >= B && fx < W - B && fy >= B + T && fy < H - B)
		return PART_CLIENT;
	if (T && fx >= B && fx < W - B && fy >= B && fy < B + T) {
		struct title_layout tl;
		title_layout(view, &tl);
		if (tl.menu_x >= 0 && fx < tl.menu_x + T)
			return PART_MENU_BUTTON;
		if (tl.max_x >= 0 && fx >= tl.max_x)
			return PART_MAXIMIZE_BUTTON;
		if (tl.min_x >= 0 && fx >= tl.min_x && (tl.max_x < 0 || fx < tl.max_x))
			return PART_MINIMIZE_BUTTON;
		return view->group ? PART_TAB : PART_TITLE;
	}
	/* A border that can't resize moves the window, like the title. */
	if (B != theme.border || !(view->funcs & FUNC_RESIZE) || view->maximized)
		return PART_TITLE;

	/* Border: the corner pieces extend B+T along each side, but no further
	 * than a third of the frame, or a short window would be at its top and bottom
	 * edge at once. */
	int kx, ky;
	corner_reach(W, H, &kx, &ky);
	bool in_lr = fx < B || fx >= W - B;
	bool in_tb = fy < B || fy >= H - B;
	if (fx < B || (in_tb && fx < kx))
		*edges |= WLR_EDGE_LEFT;
	else if (fx >= W - B || (in_tb && fx >= W - kx))
		*edges |= WLR_EDGE_RIGHT;
	if (fy < B || (in_lr && fy < ky))
		*edges |= WLR_EDGE_TOP;
	else if (fy >= H - B || (in_lr && fy >= H - ky))
		*edges |= WLR_EDGE_BOTTOM;
	return PART_RESIZE;
}

/* ---- icons -------------------------------------------------------------- */

#define LABEL_H 18

void icon_render(struct view *view)
{
	/* Icons wear the client's colours like its frame does. */
	const struct mwc_shade *s = &view->shade_inactive;
	struct cairo_buffer *buf = cairo_buffer_create(ICON_W, ICON_H);
	if (!buf)
		return;
	cairo_t *cr = buf->cr;
	int img_h = ICON_H - LABEL_H;

	set_source(cr, s->bg);
	cairo_paint(cr);
	draw_bevel(cr, 0, 0, ICON_W, img_h, 2, s, false);
	/* Chosen with the arrows in the icon box: the label sunken and highlighted. */
	bool selected = iconbox_selected(view);
	if (selected) {
		set_source(cr, theme.select);
		cairo_rectangle(cr, 0, img_h, ICON_W, LABEL_H);
		cairo_fill(cr);
	}
	draw_bevel(cr, 0, img_h, ICON_W, LABEL_H, 2, s, selected);

	/* Default icon image: a miniature Motif window in a sunken well. */
	int wx = 6, wy = 6, ww = ICON_W - 12, wh = img_h - 12;
	set_source(cr, theme.root);
	cairo_rectangle(cr, wx, wy, ww, wh);
	cairo_fill(cr);
	draw_bevel(cr, wx, wy, ww, wh, 1, s, true);
	const struct mwc_shade *a = &view->shade_active;
	int mx = wx + 8, my = wy + 6, mw = ww - 16, mh = wh - 12;
	set_source(cr, a->bg);
	cairo_rectangle(cr, mx, my, mw, mh);
	cairo_fill(cr);
	draw_bevel(cr, mx, my, mw, mh, 1, a, false);
	draw_bevel(cr, mx + 2, my + 2, mw - 4, 7, 1, a, false);
	set_source(cr, theme.inactive.bg);
	cairo_rectangle(cr, mx + 2, my + 10, mw - 4, mh - 12);
	cairo_fill(cr);

	const char *label = view_app_id(view);
	if (!label || !*label)
		label = view_title(view);
	draw_text(cr, label, theme.font, s->fg, 3, img_h + 1, ICON_W - 6,
		LABEL_H - 2, true);

	wlr_scene_buffer_set_buffer(view->icon_node, &buf->base);
	cairo_buffer_drop(buf);
}

static bool icon_slot_taken(struct server *server, struct view *self,
	int x, int y)
{
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		/* Each desktop has its own slots. */
		if (v != self && v->minimized && v->icon_placed &&
				v->desktop == self->desktop &&
				v->icon_x == x && v->icon_y == y)
			return true;
	}
	return false;
}

/* Where slot k of a row or column starts, along it, from one end. */
static int along_pos(int start, int len, int size, int gap, int k, bool from_end)
{
	return from_end ? start + len - 8 - size - k * (size + gap) :
		start + 8 + k * (size + gap);
}

/* How many icons a row or column holds. */
static int icons_per_line(const struct config_icon_placement *p, struct wlr_box area,
	int gap)
{
	int along = p->horizontal ? area.width : area.height;
	int size = p->horizontal ? ICON_W : ICON_H;
	int n = (along - 16 + gap) / (size + gap);
	return n < 1 ? 1 : n;
}

/* Slot i of the usable area, as Mwm*iconPlacement lays them out: along a row or
 * a column from one corner, the next row or column further in. mwm's default
 * "left bottom" fills the bottom row from the left. */
static void icon_slot(struct wlr_box area, int i, int *x, int *y)
{
	const struct config_icon_placement *p = config_icon_placement();
	int gap = p->tight ? 0 : 8;
	int per_line = icons_per_line(p, area, gap);
	int k = i % per_line, line = i / per_line;
	if (p->horizontal) {
		*x = along_pos(area.x, area.width, ICON_W, gap, k, p->from_right);
		*y = along_pos(area.y, area.height, ICON_H, gap, line, p->from_bottom);
	} else {
		*x = along_pos(area.x, area.width, ICON_W, gap, line, p->from_right);
		*y = along_pos(area.y, area.height, ICON_H, gap, k, p->from_bottom);
	}
}

static int by_icon_seq(const void *a, const void *b)
{
	const struct view *x = *(struct view *const *)a, *y = *(struct view *const *)b;
	return (x->icon_seq > y->icon_seq) - (x->icon_seq < y->icon_seq);
}

/* With center: the icons in sight on each output stand as one block in the middle of
 * its edge, in the order they first became icons; a full row or column goes on in the
 * next one, which is centred for itself. They close up when one goes. */
static void icons_arrange_centred(struct server *server)
{
	const struct config_icon_placement *p = config_icon_placement();
	int gap = p->tight ? 0 : 8;
	int size = p->horizontal ? ICON_W : ICON_H;
	int n = 0;
	struct view *v;
	wl_list_for_each(v, &server->views, link)
		n += v->minimized && !v->off_desktop;
	if (!n)
		return;
	struct view **list = calloc(n, sizeof(*list));
	struct wlr_box *areas = calloc(n, sizeof(*areas));
	bool *done = calloc(n, sizeof(*done));
	if (!list || !areas || !done)
		goto out;
	int i = 0;
	wl_list_for_each(v, &server->views, link) {
		if (v->minimized && !v->off_desktop)
			list[i++] = v;
	}
	qsort(list, n, sizeof(*list), by_icon_seq);
	for (i = 0; i < n; i++)
		areas[i] = output_usable_area(server, list[i]->x + 1, list[i]->y + 1);
	for (i = 0; i < n; i++) {
		if (done[i])
			continue;
		struct wlr_box a = areas[i];
		int count = 0;
		for (int j = i; j < n; j++)
			count += !done[j] && wlr_box_equal(&areas[j], &a);
		int per_line = icons_per_line(p, a, gap);
		int start = p->horizontal ? a.x : a.y, len = p->horizontal ? a.width : a.height;
		for (int j = i, m = 0; j < n; j++) {
			if (done[j] || !wlr_box_equal(&areas[j], &a))
				continue;
			done[j] = true;
			int line = m / per_line, k = m % per_line;
			int in_line = count - line * per_line;
			if (in_line > per_line)
				in_line = per_line;
			int total = in_line * size + (in_line - 1) * gap;
			int along = start + (len - total) / 2 + k * (size + gap);
			v = list[j];
			if (p->horizontal) {
				v->icon_x = along;
				v->icon_y = along_pos(a.y, a.height, ICON_H, gap, line, p->from_bottom);
			} else {
				v->icon_x = along_pos(a.x, a.width, ICON_W, gap, line, p->from_right);
				v->icon_y = along;
			}
			v->icon_placed = true;
			wlr_scene_node_set_position(&v->icon_node->node, v->icon_x, v->icon_y);
			m++;
		}
	}
out:
	free(list);
	free(areas);
	free(done);
}

/* A window keeps its slot when re-iconified. */
void icons_arrange(struct server *server)
{
	static long seq;
	struct view *v;
	/* The order they become icons in, for center; kept when restored. */
	wl_list_for_each_reverse(v, &server->views, link) {
		if (v->minimized && !v->icon_seq)
			v->icon_seq = ++seq;
	}
	if (iconbox_enabled()) {
		iconbox_arrange(server);
		return;
	}
	if (config_icon_placement()->center) {
		icons_arrange_centred(server);
		return;
	}
	wl_list_for_each_reverse(v, &server->views, link) {
		if (!v->minimized)
			continue;
		struct wlr_box area = output_usable_area(server,
			v->x + 1, v->y + 1);
		if (!v->icon_placed || icon_slot_taken(server, v,
				v->icon_x, v->icon_y)) {
			for (int i = 0;; i++) {
				int x, y;
				icon_slot(area, i, &x, &y);
				if (!icon_slot_taken(server, v, x, y)) {
					v->icon_x = x;
					v->icon_y = y;
					break;
				}
			}
			v->icon_placed = true;
		}
		wlr_scene_node_set_position(&v->icon_node->node,
			v->icon_x, v->icon_y);
	}
}
