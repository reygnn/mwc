/*
 * What a window may do and how it is decorated, from what the client says about
 * itself: its type, WM_NORMAL_HINTS or xdg min/max size, _MOTIF_WM_HINTS and the
 * window it belongs to. The rules follow mwm-x11, and so Motif's defaults.
 */
#define _POSIX_C_SOURCE 200809L
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>
#include <xcb/xcb_icccm.h>
#include "mwc.h"

/* _MOTIF_WM_HINTS flags */
#define MWM_HINTS_FUNCTIONS 1
#define MWM_HINTS_DECORATIONS 2

/* Guards against WM_TRANSIENT_FOR cycles, which a client can set up. */
#define MAX_TRANSIENT_DEPTH 16

struct view *view_parent(struct view *view)
{
	struct view *v;
	wl_list_for_each(v, &view->server->views, link) {
		if (v == view || v->type != view->type)
			continue;
		if (view->type == VIEW_XDG ?
				v->xdg_toplevel == view->xdg_toplevel->parent :
				v->xsurface == view->xsurface->parent)
			return v;
	}
	return NULL;
}

bool view_is_transient_of(struct view *view, struct view *ancestor)
{
	struct view *p = view;
	for (int i = 0; i < MAX_TRANSIENT_DEPTH; i++) {
		p = view_parent(p);
		if (!p)
			return false;
		if (p == ancestor)
			return true;
	}
	return false;
}

struct view *view_transient_root(struct view *view)
{
	struct view *r = view;
	for (int i = 0; i < MAX_TRANSIENT_DEPTH; i++) {
		struct view *p = view_parent(r);
		if (!p || p == view)
			break;
		r = p;
	}
	return r;
}

bool view_focusable(struct view *view)
{
	if (!view->mapped || view->minimized || view->hidden_with_parent ||
			view->wtype == WT_DOCK || view->wtype == WT_DESKTOP ||
			view->wtype == WT_NOTIFICATION)
		return false;
	return view->type != VIEW_XWAYLAND ||
		wlr_xwayland_surface_icccm_input_model(view->xsurface) !=
		WLR_ICCCM_INPUT_MODEL_NONE;
}

static enum window_type xwayland_type(struct wlr_xwayland_surface *xs)
{
	static const struct {
		enum wlr_xwayland_net_wm_window_type net;
		enum window_type type;
	} table[] = {
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DESKTOP, WT_DESKTOP },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DOCK, WT_DOCK },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH, WT_SPLASH },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION, WT_NOTIFICATION },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP, WT_NOTIFICATION },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU, WT_NOTIFICATION },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU, WT_NOTIFICATION },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO, WT_NOTIFICATION },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND, WT_NOTIFICATION },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG, WT_DIALOG },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY, WT_UTILITY },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR, WT_UTILITY },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU, WT_UTILITY },
		{ WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NORMAL, WT_NORMAL },
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(*table); i++) {
		if (wlr_xwayland_surface_has_window_type(xs, table[i].net))
			return table[i].type;
	}
	return xs->parent ? WT_DIALOG : WT_NORMAL;
}

static void read_xwayland_size_hints(struct view *view)
{
	struct wlr_xwayland_surface *xs = view->xsurface;
	const xcb_size_hints_t *sh = xs->size_hints;
	uint32_t flags = sh ? sh->flags : 0;
	bool has_min = flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE;
	bool has_base = flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE;

	/* ICCCM 4.1.2.3: base and min size stand in for each other. */
	view->base_w = view->base_h = view->min_w = view->min_h = 0;
	if (has_base) {
		view->base_w = sh->base_width;
		view->base_h = sh->base_height;
	} else if (has_min) {
		view->base_w = sh->min_width;
		view->base_h = sh->min_height;
	}
	if (has_min) {
		view->min_w = sh->min_width;
		view->min_h = sh->min_height;
	} else if (has_base) {
		view->min_w = sh->base_width;
		view->min_h = sh->base_height;
	}
	view->max_w = view->max_h = 0;
	if (flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
		view->max_w = sh->max_width > 0 ? sh->max_width : 0;
		view->max_h = sh->max_height > 0 ? sh->max_height : 0;
	}
	view->inc_w = view->inc_h = 0;
	if (flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) {
		view->inc_w = sh->width_inc > 0 ? sh->width_inc : 0;
		view->inc_h = sh->height_inc > 0 ? sh->height_inc : 0;
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

static void read_xdg_size_hints(struct view *view)
{
	const struct wlr_xdg_toplevel_state *st = &view->xdg_toplevel->current;
	view->min_w = st->min_width;
	view->min_h = st->min_height;
	view->max_w = st->max_width;
	view->max_h = st->max_height;
	view->base_w = view->base_h = view->inc_w = view->inc_h = 0;
	view->min_aspect = view->max_aspect = 0;
	view->gravity = XCB_GRAVITY_NORTH_WEST;
}

/* With the ALL bit set, the remaining bits name what to take away. */
static unsigned motif_mask(uint32_t hint, unsigned all)
{
	return (hint & 1) ? all & ~hint : hint & all;
}

static void update_decorations(struct view *view)
{
	unsigned d = DECOR_ALL, f = FUNC_ALL;
	switch (view->wtype) {
	case WT_DESKTOP:
	case WT_DOCK:
	case WT_SPLASH:
		d = 0;
		f = 0;
		break;
	case WT_NOTIFICATION:
		d = 0;
		f = FUNC_CLOSE;
		break;
	case WT_DIALOG:
	case WT_UTILITY:
		/* mwm's transientDecoration/transientFunctions defaults */
		d &= ~(DECOR_MINIMIZE | DECOR_MAXIMIZE);
		f &= ~(FUNC_MINIMIZE | FUNC_MAXIMIZE);
		break;
	case WT_NORMAL:
		break;
	}
	if (view->motif_flags & MWM_HINTS_FUNCTIONS)
		f &= motif_mask(view->motif_funcs, FUNC_ALL);
	if (view->motif_flags & MWM_HINTS_DECORATIONS)
		d &= motif_mask(view->motif_decor, DECOR_ALL);
	if (view->max_w && view->max_h && view->max_w == view->min_w &&
			view->max_h == view->min_h)
		f &= ~(FUNC_RESIZE | FUNC_MAXIMIZE);

	/* Decorations follow the functions they trigger. */
	if (!(f & FUNC_RESIZE) && (d & DECOR_RESIZEH))
		d = (d & ~DECOR_RESIZEH) | DECOR_BORDER;
	if (!(f & FUNC_MINIMIZE))
		d &= ~DECOR_MINIMIZE;
	if (!(f & FUNC_MAXIMIZE))
		d &= ~DECOR_MAXIMIZE;

	bool changed = d != view->decor;
	view->decor = d;
	view->funcs = f;
	if (changed && view->mapped)
		view_frame_changed(view);
}

void view_update_properties(struct view *view)
{
	enum window_type old_type = view->wtype;
	if (view->type == VIEW_XDG) {
		read_xdg_size_hints(view);
		view->wtype = view->xdg_toplevel->parent ? WT_DIALOG : WT_NORMAL;
	} else {
		read_xwayland_size_hints(view);
		view->wtype = xwayland_type(view->xsurface);
	}
	update_decorations(view);
	if (view->wtype != old_type)
		views_update_layers(view->server);
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
	*pw = w > 1 ? w : 1;
	*ph = h > 1 ? h : 1;
}

/* ICCCM 4.1.2.3: the client's position, turned into the frame's. The reference
 * point of the frame goes where the client asked for its own to be. */
void view_gravity_to_frame(struct view *view, int x, int y, int *fx, int *fy)
{
	int fw = view->width + frame_extra_w(view);
	int fh = view->height + frame_extra_h(view);
	int w = view->width, h = view->height;

	if (view->gravity == XCB_GRAVITY_STATIC) {
		*fx = x - frame_left(view);
		*fy = y - frame_top(view);
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
