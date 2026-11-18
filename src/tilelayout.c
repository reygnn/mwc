/*
 * tilelayout.c - the tiling layout's arithmetic: the area it may use beside the
 * icons, the main window's cell and the stack's, rolling stack windows up when the
 * column is too short, and which window fits nowhere. From what tile.c tells it of
 * each window; it calls nothing. Tested in tests/unit.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <wlr/util/log.h>
#include "mwc.h"

#define TILE_GAP MWC_GAP /* px around and between windows */

int tile_parse_pct(const char *name, const char *s)
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

/* What the layout may use of the area, edge by edge. */
struct strip {
	int left, right, top, bottom;
};

/* The edge the icons pile up from moves in past b. */
static void leave_free(struct strip *st, const struct config_placement *p,
	struct wlr_box b)
{
	if (p->horizontal && p->from_bottom) {
		if (b.y < st->bottom)
			st->bottom = b.y;
	} else if (p->horizontal) {
		if (b.y + b.height > st->top)
			st->top = b.y + b.height;
	} else if (p->from_right) {
		if (b.x < st->right)
			st->right = b.x;
	} else if (b.x + b.width > st->left) {
		st->left = b.x + b.width;
	}
}

struct wlr_box tile_area(struct wlr_box a, const struct config_placement *p,
	const struct wlr_box *icons, int n)
{
	struct strip st = { a.x, a.x + a.width, a.y, a.y + a.height };
	for (int i = 0; i < n; i++)
		leave_free(&st, p, icons[i]);
	/* Never less than the gaps and a pixel, however many icons. */
	int min = 2 * TILE_GAP + 1;
	a.x = st.left;
	a.width = st.right - st.left > min ? st.right - st.left : min;
	a.y = st.top;
	a.height = st.bottom - st.top > min ? st.bottom - st.top : min;
	return a;
}

/* Lay out the stack windows w[0..k) in the column at x, width cw: shaded ones take
 * their title bar, the open ones share what is left. When that is too little for
 * them, open ones from the bottom up are shaded (the focused one last). Returns
 * false if the column still can't hold them. */
static bool layout_stack(struct tile_window *w, int k, struct wlr_box a, int x, int cw)
{
	for (;;) {
		int shaded = 0, n_open = 0, need = 0;
		for (int i = 0; i < k; i++) {
			if (w[i].shaded) {
				shaded += w[i].shaded_h;
			} else {
				n_open++;
				if (w[i].min_h > need)
					need = w[i].min_h;
			}
		}
		int room = a.height - 2 * TILE_GAP - (k - 1) * TILE_GAP - shaded;
		int each = n_open ? room / n_open : 0;
		if (n_open && each < need) {
			/* Shade one more, if any can be. */
			struct tile_window *pick = NULL;
			for (int i = k - 1; i >= 0 && !pick; i--) {
				if (!w[i].shaded && w[i].can_shade && !w[i].focused)
					pick = &w[i];
			}
			for (int i = k - 1; i >= 0 && !pick; i--) {
				if (!w[i].shaded && w[i].can_shade)
					pick = &w[i];
			}
			if (pick) {
				pick->shaded = true;
				pick->auto_shaded = true;
				continue;
			}
		}
		if (room < 0 || (n_open && each < need))
			return false;
		int y = a.y + TILE_GAP, open_left = n_open;
		for (int i = 0; i < k; i++) {
			int h = w[i].shaded_h;
			if (!w[i].shaded) /* the last open one gets the rest */
				h = --open_left ? each : room - each * (n_open - 1);
			w[i].cell = (struct wlr_box){ x, y, cw, h };
			w[i].main = false;
			y += h + TILE_GAP;
		}
		return true;
	}
}

int tile_plan(struct tile_window *w, int n, struct wlr_box a, int main_pct,
	bool *overflow)
{
	*overflow = false;
	int top = a.y + TILE_GAP, h = a.height - 2 * TILE_GAP;
	/* What the layout rolled up for lack of room in an earlier round gets another
	 * chance, the room may be there now. One that has lost its title bar since has
	 * nothing left to roll up to, nor a button to unroll it: it opens again. */
	for (int i = 0; i < n; i++) {
		if (w[i].auto_shaded || (w[i].shaded && !w[i].can_shade)) {
			w[i].shaded = false;
			w[i].auto_shaded = false;
		}
	}
	/* The main window is never shaded. */
	w[0].shaded = false;
	if (n == 1) {
		w[0].cell = (struct wlr_box){ a.x + TILE_GAP, top, a.width - 2 * TILE_GAP, h };
	} else {
		int inner = a.width - 3 * TILE_GAP;
		int mw = inner * main_pct / 100, sw = inner - mw;
		/* Those that fit in no cell, before anything gets rolled up for them. A
		 * window too tall for the whole column could never be unrolled. */
		if (w[0].min_w > mw || w[0].min_h > h)
			return 0;
		for (int i = 1; i < n; i++) {
			if (w[i].min_w > sw || w[i].min_h > h)
				return i;
		}
		w[0].cell = (struct wlr_box){ a.x + 2 * TILE_GAP + sw, top, mw, h };
		if (!layout_stack(w + 1, n - 1, a, a.x + TILE_GAP, sw)) {
			*overflow = true;
			return n - 1;
		}
	}
	w[0].main = true;
	for (int i = 0; i < n; i++) {
		if (w[i].min_w > w[i].cell.width ||
				(!w[i].shaded && w[i].min_h > w[i].cell.height))
			return i;
	}
	return -1;
}
