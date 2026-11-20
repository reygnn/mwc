/*
 * What a window may do and how it is decorated, from what the client says about
 * itself: its min/max size and the window it belongs to. The rules follow mwm-x11,
 * and so Motif's defaults.
 */
#define _POSIX_C_SOURCE 200809L
#include <wlr/types/wlr_xdg_shell.h>
#include "mwc.h"

/* Guards against parent cycles. */
#define MAX_TRANSIENT_DEPTH 16

/* Straight from the parent's surface, without going through all views: it is asked
 * for each view on every focus change, and a client decides how many there are. An
 * xdg surface's data is its scene tree (xdg_surface_set_tree()), which hangs in its
 * view's tree. */
static struct view *direct_parent(struct view *view)
{
	struct view *p = NULL;
	struct wlr_xdg_toplevel *t = view->xdg_toplevel->parent;
	struct wlr_scene_tree *tree = t ? t->base->data : NULL;
	if (tree && tree->node.parent)
		p = tree->node.parent->node.data;
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
	return view->mapped && !view_hidden(view);
}

void view_update_decorations(struct view *view)
{
	unsigned d = DECOR_ALL, f = FUNC_ALL;
	if (view->wtype == WT_DIALOG) {
		/* mwm's transientDecoration/transientFunctions defaults */
		d &= ~(DECOR_MINIMIZE | DECOR_MAXIMIZE);
		f &= ~(FUNC_MINIMIZE | FUNC_MAXIMIZE);
	}
	if (view_fixed(view))
		f &= ~(FUNC_RESIZE | FUNC_MAXIMIZE);
	if (view_docked(view))
		f &= ~(FUNC_MOVE | FUNC_RESIZE | FUNC_MINIMIZE | FUNC_MAXIMIZE);

	/* Decorations follow the functions they trigger. */
	if (!(f & FUNC_RESIZE))
		d &= ~DECOR_RESIZEH;
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
	read_xdg_size_hints(view);
	view->wtype = view->xdg_toplevel->parent ? WT_DIALOG : WT_NORMAL;
	view_update_decorations(view);
	xdg_publish_caps(view);
	/* Whether it fits the layout, or takes part at all, may have changed. */
	if (view->mapped && view->server->tiling && (view->wtype != old_type ||
			view_fixed(view) != old_fixed ||
			view->min_w != old_min_w || view->min_h != old_min_h ||
			view->max_w != old_max_w || view->max_h != old_max_h))
		tile_arrange(view->server);
	tabs_view_changed(view);
}
