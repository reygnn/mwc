/*
 * sizehints.c: size limits, at their limits, from views and the client state wlroots
 * keeps, made on the stack.
 */
#include <stdint.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "../../src/mwc.h"
#include "unit.h"

/* ---- helpers ------------------------------------------------------------------ */

static void fit(struct view *v, int w, int h, int *gw, int *gh)
{
	*gw = w;
	*gh = h;
	view_apply_size_hints(v, gw, gh);
}

/* ---- view_apply_size_hints -------------------------------------------------------- */

TEST(size_without_hints_is_kept)
{
	struct view v = { 0 };
	int w, h;
	fit(&v, 300, 200, &w, &h);
	CHECK_INT(w, 300);
	CHECK_INT(h, 200);
}

TEST(size_never_below_one)
{
	struct view v = { 0 };
	int w, h;
	fit(&v, 0, -5, &w, &h);
	CHECK_INT(w, 1);
	CHECK_INT(h, 1);
}

TEST(size_min_and_max)
{
	struct view v = { 0 };
	v.min_w = 200, v.min_h = 150, v.max_w = 400, v.max_h = 300;
	int w, h;
	fit(&v, 100, 100, &w, &h);
	CHECK_INT(w, 200);
	CHECK_INT(h, 150);
	fit(&v, 1000, 1000, &w, &h);
	CHECK_INT(w, 400);
	CHECK_INT(h, 300);
}

TEST(size_max_zero_is_no_limit)
{
	struct view v = { 0 };
	v.min_w = 10, v.min_h = 10;
	int w, h;
	fit(&v, 5000, 4000, &w, &h);
	CHECK_INT(w, 5000);
	CHECK_INT(h, 4000);
}

TEST(size_extremes_dont_overflow)
{
	struct view v = { 0 };
	v.min_w = v.min_h = VIEW_MAX_SIZE;
	int w, h;
	fit(&v, 1, 1, &w, &h);
	CHECK_INT(w, VIEW_MAX_SIZE);
	CHECK_INT(h, VIEW_MAX_SIZE);
	fit(&v, INT32_MAX, INT32_MIN, &w, &h);
	CHECK_INT(w, INT32_MAX);
	CHECK_INT(h, VIEW_MAX_SIZE);
}

/* ---- read_xdg_size_hints ------------------------------------------------------------ */

static struct view xdg_with(int min_w, int min_h, int max_w, int max_h,
	struct wlr_xdg_toplevel *t)
{
	*t = (struct wlr_xdg_toplevel){ 0 };
	t->current.min_width = min_w;
	t->current.min_height = min_h;
	t->current.max_width = max_w;
	t->current.max_height = max_h;
	struct view v = { .xdg_toplevel = t };
	read_xdg_size_hints(&v);
	return v;
}

TEST(xdg_hints_taken)
{
	struct wlr_xdg_toplevel t;
	struct view v = xdg_with(200, 150, 400, 300, &t);
	CHECK_INT(v.min_w, 200);
	CHECK_INT(v.min_h, 150);
	CHECK_INT(v.max_w, 400);
	CHECK_INT(v.max_h, 300);
	CHECK(!view_fixed(&v));
}

/* Review 2026-11-18 #5: wlroots lets INT32_MAX through. */
TEST(xdg_hints_capped)
{
	struct wlr_xdg_toplevel t;
	struct view v = xdg_with(INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, &t);
	CHECK_INT(v.min_w, VIEW_MAX_SIZE);
	CHECK_INT(v.min_h, VIEW_MAX_SIZE);
	CHECK_INT(v.max_w, VIEW_MAX_SIZE);
	CHECK_INT(v.max_h, VIEW_MAX_SIZE);
	v = xdg_with(0, 0, 0, 0, &t);
	CHECK_INT(v.max_w, 0); /* still no limit */
	CHECK_INT(v.max_h, 0);
}

TEST(xdg_fixed)
{
	struct wlr_xdg_toplevel t;
	struct view v = xdg_with(300, 200, 300, 200, &t);
	CHECK(view_fixed(&v));
	v = xdg_with(300, 200, 0, 0, &t);
	CHECK(!view_fixed(&v));
	v = xdg_with(300, 200, 300, 0, &t);
	CHECK(!view_fixed(&v));
}

int main(void)
{
	RUN(size_without_hints_is_kept);
	RUN(size_never_below_one);
	RUN(size_min_and_max);
	RUN(size_max_zero_is_no_limit);
	RUN(size_extremes_dont_overflow);
	RUN(xdg_hints_taken);
	RUN(xdg_hints_capped);
	RUN(xdg_fixed);
	return unit_done();
}
