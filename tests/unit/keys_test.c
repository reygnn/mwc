/*
 * keys.c: which command each of mwm's and mwc's keys means, which keys stay the
 * client's, and what the focused window has to be for the others.
 */
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_keyboard.h>
#include "../../src/mwc.h"
#include "unit.h"

#define C WLR_MODIFIER_CTRL
#define A WLR_MODIFIER_ALT
#define S WLR_MODIFIER_SHIFT
#define L WLR_MODIFIER_LOGO

/* With a focused window, alone or in a group. */
static struct key_command key(uint32_t mods, uint32_t sym)
{
	return keys_lookup(mods, sym, true, false);
}

static struct key_command grouped(uint32_t mods, uint32_t sym)
{
	return keys_lookup(mods, sym, true, true);
}

static struct key_command no_window(uint32_t mods, uint32_t sym)
{
	return keys_lookup(mods, sym, false, false);
}

TEST(way_out)
{
	/* Whatever else is held, and whatever has the keyboard. */
	for (uint32_t mods = 0; mods < 256; mods++) {
		struct key_command c = no_window(mods, XKB_KEY_XF86Switch_VT_1);
		CHECK_INT(c.action, KEY_SWITCH_VT);
		CHECK_INT(c.arg, 1);
		CHECK(c.always);
	}
	CHECK_INT(no_window(C | A, XKB_KEY_XF86Switch_VT_12).arg, 12);
	CHECK_INT(no_window(C | A, XKB_KEY_BackSpace).action, KEY_QUIT);
	CHECK(no_window(C | A, XKB_KEY_BackSpace).always);
	CHECK_INT(no_window(C | A | S, XKB_KEY_BackSpace).action, KEY_QUIT);
	CHECK_INT(no_window(A, XKB_KEY_BackSpace).action, KEY_NONE);
	CHECK_INT(no_window(0, XKB_KEY_BackSpace).action, KEY_NONE);
	/* Nothing else is. */
	CHECK(!key(A, XKB_KEY_F4).always);
	CHECK(!key(L, XKB_KEY_Return).always);
}

TEST(programs)
{
	CHECK_INT(no_window(C | A, XKB_KEY_Return).action, KEY_TERMINAL);
	CHECK_INT(no_window(L, XKB_KEY_Return).action, KEY_TERMINAL);
	CHECK_INT(no_window(L | S, XKB_KEY_Return).action, KEY_TERMINAL);
	CHECK_INT(no_window(0, XKB_KEY_Return).action, KEY_NONE);
	CHECK_INT(no_window(C, XKB_KEY_Return).action, KEY_NONE);
	CHECK_INT(no_window(L, XKB_KEY_Escape).action, KEY_SCREEN_SAVER);
	CHECK_INT(no_window(L | S, XKB_KEY_Escape).action, KEY_NONE);
	CHECK_INT(no_window(L | C, XKB_KEY_Escape).action, KEY_NONE);
}

TEST(screenshots)
{
	CHECK_INT(no_window(0, XKB_KEY_Print).action, KEY_SCREENSHOT);
	CHECK_INT(no_window(0, XKB_KEY_Print).arg, 0);
	CHECK_INT(no_window(S, XKB_KEY_Print).arg, KEY_SHOT_WHOLE);
	CHECK_INT(no_window(C, XKB_KEY_Print).arg, KEY_SHOT_FILE);
	CHECK_INT(no_window(C | S, XKB_KEY_Print).arg, KEY_SHOT_FILE | KEY_SHOT_WHOLE);
	CHECK_INT(no_window(A, XKB_KEY_Print).action, KEY_NONE);
	CHECK_INT(no_window(L, XKB_KEY_Print).action, KEY_NONE);
}

TEST(tabs)
{
	CHECK_INT(key(L, XKB_KEY_g).action, KEY_TAB_JOIN);
	CHECK_INT(key(L | S, XKB_KEY_G).action, KEY_TAB_DETACH);
	/* Caps Lock gives the other case. */
	CHECK_INT(key(L, XKB_KEY_G).action, KEY_TAB_JOIN);
	CHECK_INT(key(L | S, XKB_KEY_g).action, KEY_TAB_DETACH);
	/* Without a window to join there is nothing to keep the key for. */
	CHECK_INT(no_window(L, XKB_KEY_g).action, KEY_NONE);
	CHECK_INT(key(L | A, XKB_KEY_g).action, KEY_NONE);
	CHECK_INT(key(L | C, XKB_KEY_g).action, KEY_NONE);

	CHECK_INT(grouped(L, XKB_KEY_Tab).action, KEY_TAB_NEXT);
	CHECK_INT(grouped(L | S, XKB_KEY_ISO_Left_Tab).action, KEY_TAB_PREVIOUS);
	CHECK_INT(grouped(L, XKB_KEY_1).action, KEY_TAB_NTH);
	CHECK_INT(grouped(L, XKB_KEY_1).arg, 0);
	CHECK_INT(grouped(L, XKB_KEY_9).arg, 8);
	CHECK_INT(grouped(L, XKB_KEY_0).action, KEY_NONE);
	CHECK_INT(grouped(L | S, XKB_KEY_1).action, KEY_NONE);
	/* A window on its own: the client's keys. */
	CHECK_INT(key(L, XKB_KEY_Tab).action, KEY_NONE);
	CHECK_INT(key(L, XKB_KEY_ISO_Left_Tab).action, KEY_NONE);
	CHECK_INT(key(L, XKB_KEY_1).action, KEY_NONE);
	CHECK_INT(no_window(L, XKB_KEY_Tab).action, KEY_NONE);
}

TEST(tiling_and_snapping)
{
	CHECK_INT(no_window(L, XKB_KEY_t).action, KEY_TILE_TOGGLE);
	CHECK_INT(no_window(L, XKB_KEY_T).action, KEY_TILE_TOGGLE);
	CHECK_INT(no_window(L | S, XKB_KEY_T).action, KEY_NONE);
	CHECK_INT(no_window(L | A, XKB_KEY_t).action, KEY_NONE);

	static const struct {
		uint32_t sym;
		enum snap_dir dir;
	} arrows[] = {
		{ XKB_KEY_Left, SNAP_DIR_LEFT }, { XKB_KEY_Right, SNAP_DIR_RIGHT },
		{ XKB_KEY_Up, SNAP_DIR_UP }, { XKB_KEY_Down, SNAP_DIR_DOWN },
	};
	for (int i = 0; i < 4; i++) {
		/* Kept from the client also with no window to snap. */
		CHECK_INT(no_window(L, arrows[i].sym).action, KEY_SNAP);
		CHECK_INT(key(L, arrows[i].sym).arg, arrows[i].dir);
		CHECK_INT(key(L | S, arrows[i].sym).action, KEY_EDGE);
		CHECK_INT(key(L | S, arrows[i].sym).arg, arrows[i].dir);
		CHECK_INT(key(L | C, arrows[i].sym).action, KEY_NONE);
		CHECK_INT(key(0, arrows[i].sym).action, KEY_NONE);
	}
}

TEST(mwm_defaults)
{
	CHECK_INT(key(S, XKB_KEY_Escape).action, KEY_WINDOW_MENU);
	CHECK_INT(no_window(S, XKB_KEY_Escape).action, KEY_NONE);
	CHECK_INT(key(S | C, XKB_KEY_Escape).action, KEY_NONE);
	CHECK_INT(key(0, XKB_KEY_Escape).action, KEY_NONE);

	CHECK_INT(key(A, XKB_KEY_Tab).action, KEY_CYCLE);
	CHECK_INT(key(A | S, XKB_KEY_ISO_Left_Tab).action, KEY_CYCLE_BACK);
	CHECK_INT(key(A, XKB_KEY_Escape).action, KEY_CYCLE_BACK);
	CHECK_INT(key(A | S, XKB_KEY_Escape).action, KEY_CYCLE_BACK);
	/* Alt+space is kept also with no window to post the menu of. */
	CHECK_INT(no_window(A, XKB_KEY_space).action, KEY_WINDOW_MENU);

	static const struct {
		uint32_t sym;
		enum menu_action action;
	} f[] = {
		{ XKB_KEY_F3, ACT_LOWER }, { XKB_KEY_F4, ACT_CLOSE }, { XKB_KEY_F5, ACT_RESTORE },
		{ XKB_KEY_F7, ACT_MOVE }, { XKB_KEY_F8, ACT_SIZE }, { XKB_KEY_F9, ACT_MINIMIZE },
		{ XKB_KEY_F10, ACT_TOGGLE_MAXIMIZE },
	};
	for (size_t i = 0; i < sizeof(f) / sizeof(*f); i++) {
		CHECK_INT(key(A, f[i].sym).action, KEY_MENU_ACTION);
		CHECK_INT(key(A, f[i].sym).arg, f[i].action);
		CHECK_INT(key(A | S, f[i].sym).action, KEY_MENU_ACTION);
		/* Not with Ctrl: Ctrl+Alt and an F key is a VT's. */
		CHECK_INT(key(A | C, f[i].sym).action, KEY_NONE);
		CHECK_INT(key(0, f[i].sym).action, KEY_NONE);
	}
	CHECK_INT(key(A, XKB_KEY_F6).action, KEY_NONE);
	CHECK_INT(key(A, XKB_KEY_F11).action, KEY_FULLSCREEN);
	CHECK_INT(key(A | C, XKB_KEY_Tab).action, KEY_NONE);
}

/* Lock and the other modifiers mwc doesn't bind change nothing. */
TEST(other_modifiers)
{
	uint32_t other = WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2;
	CHECK_INT(key(A | other, XKB_KEY_F4).action, KEY_MENU_ACTION);
	CHECK_INT(key(L | other, XKB_KEY_Left).action, KEY_SNAP);
	CHECK_INT(key(other, XKB_KEY_Print).action, KEY_SCREENSHOT);
	CHECK_INT(key(other, XKB_KEY_a).action, KEY_NONE);
}

int main(void)
{
	RUN(way_out);
	RUN(programs);
	RUN(screenshots);
	RUN(tabs);
	RUN(tiling_and_snapping);
	RUN(mwm_defaults);
	RUN(other_modifiers);
	return unit_done();
}
