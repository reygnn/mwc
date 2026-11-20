/*
 * sizehints.c - a window's size limits, from xdg min/max size, and what they make of
 * a size. It reads the client's state and calls nothing. Tested in tests/unit.
 */
#define _POSIX_C_SOURCE 200809L
#include <wlr/types/wlr_xdg_shell.h>
#include "mwc.h"

void read_xdg_size_hints(struct view *view)
{
	const struct wlr_xdg_toplevel_state *st = &view->xdg_toplevel->current;
	/* wlroots only checks them against each other; 0 is no limit. */
	view->min_w = st->min_width < VIEW_MAX_SIZE ? st->min_width : VIEW_MAX_SIZE;
	view->min_h = st->min_height < VIEW_MAX_SIZE ? st->min_height : VIEW_MAX_SIZE;
	view->max_w = st->max_width < VIEW_MAX_SIZE ? st->max_width : VIEW_MAX_SIZE;
	view->max_h = st->max_height < VIEW_MAX_SIZE ? st->max_height : VIEW_MAX_SIZE;
}

bool view_fixed(struct view *view)
{
	return view->max_w && view->max_h && view->max_w == view->min_w &&
		view->max_h == view->min_h;
}

void view_apply_size_hints(struct view *view, int *pw, int *ph)
{
	int w = *pw, h = *ph;
	if (w < view->min_w)
		w = view->min_w;
	if (h < view->min_h)
		h = view->min_h;
	if (view->max_w && w > view->max_w)
		w = view->max_w;
	if (view->max_h && h > view->max_h)
		h = view->max_h;
	*pw = w > 1 ? w : 1;
	*ph = h > 1 ? h : 1;
}
