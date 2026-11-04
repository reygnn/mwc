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

#define TILE_GAP 8       /* px around and between windows */
#define TILE_MAIN_PCT 55
#define TILE_MAIN_MIN 10
#define TILE_MAIN_MAX 90

static int main_pct = TILE_MAIN_PCT;

/* "60" or "60%" within the limits, else -1; name is for the log. */
static int parse_pct(const char *name, const char *s)
{
	if (!s || !*s)
		return -1;
	char *end;
	long v = strtol(s, &end, 10);
	if (*end == '%')
		end++;
	if (end == s || *end || v < TILE_MAIN_MIN || v > TILE_MAIN_MAX) {
		wlr_log(WLR_ERROR, "ignoring %s value %s, not a percentage from %d to %d",
			name, s, TILE_MAIN_MIN, TILE_MAIN_MAX);
		return -1;
	}
	return (int)v;
}

/* The environment over mwmrc, as for the other settings. */
static void read_main_width(void)
{
	int v;
	main_pct = TILE_MAIN_PCT;
	if ((v = parse_pct("MWC_TILE_MAIN_WIDTH", getenv("MWC_TILE_MAIN_WIDTH"))) > 0 ||
			(v = parse_pct("tileMainWidth", config_get("tileMainWidth"))) > 0)
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
	return view->mapped && view->wtype == WT_NORMAL && !view->minimized &&
		!view->hidden_with_parent && !view->fullscreen && !view_fixed(view) &&
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

/* The tiled windows in layout order, main window first. The caller frees it. */
static struct view **tiled_windows(struct server *server, int *n)
{
	struct view **list = calloc(wl_list_length(&server->views) + 1, sizeof(*list));
	struct view *v;
	*n = 0;
	wl_list_for_each(v, &server->views, link) {
		if (v->tiled)
			list[(*n)++] = v;
	}
	qsort(list, *n, sizeof(*list), by_seq);
	return list;
}

/* Smallest frame a window can have, from its limits. */
static void min_frame(struct view *view, int *w, int *h)
{
	int cw = 1, ch = 1;
	view_apply_size_hints(view, &cw, &ch, true);
	*w = cw + frame_extra_w(view);
	*h = ch + frame_extra_h(view);
}

/* Lay out the stack windows list[0..k) in the column at x, width w: shaded ones
 * take their title bar, the open ones share what is left. When that is too little
 * for them, open ones from the bottom up are shaded (the focused one last). Returns
 * false if the column still can't hold them. */
static bool layout_stack(struct view **list, int k, struct wlr_box a, int x, int w)
{
	for (;;) {
		int shaded = 0, n_open = 0, need = 0;
		for (int i = 0; i < k; i++) {
			int mw, mh;
			min_frame(list[i], &mw, &mh);
			if (list[i]->shaded) {
				shaded += frame_extra_h(list[i]);
			} else {
				n_open++;
				if (mh > need)
					need = mh;
			}
		}
		int room = a.height - 2 * TILE_GAP - (k - 1) * TILE_GAP - shaded;
		int each = n_open ? room / n_open : 0;
		if (n_open && each < need) {
			/* Shade one more, if any can be. */
			struct view *pick = NULL;
			for (int i = k - 1; i >= 0 && !pick; i--) {
				if (!list[i]->shaded && tile_can_shade(list[i]) &&
						!list[i]->activated)
					pick = list[i];
			}
			for (int i = k - 1; i >= 0 && !pick; i--) {
				if (!list[i]->shaded && tile_can_shade(list[i]))
					pick = list[i];
			}
			if (pick) {
				pick->shaded = true;
				continue;
			}
		}
		if (room < 0 || (n_open && each < need))
			return false;
		int y = a.y + TILE_GAP, open_left = n_open;
		for (int i = 0; i < k; i++) {
			struct view *v = list[i];
			int h = frame_extra_h(v);
			if (!v->shaded) /* the last open one gets the rest */
				h = --open_left ? each : room - each * (n_open - 1);
			v->tile = (struct wlr_box){ x, y, w, h };
			v->tile_main = false;
			y += h + TILE_GAP;
		}
		return true;
	}
}

/* The output the layout covers: the first one that was added. */
static struct wlr_box layout_area(struct server *server)
{
	struct output *first = wl_container_of(server->outputs.prev, first, link);
	return first->usable;
}

/* Cells for list[0..n), main window first. Returns a window that can't fit its
 * cell, or NULL; *overflow says the stack column is full even with every window in
 * it shaded. */
static struct view *plan(struct server *server, struct view **list, int n,
	bool *overflow)
{
	*overflow = false;
	struct wlr_box a = layout_area(server);
	/* Icons along the bottom stay in view: the layout ends above the highest row
	 * of them (they keep their own 8 px margin). */
	int bottom = a.y + a.height;
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->minimized && v->icon_placed && v->icon_x >= a.x &&
				v->icon_x < a.x + a.width && v->icon_y >= a.y &&
				v->icon_y < bottom)
			bottom = v->icon_y;
	}
	a.height = bottom - a.y > 2 * TILE_GAP + 1 ? bottom - a.y : 2 * TILE_GAP + 1;
	int top = a.y + TILE_GAP, h = a.height - 2 * TILE_GAP;
	/* The main window is never shaded. */
	list[0]->shaded = false;
	if (n == 1) {
		list[0]->tile = (struct wlr_box){ a.x + TILE_GAP, top,
			a.width - 2 * TILE_GAP, h };
	} else {
		int inner = a.width - 3 * TILE_GAP;
		int mw = inner * main_pct / 100, sw = inner - mw;
		list[0]->tile = (struct wlr_box){ a.x + 2 * TILE_GAP + sw, top, mw, h };
		if (!layout_stack(list + 1, n - 1, a, a.x + TILE_GAP, sw)) {
			*overflow = true;
			return list[n - 1];
		}
	}
	list[0]->tile_main = true;
	for (int i = 0; i < n; i++) {
		int mw, mh;
		min_frame(list[i], &mw, &mh);
		if (mw > list[i]->tile.width ||
				(!list[i]->shaded && mh > list[i]->tile.height))
			return list[i];
	}
	return NULL;
}

void tile_arrange(struct server *server)
{
	/* Leaving or entering the layout reconfigures windows, which must not start
	 * another round from inside this one. */
	static bool busy;
	if (busy || wl_list_empty(&server->outputs))
		return;
	busy = true;

	struct view *v;
	wl_list_for_each(v, &server->views, link)
		v->tile_nofit = false;
	for (;;) {
		wl_list_for_each(v, &server->views, link) {
			bool want = server->tiling && tileable(v);
			if (want != v->tiled)
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
}

bool tile_has_stack(struct server *server)
{
	int n = 0;
	struct view *v;
	wl_list_for_each(v, &server->views, link)
		n += v->tiled;
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
		if (!v->mapped || v->minimized || v->hidden_with_parent ||
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
