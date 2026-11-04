/*
 * Windows-style snapping. Dragging a window with the pointer against an output's
 * edge snaps it to that half, against a corner to that quarter, against the top
 * edge to the whole usable area. A Motif-bevelled outline shows the target while
 * dragging. Super+arrow keys step between the same positions, as in Windows 11.
 */
#define _POSIX_C_SOURCE 200809L
#include <wlr/util/box.h>
#include "mwc.h"

#define SNAP_EDGE 2    /* pointer this close to an output's edge */
#define SNAP_CORNER 64 /* corner zone along each edge */
#define PREVIEW_W 8    /* outline thickness */

static struct wlr_scene_buffer *preview;
static struct wlr_box preview_box;
static bool preview_visible;

struct wlr_box snap_rect(enum snap s, struct wlr_box a)
{
	int hw = a.width / 2, hh = a.height / 2;
	switch (s) {
	case SNAP_LEFT:
		return (struct wlr_box){ a.x, a.y, hw, a.height };
	case SNAP_RIGHT:
		return (struct wlr_box){ a.x + hw, a.y, a.width - hw, a.height };
	case SNAP_TOP_LEFT:
		return (struct wlr_box){ a.x, a.y, hw, hh };
	case SNAP_TOP_RIGHT:
		return (struct wlr_box){ a.x + hw, a.y, a.width - hw, hh };
	case SNAP_BOTTOM_LEFT:
		return (struct wlr_box){ a.x, a.y + hh, hw, a.height - hh };
	case SNAP_BOTTOM_RIGHT:
		return (struct wlr_box){ a.x + hw, a.y + hh, a.width - hw,
			a.height - hh };
	default:
		return a;
	}
}

const char *snap_name(enum snap s)
{
	switch (s) {
	case SNAP_LEFT: return "Left half";
	case SNAP_RIGHT: return "Right half";
	case SNAP_TOP_LEFT: return "Top left";
	case SNAP_TOP_RIGHT: return "Top right";
	case SNAP_BOTTOM_LEFT: return "Bottom left";
	case SNAP_BOTTOM_RIGHT: return "Bottom right";
	case SNAP_MAXIMIZE: return "Maximize";
	default: return "";
	}
}

enum snap snap_zone_at(struct view *view, double px, double py)
{
	if (!(view->funcs & FUNC_RESIZE) || view->fullscreen)
		return SNAP_NONE;
	struct wlr_box m = output_full_area(view->server, px, py);
	bool left = px <= m.x + SNAP_EDGE;
	bool right = px >= m.x + m.width - 1 - SNAP_EDGE;
	bool top = py <= m.y + SNAP_EDGE;
	bool bottom = py >= m.y + m.height - 1 - SNAP_EDGE;
	bool near_top = py < m.y + SNAP_CORNER;
	bool near_bottom = py >= m.y + m.height - SNAP_CORNER;
	bool near_left = px < m.x + SNAP_CORNER;
	bool near_right = px >= m.x + m.width - SNAP_CORNER;

	if (left || right) {
		if (near_top)
			return left ? SNAP_TOP_LEFT : SNAP_TOP_RIGHT;
		if (near_bottom)
			return left ? SNAP_BOTTOM_LEFT : SNAP_BOTTOM_RIGHT;
		return left ? SNAP_LEFT : SNAP_RIGHT;
	}
	if (top) {
		if (near_left)
			return SNAP_TOP_LEFT;
		if (near_right)
			return SNAP_TOP_RIGHT;
		return (view->funcs & FUNC_MAXIMIZE) ? SNAP_MAXIMIZE : SNAP_NONE;
	}
	if (bottom) {
		if (near_left)
			return SNAP_BOTTOM_LEFT;
		if (near_right)
			return SNAP_BOTTOM_RIGHT;
	}
	return SNAP_NONE;
}

/* The outline of snap position s on the output at (px, py), hidden for SNAP_NONE:
 * raised outside, sunken towards the inside, in the dragged window's colour. The
 * inside of the buffer is transparent. */
void snap_preview(struct view *view, enum snap s, double px, double py)
{
	if (s == SNAP_NONE) {
		snap_preview_hide();
		return;
	}
	snap_preview_box(view, snap_rect(s, output_usable_area(view->server, px, py)));
}

void snap_preview_box(struct view *view, struct wlr_box r)
{
	struct server *server = view->server;
	const int t = PREVIEW_W;
	const struct mwc_shade *sh = &view->shade_active;

	struct cairo_buffer *buf = cairo_buffer_create(r.width, r.height);
	if (!buf)
		return;
	cairo_t *cr = buf->cr;
	set_source(cr, sh->bg);
	cairo_paint(cr);
	draw_bevel(cr, 0, 0, r.width, r.height, theme.bevel, sh, false);
	draw_bevel(cr, t - 2, t - 2, r.width - 2 * (t - 2), r.height - 2 * (t - 2),
		2, sh, true);
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_rectangle(cr, t, t, r.width - 2 * t, r.height - 2 * t);
	cairo_fill(cr);
	cairo_restore(cr);

	if (!preview)
		preview = wlr_scene_buffer_create(server->layer_feedback, NULL);
	/* Where the window goes stays visible over the target. */
	wlr_scene_node_lower_to_bottom(&preview->node);
	wlr_scene_buffer_set_buffer(preview, &buf->base);
	cairo_buffer_drop(buf);
	wlr_scene_node_set_position(&preview->node, r.x, r.y);
	wlr_scene_node_set_enabled(&preview->node, true);
	preview_box = r;
	preview_visible = true;
}

void snap_preview_hide(void)
{
	if (preview)
		wlr_scene_node_set_enabled(&preview->node, false);
	preview_visible = false;
}

bool snap_preview_get(struct wlr_box *box)
{
	*box = preview_box;
	return preview_visible;
}

/* Super+arrow keys, as in Windows 11: halves left and right, up to a top quarter
 * or maximized, down to a bottom quarter, back, or minimized. */
void snap_step(struct view *view, enum snap_dir dir)
{
	if (!view || view->minimized || view->fullscreen ||
			!(view->funcs & FUNC_RESIZE))
		return;
	enum snap cur = view->snap, next = cur;
	bool maxed = view->maximized;

	switch (dir) {
	case SNAP_DIR_LEFT:
		if (cur == SNAP_NONE)
			next = SNAP_LEFT;
		else if (cur == SNAP_RIGHT)
			next = SNAP_NONE;
		else if (cur == SNAP_TOP_RIGHT)
			next = SNAP_TOP_LEFT;
		else if (cur == SNAP_BOTTOM_RIGHT)
			next = SNAP_BOTTOM_LEFT;
		break;
	case SNAP_DIR_RIGHT:
		if (cur == SNAP_NONE)
			next = SNAP_RIGHT;
		else if (cur == SNAP_LEFT)
			next = SNAP_NONE;
		else if (cur == SNAP_TOP_LEFT)
			next = SNAP_TOP_RIGHT;
		else if (cur == SNAP_BOTTOM_LEFT)
			next = SNAP_BOTTOM_RIGHT;
		break;
	case SNAP_DIR_UP:
		if (maxed)
			return;
		if (cur == SNAP_NONE && (view->funcs & FUNC_MAXIMIZE))
			next = SNAP_MAXIMIZE;
		else if (cur == SNAP_LEFT)
			next = SNAP_TOP_LEFT;
		else if (cur == SNAP_RIGHT)
			next = SNAP_TOP_RIGHT;
		else if (cur == SNAP_BOTTOM_LEFT)
			next = SNAP_LEFT;
		else if (cur == SNAP_BOTTOM_RIGHT)
			next = SNAP_RIGHT;
		break;
	case SNAP_DIR_DOWN:
		if (maxed) {
			next = SNAP_NONE;
			break;
		}
		if (cur == SNAP_NONE) {
			if (view->funcs & FUNC_MINIMIZE)
				view_minimize(view);
			return;
		}
		if (cur == SNAP_LEFT)
			next = SNAP_BOTTOM_LEFT;
		else if (cur == SNAP_RIGHT)
			next = SNAP_BOTTOM_RIGHT;
		else if (cur == SNAP_TOP_LEFT)
			next = SNAP_LEFT;
		else if (cur == SNAP_TOP_RIGHT)
			next = SNAP_RIGHT;
		break;
	}
	if (next == cur && !maxed)
		return;
	view_snap(view, next, view->x + (view->width + frame_extra_w(view)) / 2.0,
		view->y + (view->height + frame_extra_h(view)) / 2.0);
}

/* Super+Shift+arrow: push a floating window flush against that edge of the usable
 * area of its output; two in a row reach a corner. */
void view_move_to_edge(struct view *view, enum snap_dir dir)
{
	if (!view || view->minimized || view->fullscreen || view->maximized ||
			view->snap != SNAP_NONE || !(view->funcs & FUNC_MOVE))
		return;
	int w = view->width + frame_extra_w(view);
	int h = view->height + frame_extra_h(view);
	struct wlr_box a = output_usable_area(view->server, view->x + w / 2.0,
		view->y + h / 2.0);
	int x = view->x, y = view->y;
	/* A window larger than the area keeps its left or top edge in. */
	switch (dir) {
	case SNAP_DIR_LEFT:
		x = a.x;
		break;
	case SNAP_DIR_RIGHT:
		x = a.x + a.width - w > a.x ? a.x + a.width - w : a.x;
		break;
	case SNAP_DIR_UP:
		y = a.y;
		break;
	case SNAP_DIR_DOWN:
		y = a.y + a.height - h > a.y ? a.y + a.height - h : a.y;
		break;
	}
	view_set_position(view, x, y);
}
