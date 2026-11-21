/*
 * draw.c: colour mixing, Motif title text and shades; the theme from built-in
 * defaults, mwmrc and the environment, fonts measured by Pango; bevels and grooves
 * drawn into a real cairo_buffer and read back through wlroots' data pointer access.
 * Linked with config.c, which the theme reads.
 */
#define _POSIX_C_SOURCE 200809L
#include <drm_fourcc.h>
#include <math.h>
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

/* Motif: black above 70% brightness, three quarters the mean, a quarter the luminosity. */
TEST(contrast)
{
	CHECK(black_text(0xffffff));
	CHECK(!black_text(0x000000));
	CHECK(black_text(0xaeb2c3)); /* the default frame, at 71% */
	CHECK(!black_text(0x7387ad)); /* the default focused frame, at 55% */
	CHECK(!black_text(0x8a0303));
	CHECK(black_text(0xffff00));
	CHECK(!black_text(0xffd700)); /* gold: mwm gives it white text too */
	CHECK(!black_text(0x0000ff));
	CHECK(!black_text(0x5f9ea0)); /* CadetBlue, mwm's own focused frame */
	/* The grey on either side of the threshold. */
	CHECK(!black_text(0xb2b2b2));
	CHECK(black_text(0xb3b3b3));
}

/* An 8-bit channel as the screen shows it: the 16-bit value cut to its high byte, as
 * the X server does for mwm and cairo does for mwc. */
static int byte(float c)
{
	return (int)lroundf(c * 65535) >> 8;
}

static bool shade_is(struct mwc_color c, uint32_t v)
{
	return byte(c.r) == (int)(v >> 16) && byte(c.g) == (int)((v >> 8) & 0xff) &&
		byte(c.b) == (int)(v & 0xff);
}

/* The values Motif 2.3.8's CalculateColorsRGB() gives, as mwm shows them. */
TEST(shades)
{
	struct mwc_color bg = rgb(0x7387ad), fg = rgb(0x123456);
	struct mwc_shade s = shade_from(bg, fg);
	CHECK(same(s.bg, bg) && same(s.fg, fg));
	/* Medium: the top towards white, the bottom towards black. */
	CHECK(shade_is(s.top, 0xc0c9da) && shade_is(s.bottom, 0x394356));
	s = shade_from(rgb(0xaeb2c3), fg);
	CHECK(shade_is(s.top, 0xdddee6) && shade_is(s.bottom, 0x5e6069));
	s = shade_from(rgb(0x2e5e3e), fg);
	CHECK(shade_is(s.top, 0x9bb2a2) && shade_is(s.bottom, 0x142a1c));
	/* Light, above 93%: both towards black, the top less. */
	s = shade_from(rgb(0xf0f0f0), fg);
	CHECK(shade_is(s.top, 0xc0c0c0) && shade_is(s.bottom, 0x909090));
	struct mwc_shade w = shade_from(rgb(0xffffff), fg);
	CHECK(shade_is(w.top, 0xcccccc) && shade_is(w.bottom, 0x999999));
	/* Dark, below 20%: both towards white, the bottom less. */
	s = shade_from(rgb(0x202020), fg);
	CHECK(shade_is(s.top, 0x909090) && shade_is(s.bottom, 0x636363));
	struct mwc_shade k = shade_from(rgb(0), fg);
	CHECK(shade_is(k.top, 0x7f7f7f) && shade_is(k.bottom, 0x4c4c4c));
	/* The edges of the medium range. */
	CHECK(byte(shade_from(rgb(0x323232), fg).bottom.r) > 0x32);
	CHECK(byte(shade_from(rgb(0x333333), fg).bottom.r) < 0x33);
	CHECK(byte(shade_from(rgb(0xededed), fg).top.r) > 0xed);
	CHECK(byte(shade_from(rgb(0xeeeeee), fg).top.r) < 0xee);
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
	CHECK_INT(theme.border, 7);
	CHECK_INT(theme.thin_border, 5);
	CHECK_INT(theme.bevel, 2);
	int w, h;
	text_extent("Sans Bold 10", "Xg", &w, &h);
	CHECK_INT(theme.title_height, h + 4);
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

/* Review 2026-11-18 #13: a title text colour that isn't one is said, as the others are. */
TEST(theme_bad_foreground)
{
	no_env();
	theme_from("Mwm*foreground: nonsense\nMwm*activeForeground: #12\n");
	CHECK(!theme.has_inactive_fg && !theme.has_active_fg);
	CHECK_INT(log_lines("ignoring Mwm*foreground: nonsense"), 1);
	CHECK_INT(log_lines("ignoring Mwm*activeForeground: #12"), 1);
	theme_from("Mwm*foreground: white\n");
	CHECK_INT(log_lines(""), 0);
}

/* docs/mwmrc.irix-schemes: each scheme, commented out or not, is read without a word and
 * gives its two colours; where the file says what Motif gives for the title text, mwc
 * gives that, and the override it offers is read and differs from it. */
struct scheme {
	char name[32];
	char lines[256]; /* its Mwm lines, uncommented */
};

static uint32_t hex_after(const char *s, const char *key)
{
	const char *p = strstr(s, key);
	return p ? (uint32_t)strtoul(strchr(p, '#') + 1, NULL, 16) : 0x1000000;
}

static void irix_scheme(const struct scheme *sc)
{
	theme_from(sc->lines);
	CHECK_INT(log_lines(""), 0);
	CHECK(same(theme.active.bg, rgb(hex_after(sc->lines, "activeBackground:"))));
	CHECK(same(theme.inactive.bg, rgb(hex_after(sc->lines, "*background:"))));
}

TEST(irix_schemes)
{
	FILE *f = fopen("docs/mwmrc.irix-schemes", "r");
	CHECK(f != NULL);
	if (!f)
		return;
	static struct scheme schemes[40];
	int n = 0, claims = 0, overrides = 0;
	bool optional = false;
	const struct scheme *cur = NULL;
	char line[256], text[512];
	while (fgets(line, sizeof(line), f)) {
		const char *l = line + (line[0] == '!' ? strspn(line + 1, " ") + 1 : 0);
		if (!strncmp(line, "! ===", 5)) {
			optional = true;
		} else if (!strncmp(line, "! --- ", 6)) {
			char name[32];
			sscanf(line + 6, "%31s", name);
			cur = NULL;
			for (int i = 0; i < n; i++)
				if (!strcmp(schemes[i].name, name))
					cur = &schemes[i];
			if (!optional && n < 40) {
				CHECK(cur == NULL); /* each once */
				strcpy(schemes[n].name, name);
				schemes[n].lines[0] = '\0';
				cur = &schemes[n++];
			}
			CHECK(cur != NULL);
		} else if (!optional && cur && !strncmp(l, "Mwm*", 4)) {
			struct scheme *s = &schemes[cur - schemes];
			strncat(s->lines, l, sizeof(s->lines) - strlen(s->lines) - 1);
		} else if (optional && cur && strstr(line, "Motif gives")) {
			/* "!   activeForeground: Motif gives #rrggbb" */
			bool active = strstr(line, "activeForeground:") != NULL;
			theme_from(cur->lines);
			struct mwc_color fg = active ? theme.active.fg : theme.inactive.fg;
			CHECK(same(fg, rgb(hex_after(line, "gives"))));
			claims++;
		} else if (optional && cur && !strncmp(l, "Mwm*", 4)) {
			bool active = strstr(l, "activeForeground") != NULL;
			theme_from(cur->lines);
			struct mwc_color motif = active ? theme.active.fg : theme.inactive.fg;
			snprintf(text, sizeof(text), "%s%s", cur->lines, l);
			theme_from(text);
			CHECK_INT(log_lines(""), 0);
			CHECK(active ? theme.has_active_fg : theme.has_inactive_fg);
			struct mwc_color fg = active ? theme.active.fg : theme.inactive.fg;
			CHECK(same(fg, rgb(hex_after(l, ":"))) && !same(fg, motif));
			overrides++;
		}
	}
	fclose(f);
	CHECK_INT(n, 30);
	for (int i = 0; i < n; i++)
		irix_scheme(&schemes[i]);
	CHECK_INT(claims, 11);
	CHECK_INT(overrides, 11);
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

/* Bars grow with the font: title bars as mwm's, the font and 4, menu items the font and 8
 * but not below 24. Both stop at 128. */
TEST(theme_bar_heights)
{
	int w, h;
	no_env();
	theme_from("Mwm*fontList: Sans 4\n");
	text_extent("Sans 4", "Xg", &w, &h);
	CHECK_INT(theme.title_height, h + 4);
	CHECK_INT(theme.menu_item_height, 24);
	theme_from("Mwm*fontList: Sans 20\n");
	text_extent("Sans 20", "Xg", &w, &h);
	CHECK_INT(theme.title_height, h + 4);
	CHECK_INT(theme.menu_item_height, h + 8);
	theme_from("Mwm*fontList: Sans Bold 1000\n");
	CHECK_INT(theme.title_height, 128);
	CHECK_INT(theme.menu_item_height, 128);
	CHECK_INT(log_lines("is too large for a title bar"), 1);
}

/* Mwm*resizeBorderWidth and frameBorderWidth, 3 to 64 pixels; anything else keeps the
 * default and is logged. */
TEST(theme_border_widths)
{
	no_env();
	theme_from("Mwm*resizeBorderWidth: 10\nMwm*frameBorderWidth: 3\n");
	CHECK_INT(theme.border, 10);
	CHECK_INT(theme.thin_border, 3);
	theme_from("Mwm*resizeBorderWidth: 64\nMwm*frameBorderWidth: 64\n");
	CHECK_INT(theme.border, 64);
	CHECK_INT(theme.thin_border, 64);
	CHECK_INT(log_lines("expected 3 to 64"), 0);
	theme_from("Mwm*resizeBorderWidth: 2\nMwm*frameBorderWidth: 65\n");
	CHECK_INT(theme.border, 7);
	CHECK_INT(theme.thin_border, 5);
	CHECK_INT(log_lines("expected 3 to 64"), 2);
	theme_from("Mwm*resizeBorderWidth: 8px\nMwm*frameBorderWidth: wide\n");
	CHECK_INT(theme.border, 7);
	CHECK_INT(theme.thin_border, 5);
	CHECK_INT(log_lines("expected 3 to 64"), 2);
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
	RUN(theme_bad_foreground);
	RUN(irix_schemes);
	RUN(theme_fonts);
	RUN(theme_bar_heights);
	RUN(theme_border_widths);
	RUN(text);
	RUN(buffer);
	RUN(bevel_out_and_in);
	RUN(grooves);
	mwmrc_done();
	return unit_done();
}
