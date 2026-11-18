/*
 * sizehints.c - a window's size limits, steps, aspect ratios and gravity, from
 * WM_NORMAL_HINTS or xdg min/max size, and what they make of a size or a place. It
 * reads the client's state and calls nothing: the frame's size comes as a value.
 * Tested in tests/unit.
 */
#define _POSIX_C_SOURCE 200809L
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>
#include <xcb/xcb_icccm.h>
#include "mwc.h"

/* The hints may say anything, and size arithmetic on them must not overflow. */
static int hint_size(int32_t v)
{
	return v < 0 ? 0 : v > X11_MAX_SIZE ? X11_MAX_SIZE : v;
}

void read_xwayland_size_hints(struct view *view)
{
	struct wlr_xwayland_surface *xs = view->xsurface;
	const xcb_size_hints_t *sh = xs->size_hints;
	uint32_t flags = sh ? sh->flags : 0;
	bool has_min = flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE;
	bool has_base = flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE;

	/* ICCCM 4.1.2.3: base and min size stand in for each other. */
	view->base_w = view->base_h = view->min_w = view->min_h = 0;
	if (has_base) {
		view->base_w = hint_size(sh->base_width);
		view->base_h = hint_size(sh->base_height);
	} else if (has_min) {
		view->base_w = hint_size(sh->min_width);
		view->base_h = hint_size(sh->min_height);
	}
	if (has_min) {
		view->min_w = hint_size(sh->min_width);
		view->min_h = hint_size(sh->min_height);
	} else if (has_base) {
		view->min_w = hint_size(sh->base_width);
		view->min_h = hint_size(sh->base_height);
	}
	view->max_w = view->max_h = 0;
	if (flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
		view->max_w = hint_size(sh->max_width);
		view->max_h = hint_size(sh->max_height);
	}
	view->inc_w = view->inc_h = 0;
	if (flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) {
		view->inc_w = hint_size(sh->width_inc);
		view->inc_h = hint_size(sh->height_inc);
	}
	view->min_aspect = view->max_aspect = 0;
	if ((flags & XCB_ICCCM_SIZE_HINT_P_ASPECT) && sh->min_aspect_num > 0 &&
			sh->max_aspect_den > 0) {
		view->min_aspect = (float)sh->min_aspect_den / sh->min_aspect_num;
		view->max_aspect = (float)sh->max_aspect_num / sh->max_aspect_den;
	}
	view->gravity = (flags & XCB_ICCCM_SIZE_HINT_P_WIN_GRAVITY) ?
		sh->win_gravity : XCB_GRAVITY_NORTH_WEST;
}

void read_xdg_size_hints(struct view *view)
{
	const struct wlr_xdg_toplevel_state *st = &view->xdg_toplevel->current;
	/* wlroots only checks them against each other; 0 is no limit. */
	view->min_w = st->min_width < VIEW_MAX_SIZE ? st->min_width : VIEW_MAX_SIZE;
	view->min_h = st->min_height < VIEW_MAX_SIZE ? st->min_height : VIEW_MAX_SIZE;
	view->max_w = st->max_width < VIEW_MAX_SIZE ? st->max_width : VIEW_MAX_SIZE;
	view->max_h = st->max_height < VIEW_MAX_SIZE ? st->max_height : VIEW_MAX_SIZE;
	view->base_w = view->base_h = view->inc_w = view->inc_h = 0;
	view->min_aspect = view->max_aspect = 0;
	view->gravity = XCB_GRAVITY_NORTH_WEST;
}

bool view_fixed(struct view *view)
{
	return view->max_w && view->max_h && view->max_w == view->min_w &&
		view->max_h == view->min_h;
}

bool view_has_aspect(const struct view *view)
{
	return view->min_aspect > 0 && view->max_aspect > 0;
}

void view_aspect_follow(struct view *view, int *pw, int *ph, bool width_given)
{
	/* The same size counts as in view_apply_size_hints(). */
	bool base_is_min = view->base_w == view->min_w &&
		view->base_h == view->min_h;
	int base_w = base_is_min ? 0 : view->base_w;
	int base_h = base_is_min ? 0 : view->base_h;
	int w = *pw - base_w, h = *ph - base_h;
	if (w <= 0 || h <= 0)
		return;
	float wide = (float)w / h, tall = (float)h / w;
	/* In double and capped: a ratio of 1:10^9 makes 10^12. */
	double fw = w, fh = h;
	if (width_given) {
		if (view->max_aspect < wide)
			fh = w / (double)view->max_aspect;
		else if (view->min_aspect < tall)
			fh = w * (double)view->min_aspect;
	} else {
		if (view->min_aspect < tall)
			fw = h / (double)view->min_aspect;
		else if (view->max_aspect < wide)
			fw = h * (double)view->max_aspect;
	}
	*pw = (fw + 0.5 < X11_MAX_SIZE - base_w ? (int)(fw + 0.5) : X11_MAX_SIZE - base_w) +
		base_w;
	*ph = (fh + 0.5 < X11_MAX_SIZE - base_h ? (int)(fh + 0.5) : X11_MAX_SIZE - base_h) +
		base_h;
}

void view_apply_size_hints(struct view *view, int *pw, int *ph, bool incr)
{
	int w = *pw, h = *ph;
	bool base_is_min = view->base_w == view->min_w &&
		view->base_h == view->min_h;

	/* Aspect ratios apply to the size minus the base size, unless the base
	 * size merely stands in for the minimum (ICCCM 4.1.2.3). */
	if (!base_is_min) {
		w -= view->base_w;
		h -= view->base_h;
	}
	if (incr && view->min_aspect > 0 && view->max_aspect > 0 && w > 0 && h > 0) {
		if (view->max_aspect < (float)w / h)
			w = h * view->max_aspect + 0.5f;
		else if (view->min_aspect < (float)h / w)
			h = w * view->min_aspect + 0.5f;
	}
	if (base_is_min) {
		w -= view->base_w;
		h -= view->base_h;
	}
	if (incr && view->inc_w > 1)
		w -= w % view->inc_w;
	if (incr && view->inc_h > 1)
		h -= h % view->inc_h;
	w += view->base_w;
	h += view->base_h;
	if (w < view->min_w)
		w = view->min_w;
	if (h < view->min_h)
		h = view->min_h;
	if (view->max_w && w > view->max_w)
		w = view->max_w;
	if (view->max_h && h > view->max_h)
		h = view->max_h;
	/* What a configure can carry (uint16_t in wlroots), whatever asked for more. */
	if (view->type == VIEW_XWAYLAND) {
		w = w < X11_MAX_SIZE ? w : X11_MAX_SIZE;
		h = h < X11_MAX_SIZE ? h : X11_MAX_SIZE;
	}
	*pw = w > 1 ? w : 1;
	*ph = h > 1 ? h : 1;
}

/* ICCCM 4.1.2.3: the client's position, turned into the frame's. The reference
 * point of the frame goes where the client asked for its own to be. */
void view_gravity_to_frame(struct view *view, struct frame_size f, int x, int y, int w,
	int h, int *fx, int *fy)
{
	int fw = w + f.extra_w;
	int fh = h + f.extra_h;

	if (view->gravity == XCB_GRAVITY_STATIC) {
		*fx = x - f.left;
		*fy = y - f.top;
		return;
	}
	switch (view->gravity) {
	case XCB_GRAVITY_NORTH:
	case XCB_GRAVITY_CENTER:
	case XCB_GRAVITY_SOUTH:
		*fx = x + w / 2 - fw / 2;
		break;
	case XCB_GRAVITY_NORTH_EAST:
	case XCB_GRAVITY_EAST:
	case XCB_GRAVITY_SOUTH_EAST:
		*fx = x + w - fw;
		break;
	default:
		*fx = x;
		break;
	}
	switch (view->gravity) {
	case XCB_GRAVITY_WEST:
	case XCB_GRAVITY_CENTER:
	case XCB_GRAVITY_EAST:
		*fy = y + h / 2 - fh / 2;
		break;
	case XCB_GRAVITY_SOUTH_WEST:
	case XCB_GRAVITY_SOUTH:
	case XCB_GRAVITY_SOUTH_EAST:
		*fy = y + h - fh;
		break;
	default:
		*fy = y;
		break;
	}
}
