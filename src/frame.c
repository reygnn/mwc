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

/* How far the resize corners reach along each side of a W x H frame: B+T, at
 * most half the frame. Hit testing and the grooves agree. */
static void corner_reach(int W, int H, int *kx, int *ky)
{
	int k = theme.border + theme.title_height;
	*kx = k < W / 2 ? k : W / 2;
	*ky = k < H / 2 ? k : H / 2;
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

void frame_render(struct view *view)
{
	const int B = frame_border(view), T = frame_title(view);
	if (!B && !T) {
		/* The client covers the whole window. */
		wlr_scene_node_set_enabled(&view->frame_node->node, false);
		return;
	}

	const int bv = theme.bevel < B ? theme.bevel : B;
	const bool full = B == theme.border;
	const int W = view->width + 2 * B;
	const int H = frame_height(view);
	const struct mwc_shade *s = view->activated ?
		&view->shade_active : &view->shade_inactive;

	struct cairo_buffer *buf = cairo_buffer_create(W, H);
	if (!buf)
		return;
	cairo_t *cr = buf->cr;

	set_source(cr, s->bg);
	cairo_paint(cr);

	if (B) {
		/* Outer raised edge, inner sunken edge around title + client. */
		draw_bevel(cr, 0, 0, W, H, bv, s, false);
		if (full)
			draw_bevel(cr, B - 1, B - 1, W - 2 * B + 2,
				H - 2 * B + 2, 1, s, true);
	}

	/* Resize corner grooves, cutting the border at title-bar distance. */
	/* Tiled windows keep them: the decoration stays, inactive. */
	if (full && ((view->funcs & FUNC_RESIZE) || view->tiled) &&
			(!view->maximized || view->tiled)) {
		int kx, ky;
		corner_reach(W, H, &kx, &ky);
		draw_groove_v(cr, kx, bv, B - bv - 1, s);
		draw_groove_v(cr, W - kx - 2, bv, B - bv - 1, s);
		draw_groove_v(cr, kx, H - B + 1, B - bv - 1, s);
		draw_groove_v(cr, W - kx - 2, H - B + 1, B - bv - 1, s);
		draw_groove_h(cr, bv, ky, B - bv - 1, s);
		draw_groove_h(cr, W - B + 1, ky, B - bv - 1, s);
		draw_groove_h(cr, bv, H - ky - 2, B - bv - 1, s);
		draw_groove_h(cr, W - B + 1, H - ky - 2, B - bv - 1, s);
	}

	/* Title bar: menu button | title | minimize | maximize */
	if (T) {
		struct title_layout tl;
		title_layout(view, &tl);
		if (tl.menu_x >= 0)
			draw_button(cr, view, PART_MENU_BUTTON, tl.menu_x, B, T, s);
		draw_bevel(cr, tl.title_x, B, tl.title_w, T, 1, s,
			view->pressed == PART_TITLE);
		draw_text(cr, view_title(view), theme.font, s->fg,
			tl.title_x + 4, B + 1, tl.title_w - 8, T - 2, true);
		if (tl.min_x >= 0)
			draw_button(cr, view, PART_MINIMIZE_BUTTON, tl.min_x, B, T, s);
		if (tl.max_x >= 0)
			draw_button(cr, view, PART_MAXIMIZE_BUTTON, tl.max_x, B, T, s);
	}

	wlr_scene_buffer_set_buffer(view->frame_node, &buf->base);
	cairo_buffer_drop(buf);
	wlr_scene_node_set_enabled(&view->frame_node->node, true);
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
		return PART_TITLE;
	}
	/* A border that can't resize moves the window, like the title. */
	if (B != theme.border || !(view->funcs & FUNC_RESIZE) || view->maximized)
		return PART_TITLE;

	/* Border: the corner pieces extend B+T along each side, but no further
	 * than half the frame, or a short window would be at its top and bottom
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
	draw_bevel(cr, 0, img_h, ICON_W, LABEL_H, 2, s, false);

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
		if (v != self && v->minimized && v->icon_placed &&
				v->icon_x == x && v->icon_y == y)
			return true;
	}
	return false;
}

/* Icons go along the bottom-left of the output, like mwm's default
 * iconPlacement "left bottom". A window keeps its slot when re-iconified. */
void icons_arrange(struct server *server)
{
	struct view *v;
	wl_list_for_each_reverse(v, &server->views, link) {
		if (!v->minimized)
			continue;
		struct wlr_box area = output_usable_area(server,
			v->x + 1, v->y + 1);
		int row_y = area.y + area.height - ICON_H - 8;
		if (!v->icon_placed || icon_slot_taken(server, v,
				v->icon_x, v->icon_y)) {
			for (int i = 0;; i++) {
				int per_row = (area.width - 8) / (ICON_W + 8);
				if (per_row < 1)
					per_row = 1;
				int x = area.x + 8 + (i % per_row) * (ICON_W + 8);
				int y = row_y - (i / per_row) * (ICON_H + 8);
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
