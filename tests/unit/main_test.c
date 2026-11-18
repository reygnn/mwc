/*
 * main.c: what mwmrc says about an output, outputMode, outputScale, outputTransform
 * and outputPosition, read into a wlr_output_state or the layout. The output is a
 * wlr_output on the stack with a mode list or none, as headless has; mwmrc is a table.
 */
#define main mwc_main
#include "../../src/main.c"
#undef main
#include "unit.h"

/* ---- stubs ------------------------------------------------------------------ */

static const char *rc_key, *rc_value; /* the one resource mwmrc has */

const char *config_get_for(const char *name, const char *key)
{
	return rc_key && !strcmp(key, rc_key) ? rc_value : NULL;
}

/* In place of wlroots' own: the program's definition comes first, so output_place()
 * calls these. Where it put the output, or -1 for "the next free place". */
static long placed_x, placed_y;

struct wlr_output_layout_output *wlr_output_layout_add(struct wlr_output_layout *layout,
	struct wlr_output *output, int lx, int ly)
{
	placed_x = lx;
	placed_y = ly;
	return NULL;
}

struct wlr_output_layout_output *wlr_output_layout_add_auto(
	struct wlr_output_layout *layout, struct wlr_output *output)
{
	placed_x = placed_y = -1;
	return NULL;
}

/* ---- helpers ------------------------------------------------------------------ */

static struct wlr_output output;
static struct wlr_output_mode modes[3];

static void headless(void)
{
	output = (struct wlr_output){ .name = "HEADLESS-1" };
	wl_list_init(&output.modes);
}

/* 1920x1080 at 60 and 144 Hz, 1280x720 at 59.94. */
static void monitor(void)
{
	headless();
	modes[0] = (struct wlr_output_mode){ .width = 1920, .height = 1080, .refresh = 60000 };
	modes[1] = (struct wlr_output_mode){ .width = 1920, .height = 1080, .refresh = 144000 };
	modes[2] = (struct wlr_output_mode){ .width = 1280, .height = 720, .refresh = 59940 };
	for (int i = 0; i < 3; i++)
		wl_list_insert(output.modes.prev, &modes[i].link);
}

static void rc(const char *key, const char *value)
{
	rc_key = key;
	rc_value = value;
}

/* configure_mode() on the current output; the state is left for the caller to read. */
static bool mode(const char *value, struct wlr_output_state *state)
{
	rc("outputMode", value);
	wlr_output_state_init(state);
	bool ok = configure_mode(&output, state);
	/* Taken or not, the state says the same. */
	CHECK(ok == !!(state->committed & WLR_OUTPUT_STATE_MODE));
	return ok;
}

static bool custom_is(const struct wlr_output_state *s, int w, int h, int refresh)
{
	return s->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM && s->custom_mode.width == w &&
		s->custom_mode.height == h && s->custom_mode.refresh == refresh;
}

/* ---- outputMode ------------------------------------------------------------------ */

TEST(mode_none)
{
	struct wlr_output_state s;
	headless();
	CHECK(!mode(NULL, &s));
	wlr_output_state_finish(&s);
}

TEST(mode_any_size_without_a_mode_list)
{
	struct wlr_output_state s;
	headless();
	CHECK(mode("1920x1080", &s) && custom_is(&s, 1920, 1080, 0));
	CHECK(mode("3840x2160@30", &s) && custom_is(&s, 3840, 2160, 30000));
	CHECK(mode("800x600@59.94", &s) && custom_is(&s, 800, 600, 59940));
	CHECK(mode("1x1", &s) && custom_is(&s, 1, 1, 0));
	wlr_output_state_finish(&s);
}

TEST(mode_from_the_list)
{
	struct wlr_output_state s;
	monitor();
	/* The highest refresh rate if none is given. */
	CHECK(mode("1920x1080", &s) && s.mode == &modes[1]);
	CHECK(mode("1920x1080@60", &s) && s.mode == &modes[0]);
	/* Within half a Hz: 59.94 is what most people mean by 60. */
	CHECK(mode("1280x720@60", &s) && s.mode == &modes[2]);
	CHECK(mode("1920x1080@143.6", &s) && s.mode == &modes[1]);
	CHECK(!mode("1920x1080@120", &s));
	CHECK(!mode("1024x768", &s));
	wlr_output_state_finish(&s);
}

TEST(mode_malformed)
{
	static const char *bad[] = {
		"", "x", "1920", "1920x", "x1080", "1920 x 1080", "1920*1080",
		"0x1080", "1920x0", "-1920x1080", "1920x-1080",
		"1920x1080@", "1920x1080@0", "1920x1080@-60", "1920x1080@nan",
		"1920x1080foo", "1920x1080@60Hz", "1920x1080@60@60",
	};
	struct wlr_output_state s;
	headless();
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		if (mode(bad[i], &s))
			fprintf(stderr, "  took \"%s\"\n", bad[i]);
		CHECK(!(s.committed & WLR_OUTPUT_STATE_MODE));
	}
	wlr_output_state_finish(&s);
}

/* Review 2026-11-18 #19: past the range of int sscanf's %d is undefined (glibc made
 * 5x1080 of the first); 100000x100000 is a 40 GB buffer; 1e10 Hz overflows the
 * conversion to mHz. */
TEST(mode_out_of_range)
{
	static const char *bad[] = {
		"4294967301x1080", "2147483648x1", "1x2147483648", "99999999999999999999x1",
		"100000x100000", "32768x1080", "1920x32768",
		"1920x1080@1e10", "1920x1080@1001", "1920x1080@inf",
	};
	struct wlr_output_state s;
	headless();
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		if (mode(bad[i], &s))
			fprintf(stderr, "  took \"%s\"\n", bad[i]);
		CHECK(!(s.committed & WLR_OUTPUT_STATE_MODE));
	}
	CHECK(mode("32767x32767@1000", &s) && custom_is(&s, 32767, 32767, 1000000));
	wlr_output_state_finish(&s);
}

/* ---- outputScale ------------------------------------------------------------------ */

static float scale(const char *value)
{
	struct wlr_output_state s;
	rc("outputScale", value);
	wlr_output_state_init(&s);
	configure_scale(&output, &s);
	CHECK(s.committed & WLR_OUTPUT_STATE_SCALE);
	float got = s.scale;
	wlr_output_state_finish(&s);
	return got;
}

TEST(scale_values)
{
	headless();
	CHECK(scale(NULL) == 1.0f);
	CHECK(scale("1.5") == 1.5f);
	CHECK(scale("2") == 2.0f);
	CHECK(scale("0.1") == 0.1f);
	CHECK(scale("10") == 10.0f);
}

TEST(scale_rejected_is_one)
{
	static const char *bad[] = {
		"", "0", "0.09", "10.01", "-1", "nan", "NaN", "inf", "1.5x", "x1.5", "1,5",
		"1e309",
	};
	headless();
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		float got = scale(bad[i]);
		if (got != 1.0f)
			fprintf(stderr, "  \"%s\" gave %g\n", bad[i], got);
		CHECK(got == 1.0f);
	}
}

/* ---- outputTransform ------------------------------------------------------------------ */

static enum wl_output_transform transform(const char *value)
{
	struct wlr_output_state s;
	rc("outputTransform", value);
	wlr_output_state_init(&s);
	configure_transform(&output, &s);
	CHECK(s.committed & WLR_OUTPUT_STATE_TRANSFORM);
	enum wl_output_transform got = s.transform;
	wlr_output_state_finish(&s);
	return got;
}

TEST(transform_names)
{
	headless();
	CHECK_INT(transform(NULL), WL_OUTPUT_TRANSFORM_NORMAL);
	CHECK_INT(transform("normal"), WL_OUTPUT_TRANSFORM_NORMAL);
	CHECK_INT(transform("90"), WL_OUTPUT_TRANSFORM_90);
	CHECK_INT(transform("180"), WL_OUTPUT_TRANSFORM_180);
	CHECK_INT(transform("270"), WL_OUTPUT_TRANSFORM_270);
	CHECK_INT(transform("flipped"), WL_OUTPUT_TRANSFORM_FLIPPED);
	CHECK_INT(transform("Flipped-90"), WL_OUTPUT_TRANSFORM_FLIPPED_90);
	CHECK_INT(transform("FLIPPED-180"), WL_OUTPUT_TRANSFORM_FLIPPED_180);
	CHECK_INT(transform("flipped-270"), WL_OUTPUT_TRANSFORM_FLIPPED_270);
	CHECK_INT(transform("45"), WL_OUTPUT_TRANSFORM_NORMAL);
	CHECK_INT(transform("90 "), WL_OUTPUT_TRANSFORM_NORMAL);
	CHECK_INT(transform(""), WL_OUTPUT_TRANSFORM_NORMAL);
}

/* ---- outputPosition ------------------------------------------------------------------ */

static void place(const char *value)
{
	struct server server = { 0 };
	rc("outputPosition", value);
	placed_x = placed_y = -2;
	output_place(&server, &output);
}

TEST(position_given)
{
	headless();
	place("100,200");
	CHECK_INT(placed_x, 100);
	CHECK_INT(placed_y, 200);
	place("-1280,0");
	CHECK_INT(placed_x, -1280);
	place("1000000,-1000000");
	CHECK_INT(placed_x, 1000000);
	CHECK_INT(placed_y, -1000000);
}

TEST(position_otherwise_the_next_place)
{
	static const char *bad[] = {
		NULL, "", "100", "100,", ",200", "100,200,300", "100;200", "100, 200x",
		"a,b", "1000001,0", "0,-1000001",
		"99999999999999999999,0", "4294967396,0",
	};
	headless();
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		place(bad[i]);
		if (placed_x != -1)
			fprintf(stderr, "  \"%s\" placed at %ld,%ld\n", bad[i] ? bad[i] : "(none)",
				placed_x, placed_y);
		CHECK_INT(placed_x, -1);
	}
}

int main(void)
{
	wlr_log_init(WLR_SILENT, NULL);
	RUN(mode_none);
	RUN(mode_any_size_without_a_mode_list);
	RUN(mode_from_the_list);
	RUN(mode_malformed);
	RUN(mode_out_of_range);
	RUN(scale_values);
	RUN(scale_rejected_is_one);
	RUN(transform_names);
	RUN(position_given);
	RUN(position_otherwise_the_next_place);
	return unit_done();
}
