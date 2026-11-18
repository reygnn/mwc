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

/* Straight from the parent's surface, without going through all views: it is asked
 * for each view on every focus change, and a client decides how many there are. An
 * xdg surface's data is its scene tree (xdg_surface_set_tree()), which hangs in its
 * view's tree; an X11 surface's data is its view (new_managed()). */
static struct view *direct_parent(struct view *view)
{
	struct view *p = NULL;
	if (view->type == VIEW_XDG) {
		struct wlr_xdg_toplevel *t = view->xdg_toplevel->parent;
		struct wlr_scene_tree *tree = t ? t->base->data : NULL;
		if (tree && tree->node.parent)
			p = tree->node.parent->node.data;
	} else if (view->xsurface->parent) {
		p = view->xsurface->parent->data;
	}
	return p != view ? p : NULL;
}

/* An unmapped parent's place goes to its own parent, as xdg-shell says: a window
 * withdrawn to the tray, or an owner never shown, keeps no desktop or state that
 * its dialogs would follow out of sight. */
struct view *view_parent(struct view *view)
{
	struct view *p = direct_parent(view);
	for (int i = 0; p && !p->mapped && i < MAX_TRANSIENT_DEPTH; i++)
		p = direct_parent(p);
	return p && p->mapped && p != view ? p : NULL;
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
	if (!view->mapped || view_hidden(view) ||
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

/* With the ALL bit set, the remaining bits name what to take away. */
static unsigned motif_mask(uint32_t hint, unsigned all)
{
	return (hint & 1) ? all & ~hint : hint & all;
}

void view_update_decorations(struct view *view)
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
	if (view_fixed(view))
		f &= ~(FUNC_RESIZE | FUNC_MAXIMIZE);
	if (view_docked(view))
		f &= ~(FUNC_MOVE | FUNC_RESIZE | FUNC_MINIMIZE | FUNC_MAXIMIZE);

	/* Decorations follow the functions they trigger. */
	if (!(f & FUNC_RESIZE) && (d & DECOR_RESIZEH))
		d = (d & ~DECOR_RESIZEH) | DECOR_BORDER;
	if (!(f & FUNC_MINIMIZE))
		d &= ~DECOR_MINIMIZE;
	if (!(f & FUNC_MAXIMIZE))
		d &= ~DECOR_MAXIMIZE;

	/* Tiled: the full decoration stays, only moving and resizing do nothing;
	 * the layout places the window. */
	view->base_funcs = f;
	if (view->tiled)
		f &= ~(FUNC_MOVE | FUNC_RESIZE);

	bool changed = d != view->decor;
	view->decor = d;
	view->funcs = f;
	if (changed && view->mapped)
		view_frame_changed(view);
}

void view_update_properties(struct view *view)
{
	enum window_type old_type = view->wtype;
	int old_min_w = view->min_w, old_min_h = view->min_h;
	int old_max_w = view->max_w, old_max_h = view->max_h;
	bool old_fixed = view_fixed(view);
	if (view->type == VIEW_XDG) {
		read_xdg_size_hints(view);
		view->wtype = view->xdg_toplevel->parent ? WT_DIALOG : WT_NORMAL;
	} else {
		read_xwayland_size_hints(view);
		view->wtype = xwayland_type(view->xsurface);
	}
	view_update_decorations(view);
	xdg_publish_caps(view);
	if (view->wtype != old_type)
		views_update_layers(view->server);
	/* Whether it fits the layout, or takes part at all, may have changed. */
	if (view->mapped && view->server->tiling && (view->wtype != old_type ||
			view_fixed(view) != old_fixed ||
			view->min_w != old_min_w || view->min_h != old_min_h ||
			view->max_w != old_max_w || view->max_h != old_max_h))
		tile_arrange(view->server);
	tabs_view_changed(view);
}
