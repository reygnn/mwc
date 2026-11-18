/*
 * snap.c: the snap positions and their gaps, the zones at an output's edges, the
 * column beside a dock, Super+arrow's steps and Super+Shift+arrow's edges. One
 * output, stubbed, of 1280x720 at out.x; the frame is 6 around and 25 on top.
 */
#include "../../src/snap.c"
#include "unit.h"

/* ---- stubs ------------------------------------------------------------------ */

static struct output out;
static struct wlr_box full; /* the output's box in the layout */
static bool no_output, horizontal_dock;
static int snapped = -1; /* -1: view_snap() wasn't called */
static bool minimized;
static int moved_x, moved_y;

int frame_extra_w(struct view *view) { return 12; }
int frame_extra_h(struct view *view) { return 37; }
bool dock_horizontal(void) { return horizontal_dock; }

struct output *output_at(struct server *server, double lx, double ly)
{
	return no_output ? NULL : &out;
}

struct wlr_box output_full_area(struct server *server, double lx, double ly)
{
	return full;
}

struct wlr_box output_usable_area(struct server *server, double lx, double ly)
{
	return out.usable;
}

/* As in view.c. */
bool output_dock_in_the_way(struct output *o, struct wlr_box b)
{
	struct wlr_box hit;
	return o && wlr_box_intersection(&hit, &b, &o->dock);
}

void view_snap(struct view *view, enum snap s, double px, double py) { snapped = (int)s; }
void view_minimize(struct view *view) { minimized = true; }
void view_set_position(struct view *view, int x, int y)
{
	moved_x = x;
	moved_y = y;
}

/* ---- helpers ------------------------------------------------------------------ */

static void one_output(int x)
{
	out = (struct output){ 0 };
	full = (struct wlr_box){ x, 0, 1280, 720 };
	out.usable = out.open = full;
	no_output = horizontal_dock = false;
}

static bool box_is(struct wlr_box b, int x, int y, int w, int h)
{
	return b.x == x && b.y == y && b.width == w && b.height == h;
}

/* ---- snap_rect ------------------------------------------------------------------ */

TEST(rect_positions)
{
	struct wlr_box a = { 0, 0, 1280, 720 };
	CHECK(box_is(snap_rect(SNAP_NONE, a), 0, 0, 1280, 720));
	CHECK(box_is(snap_rect(SNAP_MAXIMIZE, a), 0, 0, 1280, 720));
	CHECK(box_is(snap_rect(SNAP_LEFT, a), 8, 8, 628, 704));
	CHECK(box_is(snap_rect(SNAP_RIGHT, a), 644, 8, 628, 704));
	CHECK(box_is(snap_rect(SNAP_TOP_LEFT, a), 8, 8, 628, 348));
	CHECK(box_is(snap_rect(SNAP_TOP_RIGHT, a), 644, 8, 628, 348));
	CHECK(box_is(snap_rect(SNAP_BOTTOM_LEFT, a), 8, 364, 628, 348));
	CHECK(box_is(snap_rect(SNAP_BOTTOM_RIGHT, a), 644, 364, 628, 348));
	/* On another output, all the same, moved. */
	struct wlr_box b = { 1280, 30, 1280, 690 };
	CHECK(box_is(snap_rect(SNAP_RIGHT, b), 1924, 38, 628, 674));
}

/* For any size, odd ones too: the halves fill the area but for the gaps, which are
 * MWC_GAP everywhere, and the quarters line up with them. */
TEST(rect_gaps_for_every_size)
{
	const int g = MWC_GAP;
	for (int w = 3 * g; w <= 2000; w += 7) {
		for (int h = 3 * g; h <= 1200; h += 13) {
			struct wlr_box a = { 100, 50, w, h };
			struct wlr_box l = snap_rect(SNAP_LEFT, a), r = snap_rect(SNAP_RIGHT, a);
			struct wlr_box tl = snap_rect(SNAP_TOP_LEFT, a);
			struct wlr_box br = snap_rect(SNAP_BOTTOM_RIGHT, a);
			if (l.x != a.x + g || l.y != a.y + g || r.x != l.x + l.width + g ||
					r.x + r.width != a.x + a.width - g ||
					l.height != a.height - 2 * g || r.width - l.width > 1 ||
					r.width < l.width || tl.x != l.x || tl.width != l.width ||
					br.x != r.x || br.width != r.width ||
					br.y != tl.y + tl.height + g ||
					br.y + br.height != a.y + a.height - g) {
				CHECK(!"gaps for every size");
				fprintf(stderr, "  area %dx%d\n", w, h);
				return;
			}
		}
	}
	CHECK(true);
}

/* ---- snap_name ------------------------------------------------------------------ */

TEST(names)
{
	CHECK(!strcmp(snap_name(SNAP_NONE), ""));
	for (enum snap s = SNAP_LEFT; s <= SNAP_MAXIMIZE; s++)
		CHECK(*snap_name(s));
	CHECK(!strcmp(snap_name(SNAP_MAXIMIZE), "Maximize"));
}

/* ---- snap_zone_at ------------------------------------------------------------------ */

static enum snap zone(int funcs, double x, double y)
{
	struct view v = { .funcs = funcs };
	return snap_zone_at(&v, x, y);
}

TEST(zones_at_the_edges)
{
	const int f = FUNC_RESIZE | FUNC_MAXIMIZE;
	one_output(0);
	CHECK_INT(zone(f, 0, 360), SNAP_LEFT);
	CHECK_INT(zone(f, 2, 360), SNAP_LEFT); /* SNAP_EDGE */
	CHECK_INT(zone(f, 3, 360), SNAP_NONE);
	CHECK_INT(zone(f, 1279, 360), SNAP_RIGHT);
	CHECK_INT(zone(f, 1277, 360), SNAP_RIGHT);
	CHECK_INT(zone(f, 1276, 360), SNAP_NONE);
	CHECK_INT(zone(f, 640, 0), SNAP_MAXIMIZE);
	CHECK_INT(zone(f, 640, 719), SNAP_NONE); /* the bottom edge, midway: nothing */
	CHECK_INT(zone(f, 640, 360), SNAP_NONE);
}

TEST(zones_in_the_corners)
{
	const int f = FUNC_RESIZE | FUNC_MAXIMIZE;
	one_output(0);
	CHECK_INT(zone(f, 0, 0), SNAP_TOP_LEFT);
	CHECK_INT(zone(f, 0, 63), SNAP_TOP_LEFT); /* SNAP_CORNER */
	CHECK_INT(zone(f, 0, 64), SNAP_LEFT);
	CHECK_INT(zone(f, 63, 0), SNAP_TOP_LEFT);
	CHECK_INT(zone(f, 64, 0), SNAP_MAXIMIZE);
	CHECK_INT(zone(f, 1279, 0), SNAP_TOP_RIGHT);
	CHECK_INT(zone(f, 1216, 0), SNAP_TOP_RIGHT);
	CHECK_INT(zone(f, 1215, 0), SNAP_MAXIMIZE);
	CHECK_INT(zone(f, 0, 719), SNAP_BOTTOM_LEFT);
	CHECK_INT(zone(f, 0, 656), SNAP_BOTTOM_LEFT);
	CHECK_INT(zone(f, 0, 655), SNAP_LEFT);
	CHECK_INT(zone(f, 10, 719), SNAP_BOTTOM_LEFT);
	CHECK_INT(zone(f, 1279, 719), SNAP_BOTTOM_RIGHT);
}

TEST(zones_on_another_output)
{
	one_output(1280);
	const int f = FUNC_RESIZE | FUNC_MAXIMIZE;
	CHECK_INT(zone(f, 1280, 360), SNAP_LEFT);
	CHECK_INT(zone(f, 1283, 360), SNAP_NONE);
	CHECK_INT(zone(f, 2559, 360), SNAP_RIGHT);
	CHECK_INT(zone(f, 1920, 0), SNAP_MAXIMIZE);
}

TEST(zones_what_the_window_may)
{
	one_output(0);
	CHECK_INT(zone(FUNC_RESIZE, 640, 0), SNAP_NONE); /* no maximize */
	CHECK_INT(zone(FUNC_RESIZE, 0, 0), SNAP_TOP_LEFT);
	CHECK_INT(zone(FUNC_MAXIMIZE, 0, 360), SNAP_NONE); /* no resize */
	struct view v = { .funcs = FUNC_RESIZE | FUNC_MAXIMIZE, .fullscreen = true };
	CHECK_INT(snap_zone_at(&v, 0, 360), SNAP_NONE);
}

/* ---- snap_area ------------------------------------------------------------------ */

TEST(area_beside_a_dock)
{
	one_output(0);
	out.open = (struct wlr_box){ 0, 0, 1280, 720 };
	out.usable = (struct wlr_box){ 0, 0, 1280, 656 };
	out.dock = (struct wlr_box){ 900, 656, 380, 64 }; /* bottom right */
	CHECK(box_is(snap_area(NULL, SNAP_LEFT, 0, 0), 0, 0, 1280, 656)); /* not along */
	horizontal_dock = true;
	/* The left column doesn't reach the dock: it keeps the strip. */
	CHECK(box_is(snap_area(NULL, SNAP_LEFT, 0, 0), 0, 0, 1280, 720));
	CHECK(box_is(snap_area(NULL, SNAP_BOTTOM_LEFT, 0, 0), 0, 0, 1280, 720));
	/* The right column does: all of it stays out of the strip, quarters too. */
	CHECK(box_is(snap_area(NULL, SNAP_RIGHT, 0, 0), 0, 0, 1280, 656));
	CHECK(box_is(snap_area(NULL, SNAP_TOP_RIGHT, 0, 0), 0, 0, 1280, 656));
	CHECK(box_is(snap_area(NULL, SNAP_MAXIMIZE, 0, 0), 0, 0, 1280, 656));
	no_output = true;
	CHECK(box_is(snap_area(NULL, SNAP_LEFT, 0, 0), 0, 0, 1280, 656));
}

/* ---- snap_step ------------------------------------------------------------------ */

static int step(enum snap from, bool maximized, enum snap_dir dir, int funcs)
{
	struct view v = { .snap = from, .maximized = maximized, .funcs = funcs };
	snapped = -1;
	minimized = false;
	snap_step(&v, dir);
	return snapped;
}

/* As in Windows 11. -1: nothing happens. */
TEST(step_table)
{
	const int f = FUNC_RESIZE | FUNC_MAXIMIZE | FUNC_MINIMIZE;
	static const struct {
		enum snap from;
		bool max;
		enum snap_dir dir;
		int to;
	} cases[] = {
		{ SNAP_NONE, false, SNAP_DIR_LEFT, SNAP_LEFT },
		{ SNAP_NONE, false, SNAP_DIR_RIGHT, SNAP_RIGHT },
		{ SNAP_NONE, false, SNAP_DIR_UP, SNAP_MAXIMIZE },
		{ SNAP_LEFT, false, SNAP_DIR_LEFT, -1 },
		{ SNAP_LEFT, false, SNAP_DIR_RIGHT, SNAP_NONE },
		{ SNAP_LEFT, false, SNAP_DIR_UP, SNAP_TOP_LEFT },
		{ SNAP_LEFT, false, SNAP_DIR_DOWN, SNAP_BOTTOM_LEFT },
		{ SNAP_RIGHT, false, SNAP_DIR_LEFT, SNAP_NONE },
		{ SNAP_RIGHT, false, SNAP_DIR_UP, SNAP_TOP_RIGHT },
		{ SNAP_RIGHT, false, SNAP_DIR_DOWN, SNAP_BOTTOM_RIGHT },
		{ SNAP_TOP_LEFT, false, SNAP_DIR_RIGHT, SNAP_TOP_RIGHT },
		{ SNAP_TOP_LEFT, false, SNAP_DIR_DOWN, SNAP_LEFT },
		{ SNAP_TOP_LEFT, false, SNAP_DIR_UP, -1 },
		{ SNAP_TOP_RIGHT, false, SNAP_DIR_LEFT, SNAP_TOP_LEFT },
		{ SNAP_TOP_RIGHT, false, SNAP_DIR_DOWN, SNAP_RIGHT },
		{ SNAP_BOTTOM_LEFT, false, SNAP_DIR_UP, SNAP_LEFT },
		{ SNAP_BOTTOM_LEFT, false, SNAP_DIR_RIGHT, SNAP_BOTTOM_RIGHT },
		{ SNAP_BOTTOM_LEFT, false, SNAP_DIR_DOWN, -1 },
		{ SNAP_BOTTOM_RIGHT, false, SNAP_DIR_UP, SNAP_RIGHT },
		{ SNAP_BOTTOM_RIGHT, false, SNAP_DIR_LEFT, SNAP_BOTTOM_LEFT },
		{ SNAP_NONE, true, SNAP_DIR_UP, -1 },
		{ SNAP_NONE, true, SNAP_DIR_DOWN, SNAP_NONE },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		int got = step(cases[i].from, cases[i].max, cases[i].dir, f);
		if (got != cases[i].to)
			fprintf(stderr, "  from %d%s, direction %d\n", cases[i].from,
				cases[i].max ? " maximized" : "", cases[i].dir);
		CHECK_INT(got, cases[i].to);
	}
}

TEST(step_down_from_floating_minimizes)
{
	CHECK_INT(step(SNAP_NONE, false, SNAP_DIR_DOWN, FUNC_RESIZE | FUNC_MINIMIZE), -1);
	CHECK(minimized);
	step(SNAP_NONE, false, SNAP_DIR_DOWN, FUNC_RESIZE);
	CHECK(!minimized);
}

TEST(step_what_the_window_may)
{
	CHECK_INT(step(SNAP_NONE, false, SNAP_DIR_UP, FUNC_RESIZE), -1); /* no maximize */
	CHECK_INT(step(SNAP_NONE, false, SNAP_DIR_LEFT, FUNC_MAXIMIZE), -1); /* no resize */
	struct view v = { .funcs = FUNC_RESIZE, .fullscreen = true };
	snapped = -1;
	snap_step(&v, SNAP_DIR_LEFT);
	CHECK_INT(snapped, -1);
	v = (struct view){ .funcs = FUNC_RESIZE, .minimized = true };
	snap_step(&v, SNAP_DIR_LEFT);
	CHECK_INT(snapped, -1);
	snap_step(NULL, SNAP_DIR_LEFT);
	CHECK_INT(snapped, -1);
}

/* ---- view_move_to_edge ------------------------------------------------------------ */

static void to_edge(int x, int y, int w, int h, enum snap_dir dir)
{
	struct view v = { .funcs = FUNC_MOVE, .x = x, .y = y, .width = w, .height = h,
		.gravity_edges = WLR_EDGE_RIGHT };
	moved_x = moved_y = -1;
	view_move_to_edge(&v, dir);
	CHECK_INT(v.gravity_edges, 0);
}

TEST(edge_each_way)
{
	one_output(0);
	out.usable = out.open = (struct wlr_box){ 0, 30, 1280, 690 }; /* a panel on top */
	/* A frame of 312x237. */
	to_edge(500, 300, 300, 200, SNAP_DIR_LEFT);
	CHECK_INT(moved_x, 0);
	CHECK_INT(moved_y, 300);
	to_edge(500, 300, 300, 200, SNAP_DIR_RIGHT);
	CHECK_INT(moved_x, 1280 - 312);
	to_edge(500, 300, 300, 200, SNAP_DIR_UP);
	CHECK_INT(moved_y, 30);
	to_edge(500, 300, 300, 200, SNAP_DIR_DOWN);
	CHECK_INT(moved_y, 720 - 237);
}

TEST(edge_larger_than_the_area)
{
	one_output(0);
	to_edge(-100, -50, 2000, 1000, SNAP_DIR_RIGHT);
	CHECK_INT(moved_x, 0); /* its left edge stays in */
	to_edge(-100, -50, 2000, 1000, SNAP_DIR_DOWN);
	CHECK_INT(moved_y, 0);
}

TEST(edge_out_of_the_dock)
{
	one_output(0);
	out.open = (struct wlr_box){ 0, 0, 1280, 720 };
	out.usable = (struct wlr_box){ 0, 0, 1280, 656 };
	out.dock = (struct wlr_box){ 900, 656, 380, 64 };
	/* Down on the left: beside the dock, as far as the output's edge. */
	to_edge(100, 100, 300, 200, SNAP_DIR_DOWN);
	CHECK_INT(moved_y, 720 - 237);
	/* Down on the right it would cover the dock: to the usable area's edge. */
	to_edge(1000, 100, 200, 200, SNAP_DIR_DOWN);
	CHECK_INT(moved_y, 656 - 237);
	/* A window already over the strip, pushed right, moves up out of it as well. */
	to_edge(700, 600, 200, 100, SNAP_DIR_RIGHT);
	CHECK_INT(moved_x, 1280 - 212);
	CHECK_INT(moved_y, 656 - 137);
}

TEST(edge_what_the_window_may)
{
	one_output(0);
	struct view v = { .funcs = FUNC_MOVE, .maximized = true, .x = 5 };
	moved_x = -1;
	view_move_to_edge(&v, SNAP_DIR_LEFT);
	CHECK_INT(moved_x, -1);
	v = (struct view){ .funcs = FUNC_MOVE, .snap = SNAP_LEFT, .x = 5 };
	view_move_to_edge(&v, SNAP_DIR_LEFT);
	CHECK_INT(moved_x, -1);
	v = (struct view){ .funcs = 0, .x = 5 };
	view_move_to_edge(&v, SNAP_DIR_LEFT);
	CHECK_INT(moved_x, -1);
}

int main(void)
{
	RUN(rect_positions);
	RUN(rect_gaps_for_every_size);
	RUN(names);
	RUN(zones_at_the_edges);
	RUN(zones_in_the_corners);
	RUN(zones_on_another_output);
	RUN(zones_what_the_window_may);
	RUN(area_beside_a_dock);
	RUN(step_table);
	RUN(step_down_from_floating_minimizes);
	RUN(step_what_the_window_may);
	RUN(edge_each_way);
	RUN(edge_larger_than_the_area);
	RUN(edge_out_of_the_dock);
	RUN(edge_what_the_window_may);
	return unit_done();
}
