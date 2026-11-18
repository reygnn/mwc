/*
 * snaprules.c - where snapping puts a window, worked out from boxes and flags: the
 * positions and their gaps, the zones at an output's edges, the column beside a
 * dock, Super+arrow's next step and Super+Shift+arrow's edge. It reads outputs and
 * calls nothing; snap.c asks it and acts. Tested in tests/unit.
 */
#define _POSIX_C_SOURCE 200809L
#include <wlr/util/box.h>
#include "mwc.h"

#define SNAP_EDGE 2    /* pointer this close to an output's edge */
#define SNAP_CORNER 64 /* corner zone along each edge */

struct wlr_box snap_rect(enum snap s, struct wlr_box a)
{
	if (s == SNAP_NONE || s == SNAP_MAXIMIZE)
		return a;
	/* The gap around, then the two halves with the gap between them. */
	const int g = MWC_GAP;
	struct wlr_box in = { a.x + g, a.y + g, a.width - 2 * g, a.height - 2 * g };
	int lw = (in.width - g) / 2, rw = in.width - g - lw;
	int th = (in.height - g) / 2, bh = in.height - g - th;
	int rx = in.x + lw + g, by = in.y + th + g;
	switch (s) {
	case SNAP_LEFT:
		return (struct wlr_box){ in.x, in.y, lw, in.height };
	case SNAP_RIGHT:
		return (struct wlr_box){ rx, in.y, rw, in.height };
	case SNAP_TOP_LEFT:
		return (struct wlr_box){ in.x, in.y, lw, th };
	case SNAP_TOP_RIGHT:
		return (struct wlr_box){ rx, in.y, rw, th };
	case SNAP_BOTTOM_LEFT:
		return (struct wlr_box){ in.x, by, lw, bh };
	case SNAP_BOTTOM_RIGHT:
		return (struct wlr_box){ rx, by, rw, bh };
	default:
		return a;
	}
}

static bool dock_in_the_way(const struct output *o, struct wlr_box b)
{
	struct wlr_box hit;
	return wlr_box_intersection(&hit, &b, &o->dock);
}

/* Beside a dock along the bottom or top a column of positions may have its strip.
 * The column decides as one, so that a half and the quarters in it line up. A dock
 * along a side would narrow some positions and not others, which then overlap. */
struct wlr_box snap_area_on(const struct output *o, bool horizontal_dock, enum snap s)
{
	bool left = s == SNAP_LEFT || s == SNAP_TOP_LEFT || s == SNAP_BOTTOM_LEFT;
	bool right = s == SNAP_RIGHT || s == SNAP_TOP_RIGHT || s == SNAP_BOTTOM_RIGHT;
	if (!horizontal_dock || !(left || right))
		return o->usable;
	struct wlr_box column = snap_rect(left ? SNAP_LEFT : SNAP_RIGHT, o->open);
	return dock_in_the_way(o, column) ? o->usable : o->open;
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

enum snap snap_zone(struct wlr_box m, int funcs, bool fullscreen, double px, double py)
{
	if (!(funcs & FUNC_RESIZE) || fullscreen)
		return SNAP_NONE;
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
		return (funcs & FUNC_MAXIMIZE) ? SNAP_MAXIMIZE : SNAP_NONE;
	}
	if (bottom) {
		if (near_left)
			return SNAP_BOTTOM_LEFT;
		if (near_right)
			return SNAP_BOTTOM_RIGHT;
	}
	return SNAP_NONE;
}

/* Super+arrow keys, as in Windows 11: halves left and right, up to a top quarter
 * or maximized, down to a bottom quarter, back, or minimized. */
struct snap_move snap_next(enum snap cur, bool maxed, int funcs, enum snap_dir dir)
{
	enum snap next = cur;
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
			return (struct snap_move){ 0 };
		if (cur == SNAP_NONE && (funcs & FUNC_MAXIMIZE))
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
		if (cur == SNAP_NONE)
			return (struct snap_move){ .minimize = (funcs & FUNC_MINIMIZE) != 0 };
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
		return (struct snap_move){ 0 };
	return (struct snap_move){ .snap = true, .to = next };
}

/* A window larger than the area keeps its left or top edge in. */
static struct wlr_box edge_box(struct wlr_box a, enum snap_dir dir, struct wlr_box f)
{
	switch (dir) {
	case SNAP_DIR_LEFT:
		f.x = a.x;
		break;
	case SNAP_DIR_RIGHT:
		f.x = a.x + a.width - f.width > a.x ? a.x + a.width - f.width : a.x;
		break;
	case SNAP_DIR_UP:
		f.y = a.y;
		break;
	case SNAP_DIR_DOWN:
		f.y = a.y + a.height - f.height > a.y ? a.y + a.height - f.height : a.y;
		break;
	}
	return f;
}

/* Beside the dock as far as the output's edge, else to the usable area's; one that
 * would now cover the dock moves out of its strip as well. */
struct wlr_box snap_edge_place(const struct output *o, struct wlr_box usable,
	enum snap_dir dir, struct wlr_box f)
{
	if (o) {
		struct wlr_box b = edge_box(o->open, dir, f);
		if (!dock_in_the_way(o, b))
			return b;
	}
	struct wlr_box b = edge_box(usable, dir, f);
	if (b.x + f.width > usable.x + usable.width)
		b.x = usable.x + usable.width - f.width > usable.x ?
			usable.x + usable.width - f.width : usable.x;
	if (b.y + f.height > usable.y + usable.height)
		b.y = usable.y + usable.height - f.height > usable.y ?
			usable.y + usable.height - f.height : usable.y;
	return b;
}
