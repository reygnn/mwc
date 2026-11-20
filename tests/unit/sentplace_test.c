/*
 * sentplace.c: the place an X11 frame takes for a size the window commits, whichever
 * configure that commit answers.
 */
#include "../../src/mwc.h"
#include "unit.h"

static bool at(const struct sent_places *s, int w, int h, int x, int y)
{
	int fx = -1, fy = -1;
	return sent_places_find(s, w, h, &fx, &fy) && fx == x && fy == y;
}

/* A left-edge drag: each size has its own x, the right edge the same for all. */
TEST(each_size_its_place)
{
	struct sent_places s = { 0 };
	sent_places_add(&s, 200, 50, 300, 200);
	sent_places_add(&s, 160, 50, 340, 200);
	sent_places_add(&s, 120, 50, 380, 200);
	CHECK(at(&s, 300, 200, 200, 50));
	CHECK(at(&s, 340, 200, 160, 50));
	CHECK(at(&s, 380, 200, 120, 50));
	int x = 7, y = 7;
	CHECK(!sent_places_find(&s, 320, 200, &x, &y));
	CHECK_INT(x, 7);
}

/* Back to a size sent before: X was told the newer place, and an old frame at that
 * size is drawn for it too. */
TEST(newest_place_for_a_size)
{
	struct sent_places s = { 0 };
	sent_places_add(&s, 200, 50, 300, 200);
	sent_places_add(&s, 160, 50, 340, 200);
	sent_places_add(&s, 260, 50, 300, 200);
	CHECK_INT(s.n, 2);
	CHECK(at(&s, 300, 200, 260, 50));
	CHECK(at(&s, 340, 200, 160, 50));
}

/* A fast drag outruns the window: the oldest sizes go first. */
TEST(oldest_dropped)
{
	struct sent_places s = { 0 };
	for (int i = 0; i < SENT_PLACES + 3; i++)
		sent_places_add(&s, -i, 0, 100 + i, 100);
	CHECK_INT(s.n, SENT_PLACES);
	int x, y;
	CHECK(!sent_places_find(&s, 100, 100, &x, &y));
	CHECK(!sent_places_find(&s, 102, 100, &x, &y));
	CHECK(at(&s, 103, 100, -3, 0));
	CHECK(at(&s, 100 + SENT_PLACES + 2, 100, -(SENT_PLACES + 2), 0));
	/* A size sent again takes its own entry's room, not the oldest's. */
	sent_places_add(&s, 50, 0, 105, 100);
	CHECK_INT(s.n, SENT_PLACES);
	CHECK(at(&s, 105, 100, 50, 0));
	CHECK(at(&s, 103, 100, -3, 0));
}

/* Moved while sizes are on their way: each frame moves with it. */
TEST(moved_with_the_window)
{
	struct sent_places s = { 0 };
	sent_places_add(&s, 200, 50, 300, 200);
	sent_places_add(&s, 160, 40, 340, 210);
	sent_places_move(&s, 15, -5);
	CHECK(at(&s, 300, 200, 215, 45));
	CHECK(at(&s, 340, 210, 175, 35));
}

TEST(cleared)
{
	struct sent_places s = { 0 };
	sent_places_add(&s, 200, 50, 300, 200);
	sent_places_clear(&s);
	int x, y;
	CHECK(!sent_places_find(&s, 300, 200, &x, &y));
	sent_places_add(&s, 10, 20, 300, 200);
	CHECK(at(&s, 300, 200, 10, 20));
}

int main(void)
{
	RUN(each_size_its_place);
	RUN(newest_place_for_a_size);
	RUN(oldest_dropped);
	RUN(moved_with_the_window);
	RUN(cleared);
	return unit_done();
}
