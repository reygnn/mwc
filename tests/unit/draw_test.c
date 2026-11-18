/*
 * draw.c: colour mixing, WCAG contrast and Motif shades; the theme from built-in
 * defaults, mwmrc and the environment, fonts measured by Pango; bevels and grooves
 * drawn into a real cairo_buffer and read back through wlroots' data pointer access.
 * Linked with config.c, which the theme reads.
 */
#define _POSIX_C_SOURCE 200809L
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include "../../src/mwc.h"
#include "unit.h"
#include "mwmrc.h"

static struct mwc_color rgb(uint32_t v)
{
	return (struct mwc_color){ ((v >> 16) & 0xff) / 255.0f, ((v >> 8) & 0xff) / 255.0f,
		(v & 0xff) / 255.0f };
}

static bool near(float a, float b)
{
	return a > b - 0.0001f && a < b + 0.0001f;
}

static bool same(struct mwc_color a, struct mwc_color b)
{
	return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b);
}

/* ---- colours ------------------------------------------------------------------ */

TEST(mix)
{
	struct mwc_color a = rgb(0x000000), b = rgb(0xffffff), c = rgb(0x204080);
	CHECK(same(color_mix(a, b, 0), a));
	CHECK(same(color_mix(a, b, 1), b));
	CHECK(same(color_mix(a, b, 0.5f), (struct mwc_color){ 0.5f, 0.5f, 0.5f }));
	CHECK(same(color_mix(c, c, 0.3f), c));
	struct mwc_color m = color_mix(c, b, 0.25f);
	CHECK(near(m.r, c.r + (1 - c.r) * 0.25f) && near(m.b, c.b + (1 - c.b) * 0.25f));
}

static bool black_text(uint32_t bg)
{
	struct mwc_color t = color_contrast(rgb(bg));
	CHECK(same(t, rgb(0)) || same(t, rgb(0xffffff)));
	return same(t, rgb(0));
}

/* WCAG: black when (L + 0.05) / 0.05 > 1.05 / (L + 0.05), L at about 0.179. */
TEST(contrast)
{
	CHECK(black_text(0xffffff));
	CHECK(!black_text(0x000000));
	CHECK(black_text(0xaeb2c3)); /* the default frame */
	CHECK(black_text(0x7387ad)); /* a luma threshold gave it white */
	CHECK(!black_text(0x8a0303));
	CHECK(black_text(0xffff00));
	CHECK(!black_text(0x0000ff));
	/* The grey on either side of the threshold. */
	CHECK(!black_text(0x757575));
	CHECK(black_text(0x767676));
	/* Below 0.04045 sRGB is linear: its green of 0x0a counts as 0.003, not 0.04,
	 * which would tip it over to black. */
	CHECK(!black_text(0x0a0a0a));
	CHECK(!black_text(0xb90aff));
}

TEST(shades)
{
	struct mwc_color bg = rgb(0x7387ad), fg = rgb(0x123456);
	struct mwc_shade s = shade_from(bg, fg);
	CHECK(same(s.bg, bg) && same(s.fg, fg));
	CHECK(near(s.top.r, bg.r + (1 - bg.r) * 0.55f) && near(s.top.g, bg.g + (1 - bg.g) * 0.55f));
	CHECK(near(s.bottom.b, bg.b * 0.5f));
	struct mwc_shade w = shade_from(rgb(0xffffff), fg);
	CHECK(same(w.top, rgb(0xffffff)));
	struct mwc_shade k = shade_from(rgb(0), fg);
	CHECK(same(k.bottom, rgb(0)));
	CHECK(near(k.top.r, 0.55f));
}

/* ---- the theme ------------------------------------------------------------------ */

static void no_env(void)
{
	unsetenv("MWC_ACTIVE");
	unsetenv("MWC_INACTIVE");
	unsetenv("MWC_ROOT");
	unsetenv("MWC_FONT");
}

static void theme_from(const char *text)
{
	load(text);
	theme_init();
}

TEST(theme_defaults)
{
	no_env();
	theme_from("");
	CHECK(same(theme.inactive.bg, rgb(0xaeb2c3)));
	CHECK(same(theme.active.bg, rgb(0x7387ad)));
	CHECK(same(theme.root, rgb(0x5b7792)) && !theme.root_none);
	CHECK(same(theme.active.fg, color_contrast(rgb(0x7387ad))));
	CHECK(!theme.has_active_fg && !theme.has_inactive_fg);
	CHECK(same(theme.menu.bg, theme.inactive.bg) && same(theme.select, theme.active.bg));
	CHECK(!strcmp(theme.font, "Sans Bold 10"));
	CHECK_INT(theme.border, 6);
	CHECK_INT(theme.bevel, 2);
	CHECK(theme.title_height >= 22 && theme.title_height <= 128);
	CHECK(theme.menu_item_height >= 24 && theme.menu_item_height >= theme.title_height);
	CHECK_INT(log_lines(""), 0);
}

TEST(theme_from_mwmrc)
{
	no_env();
	theme_from("Mwm*background: #102030\n"
		"Mwm*activeBackground: gold\n"
		"Mwm*rootBackground: rgb:0/8/f\n"
		"Mwm*foreground: white\n"
		"Mwm*activeForeground: #010203\n");
	CHECK(same(theme.inactive.bg, rgb(0x102030)));
	CHECK(same(theme.active.bg, rgb(0xffd700)));
	CHECK(same(theme.root, (struct mwc_color){ 0, 8 / 15.0f, 1 }));
	CHECK(theme.has_inactive_fg && same(theme.inactive.fg, rgb(0xffffff)));
	CHECK(theme.has_active_fg && same(theme.active.fg, rgb(0x010203)));
	CHECK(same(theme.active.top, shade_from(rgb(0xffd700), rgb(0)).top));
}

/* Environment first, then mwmrc, then the default; each bad one is said and skipped. */
TEST(theme_environment_first)
{
	no_env();
	setenv("MWC_ACTIVE", "#b24d7a", 1);
	setenv("MWC_INACTIVE", "not a colour", 1);
	setenv("MWC_ROOT", "", 1); /* empty: as if unset */
	theme_from("Mwm*activeBackground: gold\n"
		"Mwm*background: #102030\n"
		"Mwm*rootBackground: nonsense\n");
	CHECK(same(theme.active.bg, rgb(0xb24d7a)));
	CHECK(same(theme.inactive.bg, rgb(0x102030)));
	CHECK(same(theme.root, rgb(0x5b7792)));
	CHECK_INT(log_lines("ignoring MWC_INACTIVE=not a colour"), 1);
	CHECK_INT(log_lines("ignoring Mwm*rootBackground: nonsense"), 1);
	no_env();
}

/* "none" leaves the desktop to a wallpaper tool; the colour stays for the icon. */
TEST(theme_root_none)
{
	no_env();
	theme_from("Mwm*rootBackground: None\n");
	CHECK(theme.root_none && same(theme.root, rgb(0x5b7792)));
	setenv("MWC_ROOT", "none", 1);
	theme_from("Mwm*rootBackground: #000000\n");
	CHECK(theme.root_none);
	setenv("MWC_ROOT", "#000000", 1);
	theme_from("Mwm*rootBackground: none\n");
	CHECK(!theme.root_none && same(theme.root, rgb(0)));
	/* Only the root may be none. */
	no_env();
	theme_from("Mwm*activeBackground: none\n");
	CHECK(same(theme.active.bg, rgb(0x7387ad)));
	CHECK_INT(log_lines("ignoring Mwm*activeBackground: none"), 1);
}

TEST(theme_fonts)
{
	no_env();
	theme_from("Mwm*fontList: Mono 12\n");
	CHECK(!strcmp(theme.font, "Mono 12"));
	/* mwm-x11's fontconfig patterns, and a name without a size, mean another font
	 * to Pango: said, and the default taken. */
	theme_from("Mwm*fontList: Sans:bold:size=10\n");
	CHECK(!strcmp(theme.font, "Sans Bold 10"));
	CHECK_INT(log_lines("ignoring font \"Sans:bold:size=10\" from Mwm*fontList"), 1);
	theme_from("Mwm*fontList: Sans:bold 10\n"); /* sized, but still a pattern */
	CHECK(!strcmp(theme.font, "Sans Bold 10"));
	CHECK_INT(log_lines("ignoring font"), 1);
	theme_from("Mwm*fontList: Sans-12\n");
	CHECK(!strcmp(theme.font, "Sans Bold 10"));
	CHECK_INT(log_lines("ignoring font"), 1);
	setenv("MWC_FONT", "Serif 11", 1);
	theme_from("Mwm*fontList: Mono 12\n");
	CHECK(!strcmp(theme.font, "Serif 11"));
	setenv("MWC_FONT", "Serif", 1); /* bad: mwmrc's next */
	theme_from("Mwm*fontList: Mono 12\n");
	CHECK(!strcmp(theme.font, "Mono 12"));
	no_env();
}

/* Bars grow with the font, not below 22 and 24, and stop at 128. */
TEST(theme_bar_heights)
{
	no_env();
	theme_from("Mwm*fontList: Sans 4\n");
	CHECK_INT(theme.title_height, 22);
	CHECK_INT(theme.menu_item_height, 24);
	theme_from("Mwm*fontList: Sans 20\n");
	int h20 = theme.title_height;
	CHECK(h20 > 22 && theme.menu_item_height == h20);
	theme_from("Mwm*fontList: Sans Bold 1000\n");
	CHECK_INT(theme.title_height, 128);
	CHECK_INT(theme.menu_item_height, 128);
	CHECK_INT(log_lines("is too large for a title bar"), 1);
}

TEST(text)
{
	int w = -1, h = -1, w2, h2;
	text_extent("Sans 10", "", &w, &h);
	CHECK_INT(w, 0);
	CHECK(h > 0);
	text_extent("Sans 10", "Xg", &w, &h);
	text_extent("Sans 10", "XgXgXgXg", &w2, &h2);
	CHECK(w > 0 && w2 > 3 * w && h2 == h);
	text_extent("Sans 20", "Xg", &w2, &h2);
	CHECK(h2 > h);
	/* The same description each time, a new one for another font. */
	const PangoFontDescription *d = font_description("Sans 10");
	CHECK(font_description("Sans 10") == d);
	CHECK(pango_font_description_get_size(font_description("Mono 14")) == 14 * PANGO_SCALE);
}

/* ---- bevels in a cairo_buffer --------------------------------------------------- */

static struct mwc_shade bw(void)
{
	return (struct mwc_shade){ .top = rgb(0xffffff), .bottom = rgb(0x000000),
		.bg = rgb(0x808080) };
}

/* Its pixel as 0xRRGGBB, or -1 where nothing was drawn. */
static long pixel(struct cairo_buffer *b, int x, int y)
{
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(&b->base, WLR_BUFFER_DATA_PTR_ACCESS_READ,
			&data, &format, &stride))
		return -2;
	uint32_t p = ((uint32_t *)((char *)data + y * stride))[x];
	wlr_buffer_end_data_ptr_access(&b->base);
	return (p >> 24) ? (long)(p & 0xffffff) : -1;
}

TEST(buffer)
{
	struct cairo_buffer *b = cairo_buffer_create(30, 20);
	CHECK(b && b->base.width == 30 && b->base.height == 20);
	void *data;
	uint32_t format;
	size_t stride;
	CHECK(wlr_buffer_begin_data_ptr_access(&b->base, WLR_BUFFER_DATA_PTR_ACCESS_READ,
		&data, &format, &stride));
	wlr_buffer_end_data_ptr_access(&b->base);
	CHECK(format == DRM_FORMAT_ARGB8888 && stride >= 30 * 4 && data);
	/* The scene only reads it; drawing goes through cairo. */
	CHECK(!wlr_buffer_begin_data_ptr_access(&b->base, WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
		&data, &format, &stride));
	CHECK_INT(pixel(b, 0, 0), -1); /* transparent to begin with */
	cairo_buffer_drop(b);
	b = cairo_buffer_create(0, -5); /* at least 1x1 */
	CHECK(b && b->base.width == 1 && b->base.height == 1);
	cairo_buffer_drop(b);
	/* More than cairo can do, as a client may ask: said, and none. */
	logged[0] = '\0';
	CHECK(cairo_buffer_create(40000, 10) == NULL);
	CHECK_INT(log_lines("cannot create a 40000x10 buffer"), 1);
}

TEST(bevel_out_and_in)
{
	struct mwc_shade s = bw();
	for (int in = 0; in < 2; in++) {
		long tl = in ? 0x000000 : 0xffffff, br = in ? 0xffffff : 0x000000;
		struct cairo_buffer *b = cairo_buffer_create(20, 20);
		draw_bevel(b->cr, 0, 0, 20, 20, 2, &s, in);
		CHECK_INT(pixel(b, 0, 10), tl);
		CHECK_INT(pixel(b, 1, 10), tl);
		CHECK_INT(pixel(b, 2, 10), -1); /* two wide, the inside left alone */
		CHECK_INT(pixel(b, 10, 0), tl);
		CHECK_INT(pixel(b, 10, 1), tl);
		CHECK_INT(pixel(b, 10, 2), -1);
		CHECK_INT(pixel(b, 19, 10), br);
		CHECK_INT(pixel(b, 18, 10), br);
		CHECK_INT(pixel(b, 17, 10), -1);
		CHECK_INT(pixel(b, 10, 19), br);
		CHECK_INT(pixel(b, 10, 18), br);
		CHECK_INT(pixel(b, 10, 10), -1);
		/* Motif's diagonal joins at the top right and bottom left. */
		CHECK_INT(pixel(b, 18, 0), tl);
		CHECK_INT(pixel(b, 19, 1), br);
		CHECK_INT(pixel(b, 0, 18), tl);
		CHECK_INT(pixel(b, 1, 19), br);
		cairo_buffer_drop(b);
	}
}

TEST(grooves)
{
	struct mwc_shade s = bw();
	struct cairo_buffer *b = cairo_buffer_create(20, 20);
	draw_groove_h(b->cr, 2, 5, 10, &s);
	CHECK_INT(pixel(b, 2, 5), 0x000000);
	CHECK_INT(pixel(b, 11, 6), 0xffffff);
	CHECK_INT(pixel(b, 12, 5), -1);
	CHECK_INT(pixel(b, 5, 7), -1);
	draw_groove_v(b->cr, 15, 8, 6, &s);
	CHECK_INT(pixel(b, 15, 8), 0x000000);
	CHECK_INT(pixel(b, 16, 13), 0xffffff);
	CHECK_INT(pixel(b, 17, 10), -1);
	CHECK_INT(pixel(b, 15, 14), -1);
	cairo_buffer_drop(b);
}

int main(void)
{
	mwmrc_init();
	RUN(mix);
	RUN(contrast);
	RUN(shades);
	RUN(theme_defaults);
	RUN(theme_from_mwmrc);
	RUN(theme_environment_first);
	RUN(theme_root_none);
	RUN(theme_fonts);
	RUN(theme_bar_heights);
	RUN(text);
	RUN(buffer);
	RUN(bevel_out_and_in);
	RUN(grooves);
	mwmrc_done();
	return unit_done();
}
