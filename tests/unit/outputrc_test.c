/*
 * outputrc.c: outputMode, outputScale, outputTransform and outputPosition as mwmrc
 * writes them, well formed, malformed and out of range, the choice from a real
 * list of wlr_output_mode, and when input wakes a list of real outputs.
 */
#include <stdint.h>
#include <string.h>
#include <wlr/types/wlr_output.h>
#include "../../src/mwc.h"
#include "unit.h"

/* ---- outputMode ------------------------------------------------------------------ */

static bool mode_is(const char *value, int w, int h, double hz)
{
	int gw = -1, gh = -1;
	double ghz = -1;
	return outputrc_mode(value, &gw, &gh, &ghz) && gw == w && gh == h && ghz == hz;
}

TEST(mode_values)
{
	CHECK(mode_is("1920x1080", 1920, 1080, 0));
	CHECK(mode_is("3840x2160@30", 3840, 2160, 30));
	CHECK(mode_is("800x600@59.94", 800, 600, 59.94));
	CHECK(mode_is("1x1", 1, 1, 0));
	CHECK(mode_is("32767x32767@1000", 32767, 32767, 1000));
}

static void refused(const char *const *bad, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int w = -1, h = -1;
		double hz = -1;
		bool took = outputrc_mode(bad[i], &w, &h, &hz);
		if (took)
			fprintf(stderr, "  took \"%s\"\n", bad[i]);
		CHECK(!took);
		CHECK(w == -1 && h == -1 && hz == -1); /* left alone */
	}
}

TEST(mode_malformed)
{
	static const char *const bad[] = {
		"", "x", "1920", "1920x", "x1080", "1920 x 1080", "1920*1080",
		"0x1080", "1920x0", "-1920x1080", "1920x-1080",
		"1920x1080@", "1920x1080@0", "1920x1080@-60", "1920x1080@nan",
		"1920x1080foo", "1920x1080@60Hz", "1920x1080@60@60",
	};
	refused(bad, sizeof(bad) / sizeof(*bad));
}

/* Review 2026-11-18 #19: past the range of int sscanf's %d is undefined (glibc made
 * 5x1080 of the first); 100000x100000 is a 40 GB buffer; 1e10 Hz overflows the
 * conversion to mHz; NaN passed `hz <= 0`. */
TEST(mode_out_of_range)
{
	static const char *const bad[] = {
		"4294967301x1080", "2147483648x1", "1x2147483648", "99999999999999999999x1",
		"100000x100000", "32768x1080", "1920x32768",
		"1920x1080@1e10", "1920x1080@1000.01", "1920x1080@inf", "1920x1080@NaN",
	};
	refused(bad, sizeof(bad) / sizeof(*bad));
}

/* 1920x1080 at 60 and 144 Hz, 1280x720 at 59.94, as a monitor lists them. */
static struct wl_list modes;
static struct wlr_output_mode mode[3];

static void monitor(void)
{
	wl_list_init(&modes);
	mode[0] = (struct wlr_output_mode){ .width = 1920, .height = 1080, .refresh = 60000 };
	mode[1] = (struct wlr_output_mode){ .width = 1920, .height = 1080, .refresh = 144000 };
	mode[2] = (struct wlr_output_mode){ .width = 1280, .height = 720, .refresh = 59940 };
	for (int i = 0; i < 3; i++)
		wl_list_insert(modes.prev, &mode[i].link);
}

TEST(mode_from_the_list)
{
	monitor();
	/* The highest refresh rate if none is given. */
	CHECK(outputrc_pick_mode(&modes, 1920, 1080, 0) == &mode[1]);
	CHECK(outputrc_pick_mode(&modes, 1920, 1080, 60) == &mode[0]);
	/* Within half a Hz: 59.94 is what most people mean by 60. */
	CHECK(outputrc_pick_mode(&modes, 1280, 720, 60) == &mode[2]);
	CHECK(outputrc_pick_mode(&modes, 1280, 720, 59.44) == &mode[2]);
	CHECK(outputrc_pick_mode(&modes, 1280, 720, 59.43) == NULL);
	CHECK(outputrc_pick_mode(&modes, 1920, 1080, 143.6) == &mode[1]);
	CHECK(outputrc_pick_mode(&modes, 1920, 1080, 120) == NULL);
	CHECK(outputrc_pick_mode(&modes, 1024, 768, 0) == NULL);
	CHECK(outputrc_pick_mode(&modes, 1080, 1920, 0) == NULL); /* not turned */
	wl_list_init(&modes);
	CHECK(outputrc_pick_mode(&modes, 1920, 1080, 0) == NULL);
}

/* ---- outputScale ------------------------------------------------------------------ */

TEST(scale_values)
{
	double s = 0;
	CHECK(outputrc_scale("1.5", &s) && s == 1.5);
	CHECK(outputrc_scale("2", &s) && s == 2);
	CHECK(outputrc_scale("0.1", &s) && s == 0.1);
	CHECK(outputrc_scale("10", &s) && s == 10);
}

TEST(scale_refused)
{
	static const char *const bad[] = {
		"", "0", "0.09", "10.01", "-1", "nan", "NaN", "inf", "1.5x", "x1.5", "1,5",
		"1e309", " ",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		double s = -1;
		bool took = outputrc_scale(bad[i], &s);
		if (took)
			fprintf(stderr, "  took \"%s\" as %g\n", bad[i], s);
		CHECK(!took && s == -1);
	}
}

/* ---- outputTransform ------------------------------------------------------------------ */

static int transform(const char *value)
{
	enum wl_output_transform t = 99;
	return outputrc_transform(value, &t) ? (int)t : -1;
}

TEST(transform_names)
{
	CHECK_INT(transform("normal"), WL_OUTPUT_TRANSFORM_NORMAL);
	CHECK_INT(transform("90"), WL_OUTPUT_TRANSFORM_90);
	CHECK_INT(transform("180"), WL_OUTPUT_TRANSFORM_180);
	CHECK_INT(transform("270"), WL_OUTPUT_TRANSFORM_270);
	CHECK_INT(transform("flipped"), WL_OUTPUT_TRANSFORM_FLIPPED);
	CHECK_INT(transform("Flipped-90"), WL_OUTPUT_TRANSFORM_FLIPPED_90);
	CHECK_INT(transform("FLIPPED-180"), WL_OUTPUT_TRANSFORM_FLIPPED_180);
	CHECK_INT(transform("flipped-270"), WL_OUTPUT_TRANSFORM_FLIPPED_270);
	CHECK_INT(transform("45"), -1);
	CHECK_INT(transform("90 "), -1);
	CHECK_INT(transform("flipped90"), -1);
	CHECK_INT(transform(""), -1);
}

/* ---- outputPosition ------------------------------------------------------------------ */

static bool position_is(const char *value, int x, int y)
{
	int gx = 7, gy = 7;
	return outputrc_position(value, &gx, &gy) && gx == x && gy == y;
}

TEST(position_values)
{
	CHECK(position_is("100,200", 100, 200));
	CHECK(position_is("-1280,0", -1280, 0));
	CHECK(position_is("1000000,-1000000", 1000000, -1000000));
}

TEST(position_refused)
{
	static const char *const bad[] = {
		"", "100", "100,", ",200", "100,200,300", "100;200", "100,200x",
		"a,b", "1000001,0", "0,-1000001",
		"99999999999999999999,0", "4294967396,0",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		int x = 7, y = 7;
		bool took = outputrc_position(bad[i], &x, &y);
		if (took)
			fprintf(stderr, "  took \"%s\" as %d,%d\n", bad[i], x, y);
		CHECK(!took && x == 7 && y == 7);
	}
}

/* ---- waking ---------------------------------------------------------------------- */

struct outputs {
	struct wl_list list;
	struct output o[3];
	struct wlr_output w[3];
};

/* n outputs; each on, dark (off through wlr-output-power) or off through output
 * management, as "o", "d" and "m" in states. */
static struct wl_list *outputs(struct outputs *s, const char *states)
{
	memset(s, 0, sizeof(*s));
	wl_list_init(&s->list);
	for (int i = 0; states[i]; i++) {
		s->o[i].wlr_output = &s->w[i];
		s->w[i].enabled = states[i] == 'o';
		s->o[i].dark = states[i] == 'd';
		s->o[i].client_off = states[i] == 'm';
		wl_list_insert(s->list.prev, &s->o[i].link);
	}
	return &s->list;
}

/* An hour after the last went dark, past the second in which nothing wakes. */
static bool wakes(struct outputs *s, const char *states)
{
	return outputrc_wakes(outputs(s, states), 3600000, 0);
}

TEST(wake)
{
	struct outputs s;
	CHECK(wakes(&s, "d"));
	CHECK(wakes(&s, "dd"));
	/* One on: the other stays off, as the client set it. */
	CHECK(!wakes(&s, "do"));
	CHECK(!wakes(&s, "od"));
	CHECK(!wakes(&s, "o"));
	/* Off through output management only: only such a client turns it on. */
	CHECK(!wakes(&s, "m"));
	CHECK(wakes(&s, "md"));
	CHECK(wakes(&s, "dmd"));
	/* No output at all: nothing to wake. */
	CHECK(!wakes(&s, ""));
}

/* Not in the first second after the last went dark, also across the clock's wrap. */
TEST(wake_after_a_second)
{
	struct outputs s;
	CHECK(!outputrc_wakes(outputs(&s, "d"), 5000, 5000));
	CHECK(!outputrc_wakes(outputs(&s, "d"), 5999, 5000));
	CHECK(outputrc_wakes(outputs(&s, "d"), 6000, 5000));
	CHECK(!outputrc_wakes(outputs(&s, "d"), 0x100, 0xfffffff0u));
	CHECK(outputrc_wakes(outputs(&s, "d"), 0x400, 0xfffffff0u));
	CHECK(!outputrc_wakes(outputs(&s, "o"), 6000, 5000));
}

int main(void)
{
	RUN(mode_values);
	RUN(mode_malformed);
	RUN(mode_out_of_range);
	RUN(mode_from_the_list);
	RUN(scale_values);
	RUN(scale_refused);
	RUN(transform_names);
	RUN(position_values);
	RUN(position_refused);
	RUN(wake);
	RUN(wake_after_a_second);
	return unit_done();
}
