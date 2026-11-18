/*
 * sizehints.c: size hints, aspect ratios and gravity, at their limits, from views
 * and the client state wlroots keeps, made on the stack. The frame is that of mwc's
 * default theme: a border of 6, a title of 25.
 */
#include <stdint.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>
#include <xcb/xcb_icccm.h>
#include "../../src/mwc.h"
#include "unit.h"

static const int border = 6, title = 25;
static const struct frame_size frame = { 6, 31, 12, 37 };
static const struct frame_size no_frame = { 0, 0, 0, 0 };

/* ---- helpers ------------------------------------------------------------------ */

static struct view x11(void)
{
	return (struct view){ .type = VIEW_XWAYLAND, .gravity = XCB_GRAVITY_NORTH_WEST };
}

static struct view xdg(void)
{
	return (struct view){ .type = VIEW_XDG, .gravity = XCB_GRAVITY_NORTH_WEST };
}

static void fit(struct view *v, int w, int h, bool incr, int *gw, int *gh)
{
	*gw = w;
	*gh = h;
	view_apply_size_hints(v, gw, gh, incr);
}

/* ---- view_apply_size_hints -------------------------------------------------------- */

TEST(size_without_hints_is_kept)
{
	struct view v = xdg();
	int w, h;
	fit(&v, 300, 200, true, &w, &h);
	CHECK_INT(w, 300);
	CHECK_INT(h, 200);
}

TEST(size_never_below_one)
{
	struct view v = xdg();
	int w, h;
	fit(&v, 0, -5, true, &w, &h);
	CHECK_INT(w, 1);
	CHECK_INT(h, 1);
}

TEST(size_min_and_max)
{
	struct view v = xdg();
	v.min_w = 200, v.min_h = 150, v.max_w = 400, v.max_h = 300;
	int w, h;
	fit(&v, 100, 100, false, &w, &h);
	CHECK_INT(w, 200);
	CHECK_INT(h, 150);
	fit(&v, 1000, 1000, false, &w, &h);
	CHECK_INT(w, 400);
	CHECK_INT(h, 300);
}

TEST(size_max_zero_is_no_limit)
{
	struct view v = xdg();
	v.min_w = 10, v.min_h = 10;
	int w, h;
	fit(&v, 5000, 4000, false, &w, &h);
	CHECK_INT(w, 5000);
	CHECK_INT(h, 4000);
}

TEST(size_steps_above_base)
{
	struct view v = x11();
	v.base_w = 20, v.base_h = 10, v.inc_w = 30, v.inc_h = 40;
	int w, h;
	fit(&v, 347, 253, true, &w, &h);
	CHECK_INT((w - 20) % 30, 0);
	CHECK_INT((h - 10) % 40, 0);
	CHECK(w <= 347 && w > 347 - 30);
	CHECK(h <= 253 && h > 253 - 40);
	/* Only while resizing: a size the client chose stays. */
	fit(&v, 347, 253, false, &w, &h);
	CHECK_INT(w, 347);
	CHECK_INT(h, 253);
}

TEST(size_aspect_fixed)
{
	struct view v = x11();
	v.min_aspect = 0.5f; /* tall: h/w at most 1/2 */
	v.max_aspect = 2.0f; /* wide: w/h at most 2 */
	int w, h;
	fit(&v, 800, 200, true, &w, &h);
	CHECK_INT(w, 400);
	CHECK_INT(h, 200);
	fit(&v, 200, 800, true, &w, &h);
	CHECK_INT(w, 200);
	CHECK_INT(h, 100);
}

/* ICCCM 4.1.2.3: a base size that only stands in for the minimum doesn't count. */
TEST(size_aspect_minus_base)
{
	struct view v = x11();
	v.min_aspect = 1.0f, v.max_aspect = 1.0f;
	v.base_w = 20, v.base_h = 0;
	int w, h;
	fit(&v, 220, 100, true, &w, &h);
	CHECK_INT(w - 20, h); /* square without the base */
	v.min_w = 20, v.min_h = 0; /* base is min: the whole size is square */
	fit(&v, 220, 100, true, &w, &h);
	CHECK_INT(w, h);
}

TEST(size_x11_capped)
{
	struct view v = x11();
	int w, h;
	fit(&v, 50000, 40000, false, &w, &h);
	CHECK_INT(w, X11_MAX_SIZE);
	CHECK_INT(h, X11_MAX_SIZE);
	/* An xdg window may be larger: VIEW_MAX_SIZE holds it elsewhere. */
	struct view x = xdg();
	fit(&x, 50000, 40000, false, &w, &h);
	CHECK_INT(w, 50000);
	CHECK_INT(h, 40000);
}

TEST(size_extremes_dont_overflow)
{
	struct view v = x11();
	v.base_w = v.base_h = X11_MAX_SIZE;
	v.min_w = v.min_h = X11_MAX_SIZE;
	v.inc_w = v.inc_h = X11_MAX_SIZE;
	int w, h;
	fit(&v, INT32_MAX - X11_MAX_SIZE, INT32_MAX - X11_MAX_SIZE, true, &w, &h);
	CHECK_INT(w, X11_MAX_SIZE);
	CHECK_INT(h, X11_MAX_SIZE);
	struct view x = xdg();
	x.min_w = x.min_h = VIEW_MAX_SIZE;
	fit(&x, 1, 1, true, &w, &h);
	CHECK_INT(w, VIEW_MAX_SIZE);
	CHECK_INT(h, VIEW_MAX_SIZE);
}

/* ---- view_aspect_follow ------------------------------------------------------------ */

static void follow(struct view *v, int w, int h, bool width_given, int *gw, int *gh)
{
	*gw = w;
	*gh = h;
	view_aspect_follow(v, gw, gh, width_given);
}

TEST(follow_the_dragged_side)
{
	struct view v = x11();
	v.min_aspect = 0.5f, v.max_aspect = 2.0f; /* 2:1 */
	int w, h;
	follow(&v, 600, 200, true, &w, &h);
	CHECK_INT(w, 600);
	CHECK_INT(h, 300);
	follow(&v, 400, 230, false, &w, &h);
	CHECK_INT(w, 460);
	CHECK_INT(h, 230);
}

TEST(follow_within_the_range_unchanged)
{
	struct view v = x11();
	v.min_aspect = 1.0f, v.max_aspect = 4.0f; /* w/h between 1 and 4 */
	int w, h;
	follow(&v, 300, 200, true, &w, &h);
	CHECK_INT(w, 300);
	CHECK_INT(h, 200);
}

TEST(follow_nothing_to_go_by)
{
	struct view v = x11();
	v.min_aspect = 0.5f, v.max_aspect = 2.0f;
	int w, h;
	follow(&v, 0, 200, true, &w, &h);
	CHECK_INT(w, 0);
	CHECK_INT(h, 200);
	follow(&v, 300, -1, false, &w, &h);
	CHECK_INT(w, 300);
	CHECK_INT(h, -1);
}

/* ICCCM 4.1.2.3, as for view_apply_size_hints(): the ratio is of the size minus the
 * base, unless the base only stands in for the minimum. */
TEST(follow_minus_base)
{
	struct view v = x11();
	v.min_aspect = 1.0f, v.max_aspect = 1.0f;
	v.base_w = 20, v.base_h = 0;
	int w, h;
	follow(&v, 220, 100, true, &w, &h);
	CHECK_INT(w, 220);
	CHECK_INT(h, 200); /* 200x200 without the base */
	v.min_w = 20, v.min_h = 0;
	follow(&v, 220, 100, true, &w, &h);
	CHECK_INT(h, 220); /* the whole size square */
}

/* Review 2026-11-18 #40: 1:10^9 asked for 10^12 and overflowed an int. */
TEST(follow_extreme_ratios_capped)
{
	struct view v = x11();
	v.min_aspect = 2e9f, v.max_aspect = 1e-9f;
	int w, h;
	follow(&v, 500, 300, true, &w, &h);
	CHECK_INT(h, X11_MAX_SIZE);
	follow(&v, 300, 500, false, &w, &h);
	CHECK(w >= 0 && w <= X11_MAX_SIZE);
	struct view t = x11();
	t.min_aspect = 1e-9f, t.max_aspect = 1e9f;
	follow(&t, 500, 30000, false, &w, &h);
	CHECK(w >= 0 && w <= X11_MAX_SIZE);
	follow(&t, 30000, 500, true, &w, &h);
	CHECK(h >= 0 && h <= X11_MAX_SIZE);
	/* 1:100 asked for 50000, more than X11 takes. */
	struct view m = x11();
	m.min_aspect = 200.0f, m.max_aspect = 0.01f;
	follow(&m, 500, 300, true, &w, &h);
	CHECK_INT(h, X11_MAX_SIZE);
}

TEST(follow_cap_counts_the_base)
{
	struct view v = x11();
	v.min_aspect = 200.0f, v.max_aspect = 0.01f;
	v.base_w = 100, v.base_h = 100; /* not the minimum: it counts apart */
	int w, h;
	follow(&v, 600, 400, true, &w, &h);
	CHECK_INT(h, X11_MAX_SIZE);
	CHECK_INT(w, 600);
}

/* ---- view_gravity_to_frame ------------------------------------------------------------ */

TEST(gravity_every_point)
{
	/* A client of 100x50 at 200,300; its frame is 112x87, the client at 6,31 in it. */
	static const struct {
		int gravity, fx, fy;
	} cases[] = {
		{ XCB_GRAVITY_NORTH_WEST, 200, 300 },
		{ XCB_GRAVITY_NORTH, 194, 300 },
		{ XCB_GRAVITY_NORTH_EAST, 188, 300 },
		{ XCB_GRAVITY_WEST, 200, 282 },
		{ XCB_GRAVITY_CENTER, 194, 282 },
		{ XCB_GRAVITY_EAST, 188, 282 },
		{ XCB_GRAVITY_SOUTH_WEST, 200, 263 },
		{ XCB_GRAVITY_SOUTH, 194, 263 },
		{ XCB_GRAVITY_SOUTH_EAST, 188, 263 },
		{ XCB_GRAVITY_STATIC, 194, 269 },
		{ 0, 200, 300 }, /* ForgetGravity, or anything unknown: north-west */
		{ 99, 200, 300 },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		struct view v = x11();
		v.gravity = cases[i].gravity;
		int fx, fy;
		view_gravity_to_frame(&v, frame, 200, 300, 100, 50, &fx, &fy);
		CHECK_INT(fx, cases[i].fx);
		CHECK_INT(fy, cases[i].fy);
	}
}

/* With the frame added, the client's own reference point lands where it asked. */
TEST(gravity_keeps_the_reference_point)
{
	struct view v = x11();
	v.gravity = XCB_GRAVITY_SOUTH_EAST;
	int fx, fy;
	view_gravity_to_frame(&v, frame, 1000, 700, 300, 200, &fx, &fy);
	CHECK_INT(fx + 300 + 2 * border, 1300);
	CHECK_INT(fy + 200 + 2 * border + title, 900);
	view_gravity_to_frame(&v, no_frame, 1000, 700, 300, 200, &fx, &fy);
	CHECK_INT(fx, 1000);
	CHECK_INT(fy, 700);
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
	struct view v = xdg();
	v.xdg_toplevel = t;
	v.base_w = v.inc_w = 7; /* left over from somewhere: reset */
	v.min_aspect = 1.0f;
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
	CHECK_INT(v.base_w, 0);
	CHECK_INT(v.inc_w, 0);
	CHECK(!view_has_aspect(&v));
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

/* ---- read_xwayland_size_hints ------------------------------------------------------------ */

static struct view x11_with(const xcb_size_hints_t *sh, struct wlr_xwayland_surface *xs)
{
	*xs = (struct wlr_xwayland_surface){ 0 };
	xs->size_hints = (xcb_size_hints_t *)sh;
	struct view v = x11();
	v.xsurface = xs;
	read_xwayland_size_hints(&v);
	return v;
}

TEST(x11_hints_none)
{
	struct wlr_xwayland_surface xs;
	struct view v = x11_with(NULL, &xs);
	CHECK_INT(v.min_w, 0);
	CHECK_INT(v.max_w, 0);
	CHECK_INT(v.base_w, 0);
	CHECK_INT(v.gravity, XCB_GRAVITY_NORTH_WEST);
	CHECK(!view_has_aspect(&v));
}

/* ICCCM 4.1.2.3: base stands in for a missing min, and min for a missing base. */
TEST(x11_hints_base_and_min_stand_in)
{
	struct wlr_xwayland_surface xs;
	xcb_size_hints_t sh = { .flags = XCB_ICCCM_SIZE_HINT_BASE_SIZE,
		.base_width = 40, .base_height = 30 };
	struct view v = x11_with(&sh, &xs);
	CHECK_INT(v.min_w, 40);
	CHECK_INT(v.min_h, 30);
	sh = (xcb_size_hints_t){ .flags = XCB_ICCCM_SIZE_HINT_P_MIN_SIZE,
		.min_width = 50, .min_height = 60 };
	v = x11_with(&sh, &xs);
	CHECK_INT(v.base_w, 50);
	CHECK_INT(v.base_h, 60);
	CHECK_INT(v.min_w, 50);
}

/* Any int32 the hints may hold, to 0 .. 32767. */
TEST(x11_hints_clamped)
{
	static const struct {
		int32_t in, out;
	} cases[] = {
		{ INT32_MIN, 0 }, { -1, 0 }, { 0, 0 }, { 640, 640 }, { 32767, 32767 },
		{ 32768, 32767 }, { INT32_MAX, 32767 },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		struct wlr_xwayland_surface xs;
		xcb_size_hints_t sh = { .flags = XCB_ICCCM_SIZE_HINT_P_MAX_SIZE,
			.max_width = cases[i].in, .max_height = cases[i].in };
		struct view v = x11_with(&sh, &xs);
		CHECK_INT(v.max_w, cases[i].out);
		CHECK_INT(v.max_h, cases[i].out);
	}
}

TEST(x11_hints_hostile)
{
	struct wlr_xwayland_surface xs;
	xcb_size_hints_t sh = {
		.flags = XCB_ICCCM_SIZE_HINT_P_MIN_SIZE | XCB_ICCCM_SIZE_HINT_P_MAX_SIZE |
			XCB_ICCCM_SIZE_HINT_BASE_SIZE | XCB_ICCCM_SIZE_HINT_P_RESIZE_INC,
		.min_width = -5, .min_height = INT32_MAX,
		.max_width = INT32_MIN, .max_height = 99999,
		.base_width = INT32_MAX, .base_height = -1,
		.width_inc = -10, .height_inc = INT32_MAX,
	};
	struct view v = x11_with(&sh, &xs);
	CHECK_INT(v.min_w, 0);
	CHECK_INT(v.min_h, X11_MAX_SIZE);
	CHECK_INT(v.max_w, 0);
	CHECK_INT(v.max_h, X11_MAX_SIZE);
	CHECK_INT(v.base_w, X11_MAX_SIZE);
	CHECK_INT(v.base_h, 0);
	CHECK_INT(v.inc_w, 0);
	CHECK_INT(v.inc_h, X11_MAX_SIZE);
	int w = INT32_MAX, h = INT32_MAX;
	view_apply_size_hints(&v, &w, &h, true);
	CHECK(w >= 1 && w <= X11_MAX_SIZE);
	CHECK(h >= 1 && h <= X11_MAX_SIZE);
}

TEST(x11_hints_aspect)
{
	struct wlr_xwayland_surface xs;
	xcb_size_hints_t sh = { .flags = XCB_ICCCM_SIZE_HINT_P_ASPECT,
		.min_aspect_num = 1, .min_aspect_den = 2,
		.max_aspect_num = 3, .max_aspect_den = 1 };
	struct view v = x11_with(&sh, &xs);
	CHECK(view_has_aspect(&v));
	CHECK(v.min_aspect == 2.0f); /* tall: h/w at most 2 */
	CHECK(v.max_aspect == 3.0f); /* wide: w/h at most 3 */
	/* A zero numerator or denominator would divide by zero: no ratio then. */
	sh.min_aspect_num = 0;
	v = x11_with(&sh, &xs);
	CHECK(!view_has_aspect(&v));
	sh.min_aspect_num = 1, sh.max_aspect_den = 0;
	v = x11_with(&sh, &xs);
	CHECK(!view_has_aspect(&v));
}

TEST(x11_hints_gravity)
{
	struct wlr_xwayland_surface xs;
	xcb_size_hints_t sh = { .flags = XCB_ICCCM_SIZE_HINT_P_WIN_GRAVITY,
		.win_gravity = XCB_GRAVITY_SOUTH_EAST };
	struct view v = x11_with(&sh, &xs);
	CHECK_INT(v.gravity, XCB_GRAVITY_SOUTH_EAST);
	sh.flags = 0;
	v = x11_with(&sh, &xs);
	CHECK_INT(v.gravity, XCB_GRAVITY_NORTH_WEST);
}

int main(void)
{
	RUN(size_without_hints_is_kept);
	RUN(size_never_below_one);
	RUN(size_min_and_max);
	RUN(size_max_zero_is_no_limit);
	RUN(size_steps_above_base);
	RUN(size_aspect_fixed);
	RUN(size_aspect_minus_base);
	RUN(size_x11_capped);
	RUN(size_extremes_dont_overflow);
	RUN(follow_the_dragged_side);
	RUN(follow_within_the_range_unchanged);
	RUN(follow_nothing_to_go_by);
	RUN(follow_minus_base);
	RUN(follow_extreme_ratios_capped);
	RUN(follow_cap_counts_the_base);
	RUN(gravity_every_point);
	RUN(gravity_keeps_the_reference_point);
	RUN(xdg_hints_taken);
	RUN(xdg_hints_capped);
	RUN(xdg_fixed);
	RUN(x11_hints_none);
	RUN(x11_hints_base_and_min_stand_in);
	RUN(x11_hints_clamped);
	RUN(x11_hints_hostile);
	RUN(x11_hints_aspect);
	RUN(x11_hints_gravity);
	return unit_done();
}
