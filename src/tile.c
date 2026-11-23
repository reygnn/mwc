/*
 * Tiling mode: the main window takes the right part of the usable area, the other
 * windows share the left part, one above the other. Gaps around and between the
 * windows keep the desktop (and its root menu) within reach.
 *
 * There is no dragging: tiled windows keep their full decoration, but moving and
 * resizing do nothing. Minimize on the main window moves it down into the stack, on
 * a stack window it shades it (rolls it up to its title bar); maximize on a stack
 * window makes it the main window. A new window, or one back from an icon, becomes
 * the main window.
 *
 * Only normal top-level windows take part. Dialogs, windows of a fixed size,
 * fullscreen and minimized windows float as before, and so do programs left out by
 * Mwm*DoNotTile or Mwm*TileOnly. The layout covers the first output. "Raise/Lower
 * Tiled Windows" in the root menu puts the layout above or below the floating
 * windows.
 *
 * The main window's share of the width is Mwm*tileMainWidth (or
 * MWC_TILE_MAIN_WIDTH), 55 % by default. Super+T or "Tile Windows" in the root menu
 * switches it; Mwm*tiling: true in mwmrc (or MWC_TILING=1) turns it on from the
 * start, and reloading the configuration keeps the current choice.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>
#include "mwc.h"

#define TILE_MAIN_PCT 55

static int main_pct = TILE_MAIN_PCT;

/* The environment over mwmrc, as for the other settings. */
static void read_main_width(void)
{
	int v;
	main_pct = TILE_MAIN_PCT;
	if ((v = tile_parse_pct("MWC_TILE_MAIN_WIDTH", getenv("MWC_TILE_MAIN_WIDTH"))) > 0 ||
			(v = tile_parse_pct("tileMainWidth", config_get("tileMainWidth"))) > 0)
		main_pct = v;
}

void tile_init(struct server *server)
{
	int v = config_bool("MWC_TILING", getenv("MWC_TILING"));
	if (v >= 0)
		server->tiling = v;
	else
		server->tiling = config_bool("tiling", config_get("tiling")) == 1;
	read_main_width();
}

void tile_config_changed(struct server *server)
{
	read_main_width();
	struct view *v;
	wl_list_for_each(v, &server->views, link)
		config_view_tiling(v);
	tile_arrange(server);
}

void tile_toggle(struct server *server)
{
	server->tiling = !server->tiling;
	tile_arrange(server);
}

bool tile_can_shade(struct view *view)
{
	return frame_title(view) > 0;
}

/* Normal top-level windows that may be resized; nofit ones (their minimum size
 * doesn't fit their cell) float for this round. */
static bool tileable(struct view *view)
{
	return view->mapped && view->wtype == WT_NORMAL && !view_hidden(view) &&
		!view->fullscreen && !view_fixed(view) &&
		(view->base_funcs & FUNC_RESIZE) && !view_parent(view) &&
		!view->tile_nofit && !view->no_tile;
}

/* Layout order; ties can't happen, every window gets its own number. */
static int by_seq(const void *a, const void *b)
{
	const struct view *x = *(struct view *const *)a;
	const struct view *y = *(struct view *const *)b;
	return (x->tile_seq > y->tile_seq) - (x->tile_seq < y->tile_seq);
}

/* The tiled windows of the current desktop in layout order, main window first. The
 * caller frees it. */
static struct view **tiled_windows(struct server *server, int *n)
{
	struct view **list = calloc(wl_list_length(&server->views) + 1, sizeof(*list));
	struct view *v;
	*n = 0;
	if (!list)
		return NULL;
	wl_list_for_each(v, &server->views, link) {
		if (v->tiled && !v->off_desktop)
			list[(*n)++] = v;
	}
	qsort(list, *n, sizeof(*list), by_seq);
	return list;
}

/* The output the layout covers: the first one that was added and is in the layout.
 * tile_arrange() doesn't start without one. Icons stay in view: the layout leaves out
 * the strip along the edge they pile up from, up to the innermost row or column of
 * them (they keep their own 8 px margin), or the icon box. */
struct wlr_box tile_layout_area(struct server *server)
{
	struct wlr_box a = output_first(server)->usable;
	const struct config_placement *p = config_icon_placement();
	struct wlr_box *icons = calloc(wl_list_length(&server->views) + 1, sizeof(*icons));
	int n = 0;
	if (!icons)
		return tile_area(a, p, NULL, 0);
	if (iconbox_enabled()) {
		if (iconbox_rect(&icons[0]))
			n = 1;
	} else {
		struct view *v;
		wl_list_for_each(v, &server->views, link) {
			if (!v->minimized || v->off_desktop || !v->icon_placed ||
					v->icon_x < a.x || v->icon_x >= a.x + a.width ||
					v->icon_y < a.y || v->icon_y >= a.y + a.height)
				continue;
			icons[n++] = (struct wlr_box){ v->icon_x, v->icon_y, ICON_W, ICON_H };
		}
	}
	a = tile_area(a, p, icons, n);
	free(icons);
	return a;
}

/* Cells for list[0..n), main window first, through tile_plan(). Returns a window
 * that can't fit its cell, or NULL; *overflow says the stack column is full even
 * with every window in it shaded. */
static struct view *plan(struct server *server, struct view **list, int n,
	bool *overflow)
{
	*overflow = false;
	struct tile_window *w = calloc(n, sizeof(*w));
	if (!w)
		return NULL;
	for (int i = 0; i < n; i++) {
		struct view *v = list[i];
		int cw = 1, ch = 1;
		view_apply_size_hints(v, &cw, &ch);
		w[i] = (struct tile_window){
			.min_w = cw + frame_extra_w(v), .min_h = ch + frame_extra_h(v),
			.shaded_h = frame_extra_h(v), .can_shade = tile_can_shade(v),
			.focused = v->activated, .shaded = v->shaded,
			.auto_shaded = v->auto_shaded, .cell = v->tile, .main = v->tile_main,
		};
	}
	int misfit = tile_plan(w, n, tile_layout_area(server), main_pct, overflow);
	for (int i = 0; i < n; i++) {
		list[i]->shaded = w[i].shaded;
		list[i]->auto_shaded = w[i].auto_shaded;
		list[i]->tile = w[i].cell;
		list[i]->tile_main = w[i].main;
	}
	free(w);
	return misfit >= 0 ? list[misfit] : NULL;
}

/* Would the layout take it, as things are? The same plan as a round, with the window
 * counted in, but without tiling it. */
static bool fits(struct server *server, struct view *view)
{
	view->tiled = true;
	int n;
	struct view **list = tiled_windows(server, &n);
	bool overflow = false;
	struct view *misfit = n ? plan(server, list, n, &overflow) : view;
	free(list);
	view->tiled = false;
	return misfit != view;
}

void tile_arrange(struct server *server)
{
	/* Leaving or entering the layout reconfigures windows, which must not start
	 * another round from inside this one. */
	static bool busy;
	/* Without an output in the layout there is nowhere to put them: an empty area
	 * would make every window a misfit, and minimize them one after another. */
	if (busy || !output_first(server))
		return;
	busy = true;

	/* One being moved or resized: taking it into the layout ends that. */
	struct view *held = server->grab.mode == CURSOR_MOVE ||
		server->grab.mode == CURSOR_RESIZE ? server->grab.view : NULL;
	struct view *v;
	wl_list_for_each(v, &server->views, link)
		v->tile_nofit = false;
	for (;;) {
		wl_list_for_each(v, &server->views, link) {
			/* Another desktop's windows keep their place in its layout. */
			if (v->off_desktop)
				continue;
			bool want = server->tiling && tileable(v);
			if (want == v->tiled)
				continue;
			/* Only if it fits now: trying it and letting it float again would end
			 * the grab for nothing. */
			if (want && v == held && !fits(server, v)) {
				v->tile_nofit = true;
				continue;
			}
			view_set_tiled(v, want, true);
		}
		int n;
		struct view **list = tiled_windows(server, &n);
		bool overflow = false;
		struct view *misfit = n ? plan(server, list, n, &overflow) : NULL;
		if (misfit && overflow) {
			/* No room even rolled up: the oldest one becomes an icon, still
			 * in reach, and the newest when restored. */
			free(list);
			view_minimize(misfit);
			continue;
		}
		if (misfit) {
			/* Too large for its cell: it floats, and the others share its
			 * room. */
			misfit->tile_nofit = true;
			free(list);
			continue;
		}
		for (int i = 0; i < n; i++)
			view_apply_tile(list[i]);
		free(list);
		break;
	}
	busy = false;
	input_pointer_changed(server);
}

bool tile_has_stack(struct server *server)
{
	int n = 0;
	struct view *v;
	wl_list_for_each(v, &server->views, link)
		n += v->tiled && !v->off_desktop;
	return n > 1;
}

static void swap_seq(struct view *a, struct view *b)
{
	long t = a->tile_seq;
	a->tile_seq = b->tile_seq;
	b->tile_seq = t;
}

void tile_iconify(struct view *view)
{
	struct server *server = view->server;
	if (!view->tiled)
		return;
	if (!view->tile_main) {
		if (!tile_can_shade(view))
			return; /* no title bar to roll up to */
		view->shaded = !view->shaded;
		tile_arrange(server);
		/* Nothing to type into any more. */
		if (view->shaded && view->activated)
			view_focus_next(view);
		return;
	}
	int n;
	struct view **list = tiled_windows(server, &n);
	if (n > 1) {
		swap_seq(list[0], list[1]);
		tile_arrange(server);
	}
	free(list);
}

void tile_promote(struct view *view)
{
	struct server *server = view->server;
	if (!view->tiled || view->tile_main)
		return;
	int n;
	struct view **list = tiled_windows(server, &n);
	if (n > 1) {
		view->shaded = false;
		swap_seq(list[0], view);
		tile_arrange(server);
		view_focus(view);
	}
	free(list);
}

void tile_unshade(struct view *view)
{
	if (!view->shaded)
		return;
	view->shaded = false;
	tile_arrange(view->server);
}

/* A tiled window or one of its dialogs: what goes up or down with the layout. */
static bool in_layout(struct view *view)
{
	return view_transient_root(view)->tiled;
}

int tile_stacking(struct server *server)
{
	int top = -1;
	bool tiled = false, floating = false;
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (!v->mapped || view_hidden(v) ||
				v->tree->node.parent != server->layer_views)
			continue;
		bool t = in_layout(v);
		if (top < 0)
			top = t;
		tiled |= t;
		floating |= !t;
	}
	return tiled && floating ? top : -1;
}

void tile_stack(struct server *server, bool raise)
{
	int count = wl_list_length(&server->views), n = 0;
	struct view **order = calloc(count, sizeof(*order));
	if (!order)
		return;
	/* Front to back: those that go on top first, each side in its own order. */
	for (int pass = 0; pass < 2; pass++) {
		struct view *v;
		wl_list_for_each(v, &server->views, link) {
			if (v->mapped && in_layout(v) == ((pass == 0) == raise))
				order[n++] = v;
		}
	}
	for (int i = n - 1; i >= 0; i--)
		view_raise_one(order[i]);
	/* The focus moves to the side that is now on top. */
	struct view *focus = NULL;
	for (int i = 0; i < n; i++) {
		if (order[i]->activated)
			focus = order[i];
	}
	if (!focus || in_layout(focus) != raise) {
		for (int i = 0; i < n; i++) {
			if (in_layout(order[i]) == raise && view_focusable(order[i]) &&
					!order[i]->shaded) {
				view_focus(order[i]);
				break;
			}
		}
	}
	free(order);
}
