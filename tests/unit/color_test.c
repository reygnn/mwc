/*
 * color.c: Motif 2.3.8's title text and shadows on 0xrrggbb, as mwm shows them, the
 * built-in defaults. The values are what CalculateColorsRGB() gives, cut to 8 bits.
 */
#include <stdbool.h>
#include "../../src/color.h"
#include "unit.h"

static bool shades_are(uint32_t bg, uint32_t top, uint32_t bottom)
{
	struct color_shades s = color_shades(bg);
	if (s.top == top && s.bottom == bottom)
		return true;
	fprintf(stderr, "  %06x: %06x/%06x, expected %06x/%06x\n", (unsigned)bg,
		(unsigned)s.top, (unsigned)s.bottom, (unsigned)top, (unsigned)bottom);
	return false;
}

static bool black_text(uint32_t bg)
{
	uint32_t t = color_text(bg);
	CHECK(t == 0x000000 || t == 0xffffff);
	return t == 0x000000;
}

/* Black above 70% brightness: three quarters the mean, a quarter the luminosity. */
TEST(text)
{
	CHECK(black_text(0xffffff));
	CHECK(!black_text(0x000000));
	CHECK(black_text(0xaeb2c3)); /* at 71% */
	CHECK(black_text(0xc6c1aa)); /* at 74% */
	CHECK(!black_text(0x7387ad)); /* at 55% */
	CHECK(!black_text(0xaaaaaa)); /* at 67% */
	CHECK(!black_text(COLOR_ACTIVE));
	CHECK(black_text(COLOR_INACTIVE)); /* at 71%, so no rule of its own */
	CHECK(black_text(COLOR_BASIC));
	CHECK(!black_text(0x8a0303));
	CHECK(black_text(0xffff00));
	CHECK(!black_text(0xffd700)); /* gold: mwm gives it white text too */
	CHECK(!black_text(0x0000ff));
	CHECK(!black_text(0x5f9ea0)); /* CadetBlue, mwm's own focused frame */
	/* The grey on either side of the threshold. */
	CHECK(!black_text(0xb2b2b2));
	CHECK(black_text(0xb3b3b3));
	/* Exactly at 70%: not above it. */
	CHECK(!black_text(0x17fcff));
}

TEST(shades)
{
	/* Medium: the top towards white, the bottom towards black. */
	CHECK(shades_are(0x7387ad, 0xc0c9da, 0x394356));
	CHECK(shades_are(0xaeb2c3, 0xdddee6, 0x5e6069));
	CHECK(shades_are(0x2e5e3e, 0x9bb2a2, 0x142a1c));
	CHECK(shades_are(0xc6c1aa, 0xe7e5db, 0x6b685c));
	CHECK(shades_are(0xaaaaaa, 0xdadada, 0x5a5a5a));
	CHECK(shades_are(COLOR_ACTIVE, 0x9db2c9, 0x152a42));
	CHECK(shades_are(COLOR_INACTIVE, 0xdfdfdf, 0x616161));
	CHECK(shades_are(COLOR_BASIC, 0xe6e6e6, 0x6c6c6c));
	/* Light, above 93%: both towards black, the top less. */
	CHECK(shades_are(0xf0f0f0, 0xc0c0c0, 0x909090));
	CHECK(shades_are(0xffffff, 0xcccccc, 0x999999));
	/* Dark, below 20%: both towards white, the bottom less. */
	CHECK(shades_are(0x202020, 0x909090, 0x636363));
	CHECK(shades_are(0x000000, 0x7f7f7f, 0x4c4c4c));
	/* The edges of the medium range. */
	CHECK((color_shades(0x323232).bottom & 0xff) > 0x32);
	CHECK((color_shades(0x333333).bottom & 0xff) < 0x33);
	CHECK((color_shades(0xededed).top & 0xff) > 0xed);
	CHECK((color_shades(0xeeeeee).top & 0xff) < 0xee);
	/* Exactly at 20% and 93%: still medium. */
	CHECK(shades_are(0x002287, 0x8293c4, 0x000e3a));
	CHECK(shades_are(0xcbfdfe, 0xeaffff, 0x769393));
}

/* The defaults mwc and mtk share. */
TEST(defaults)
{
	CHECK_INT(COLOR_ACTIVE, 0x2e5a8c);
	CHECK_INT(COLOR_INACTIVE, 0xb4b4b4);
	CHECK_INT(COLOR_BASIC, 0xc4c4c4);
	CHECK_INT(COLOR_TEXT, 0x000000);
	CHECK_INT(COLOR_ROOT, 0x5b7792);
}

int main(void)
{
	RUN(text);
	RUN(shades);
	RUN(defaults);
	return unit_done();
}
