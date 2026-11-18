/*
 * tilelayout.c: tileMainWidth, the area beside the icons, the cells of the main window
 * and the stack with their gaps, rolling stack windows up when the column is short,
 * and what fits nowhere. Windows are tile_window values the test fills: a frame of
 * 6 around and 25 on top, 37 rolled up, as in mwc's default theme.
 */
#include <stdarg.h>
#include <string.h>
#include <wlr/util/log.h>
#include "../../src/mwc.h"
#include "unit.h"

static char logged[1024];

static void log_to_buffer(enum wlr_log_importance importance, const char *fmt,
	va_list args)
{
	size_t n = strlen(logged);
	vsnprintf(logged + n, sizeof(logged) - n, fmt, args);
}

static bool box_is(struct wlr_box b, int x, int y, int w, int h)
{
	if (b.x == x && b.y == y && b.width == w && b.height == h)
		return true;
	fprintf(stderr, "  box %d,%d %dx%d, expected %d,%d %dx%d\n", b.x, b.y, b.width,
		b.height, x, y, w, h);
	return false;
}

/* A window that can be as small as its frame allows. */
static struct tile_window win(void)
{
	return (struct tile_window){ .min_w = 13, .min_h = 38, .shaded_h = 37,
		.can_shade = true };
}

static const struct wlr_box screen = { 0, 0, 1280, 720 };

/* ---- tileMainWidth ------------------------------------------------------------ */

TEST(percentages)
{
	logged[0] = '\0';
	CHECK_INT(tile_parse_pct("x", "60"), 60);
	CHECK_INT(tile_parse_pct("x", "60%"), 60);
	CHECK_INT(tile_parse_pct("x", "10"), TILE_MAIN_MIN);
	CHECK_INT(tile_parse_pct("x", "90"), TILE_MAIN_MAX);
	CHECK_INT(tile_parse_pct("x", NULL), -1);
	CHECK_INT(tile_parse_pct("x", ""), -1);
	CHECK(!*logged); /* unset says nothing */
	static const char *const bad[] = {
		"9", "91", "-60", "60%%", "60 %", "%", "abc", "60px", "99999999999999999999",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		logged[0] = '\0';
		CHECK_INT(tile_parse_pct("tileMainWidth", bad[i]), -1);
		CHECK(strstr(logged, "ignoring tileMainWidth value") != NULL);
	}
}

/* ---- the area beside the icons ------------------------------------------------ */

static const struct config_placement bottom = { .horizontal = true, .from_bottom = true };
static const struct config_placement top = { .horizontal = true };
static const struct config_placement right = { .from_right = true };
static const struct config_placement left = { 0 };

TEST(area_without_icons)
{
	CHECK(box_is(tile_area(screen, &bottom, NULL, 0), 0, 0, 1280, 720));
	struct wlr_box b = { 100, 30, 800, 600 };
	CHECK(box_is(tile_area(b, &right, NULL, 0), 100, 30, 800, 600));
}

/* Up to the innermost row or column of them, on the edge they pile up from. */
TEST(area_beside_icons)
{
	struct wlr_box icons[] = { { 8, 632, 80, 80 }, { 96, 548, 80, 80 } };
	CHECK(box_is(tile_area(screen, &bottom, icons, 2), 0, 0, 1280, 548));
	struct wlr_box t[] = { { 8, 8, 80, 80 }, { 8, 96, 80, 80 } };
	CHECK(box_is(tile_area(screen, &top, t, 2), 0, 176, 1280, 544));
	struct wlr_box r[] = { { 1192, 8, 80, 80 }, { 1104, 8, 80, 80 } };
	CHECK(box_is(tile_area(screen, &right, r, 2), 0, 0, 1104, 720));
	struct wlr_box l[] = { { 8, 8, 80, 80 }, { 8, 96, 80, 80 } };
	CHECK(box_is(tile_area(screen, &left, l, 2), 88, 0, 1192, 720));
	/* An icon box is one such box. */
	struct wlr_box box = { 0, 0, 1280, 100 };
	CHECK(box_is(tile_area(screen, &top, &box, 1), 0, 100, 1280, 620));
}

/* However many icons there are, the gaps and a pixel are left. */
TEST(area_never_empty)
{
	struct wlr_box all = { 0, -50, 1280, 1000 };
	struct wlr_box a = tile_area(screen, &bottom, &all, 1);
	CHECK_INT(a.height, 2 * MWC_GAP + 1);
	CHECK_INT(a.width, 1280);
	a = tile_area(screen, &left, &all, 1);
	CHECK_INT(a.width, 2 * MWC_GAP + 1);
}

/* ---- cells ------------------------------------------------------------------ */

static int plan(struct tile_window *w, int n, struct wlr_box a, int pct, bool *overflow)
{
	bool o;
	int r = tile_plan(w, n, a, pct, overflow ? overflow : &o);
	if (!overflow)
		CHECK(!o);
	return r;
}

TEST(one_window)
{
	struct tile_window w[1] = { win() };
	CHECK_INT(plan(w, 1, screen, 55, NULL), -1);
	CHECK(box_is(w[0].cell, 8, 8, 1264, 704));
	CHECK(w[0].main);
	struct wlr_box b = { 1280, 30, 1280, 690 };
	CHECK_INT(plan(w, 1, b, 55, NULL), -1);
	CHECK(box_is(w[0].cell, 1288, 38, 1264, 674));
}

/* The main window on the right, its share of the width less the three gaps. */
TEST(main_and_stack)
{
	struct tile_window w[2] = { win(), win() };
	w[1].main = true; /* left over from a round where it was the main one */
	CHECK_INT(plan(w, 2, screen, 55, NULL), -1);
	/* 1280 - 3 * 8 = 1256; 55 % of it 690, the stack 566. */
	CHECK(box_is(w[1].cell, 8, 8, 566, 704));
	CHECK(box_is(w[0].cell, 582, 8, 690, 704));
	CHECK(w[0].main && !w[1].main);
	CHECK_INT(w[0].cell.x - (w[1].cell.x + w[1].cell.width), MWC_GAP);
	CHECK_INT(1280 - (w[0].cell.x + w[0].cell.width), MWC_GAP);
	CHECK_INT(plan(w, 2, screen, 90, NULL), -1);
	CHECK_INT(w[0].cell.width, 1130);
	CHECK_INT(w[1].cell.width, 126);
	CHECK_INT(plan(w, 2, screen, 10, NULL), -1);
	CHECK_INT(w[0].cell.width, 125);
}

/* The stack shares its column, the gaps between; the last one gets what rounding left. */
TEST(stack_shares_the_column)
{
	struct tile_window w[3] = { win(), win(), win() };
	CHECK_INT(plan(w, 3, screen, 55, NULL), -1);
	/* 720 - 2 * 8 - 8 = 696, 348 each. */
	CHECK(box_is(w[1].cell, 8, 8, 566, 348));
	CHECK(box_is(w[2].cell, 8, 364, 566, 348));
	struct wlr_box odd = { 0, 0, 1280, 721 };
	CHECK_INT(plan(w, 3, odd, 55, NULL), -1);
	CHECK_INT(w[1].cell.height, 348);
	CHECK_INT(w[2].cell.height, 349);
	CHECK_INT(w[2].cell.y + w[2].cell.height, 721 - MWC_GAP);
	struct tile_window four[4] = { win(), win(), win(), win() };
	CHECK_INT(plan(four, 4, screen, 55, NULL), -1);
	CHECK_INT(four[1].cell.height + four[2].cell.height + four[3].cell.height + 2 * 8,
		704);
}

/* ---- rolling up ------------------------------------------------------------------ */

/* Four stack windows of at least 200 in 704 - 3 * 8: the bottom one rolls up. */
TEST(roll_up_from_the_bottom)
{
	struct tile_window w[5] = { win(), win(), win(), win(), win() };
	for (int i = 1; i < 5; i++)
		w[i].min_h = 200;
	CHECK_INT(plan(w, 5, screen, 55, NULL), -1);
	CHECK(w[4].shaded && w[4].auto_shaded);
	CHECK(!w[1].shaded && !w[2].shaded && !w[3].shaded);
	CHECK_INT(w[4].cell.height, 37);
	CHECK(w[1].cell.height >= 200 && w[3].cell.height >= 200);
	CHECK_INT(w[4].cell.y + 37, 720 - MWC_GAP);
	/* The focused one last: the one above it goes instead. */
	struct tile_window f[5] = { win(), win(), win(), win(), win() };
	for (int i = 1; i < 5; i++)
		f[i].min_h = 200;
	f[4].focused = true;
	CHECK_INT(plan(f, 5, screen, 55, NULL), -1);
	CHECK(!f[4].shaded && f[3].shaded);
}

/* Rolled up by the layout, it opens again when there is room; by the user, it stays. */
TEST(rolled_up_next_round)
{
	struct tile_window w[3] = { win(), win(), win() };
	w[1].shaded = w[1].auto_shaded = true;
	w[2].shaded = true;
	CHECK_INT(plan(w, 3, screen, 55, NULL), -1);
	CHECK(!w[1].shaded && !w[1].auto_shaded);
	CHECK(w[2].shaded);
	CHECK_INT(w[2].cell.height, 37);
	CHECK_INT(w[1].cell.height, 704 - 8 - 37);
	/* One that lost its title bar can't stay rolled up. */
	struct tile_window t[2] = { win(), win() };
	t[1].shaded = true;
	t[1].can_shade = false;
	CHECK_INT(plan(t, 2, screen, 55, NULL), -1);
	CHECK(!t[1].shaded);
	/* The main window never is. */
	struct tile_window m[2] = { win(), win() };
	m[0].shaded = true;
	CHECK_INT(plan(m, 2, screen, 55, NULL), -1);
	CHECK(!m[0].shaded);
}

/* ---- no room ------------------------------------------------------------------ */

TEST(too_large_for_its_cell)
{
	struct tile_window w[3] = { win(), win(), win() };
	w[0].min_w = 691; /* the main cell is 690 */
	CHECK_INT(plan(w, 3, screen, 55, NULL), 0);
	w[0] = win();
	w[0].min_h = 705;
	CHECK_INT(plan(w, 3, screen, 55, NULL), 0);
	w[0] = win();
	w[2].min_w = 567; /* the stack is 566 */
	CHECK_INT(plan(w, 3, screen, 55, NULL), 2);
	w[2] = win();
	w[1].min_h = 705; /* taller than the whole column: never to be unrolled */
	CHECK_INT(plan(w, 3, screen, 55, NULL), 1);
	CHECK(!w[2].shaded); /* nothing rolled up for it */
	struct tile_window one[1] = { win() };
	one[0].min_w = 1265;
	CHECK_INT(plan(one, 1, screen, 55, NULL), 0);
	one[0].min_w = 1264;
	CHECK_INT(plan(one, 1, screen, 55, NULL), -1);
}

TEST(stack_overflows)
{
	/* Twenty stack windows in 704: rolled up, 20 * 37 + 19 * 8 = 892. */
	struct tile_window w[21];
	for (int i = 0; i < 21; i++)
		w[i] = win();
	bool overflow = false;
	CHECK_INT(plan(w, 21, screen, 55, &overflow), 20);
	CHECK(overflow);
	/* Those that can't be rolled up, when the others aren't enough. */
	struct tile_window t[4] = { win(), win(), win(), win() };
	for (int i = 1; i < 4; i++) {
		t[i].min_h = 300;
		t[i].can_shade = false;
	}
	CHECK_INT(plan(t, 4, screen, 55, &overflow), 3);
	CHECK(overflow);
	/* Enough of them can: no overflow. */
	t[3].can_shade = true;
	t[2].can_shade = true;
	CHECK_INT(plan(t, 4, screen, 55, &overflow), -1);
	CHECK(!overflow);
}

int main(void)
{
	wlr_log_init(WLR_ERROR, log_to_buffer);
	RUN(percentages);
	RUN(area_without_icons);
	RUN(area_beside_icons);
	RUN(area_never_empty);
	RUN(one_window);
	RUN(main_and_stack);
	RUN(stack_shares_the_column);
	RUN(roll_up_from_the_bottom);
	RUN(rolled_up_next_round);
	RUN(too_large_for_its_cell);
	RUN(stack_overflows);
	return unit_done();
}
