/*
 * keys.c - mwm's default key bindings and mwc's own, as a table: which command a key
 * pressed with these modifiers means. It looks at its arguments and calls nothing;
 * input.c asks it and acts. Tested in tests/unit.
 */
#define _POSIX_C_SOURCE 200809L
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_keyboard.h>
#include "mwc.h"

#define C WLR_MODIFIER_CTRL
#define A WLR_MODIFIER_ALT
#define S WLR_MODIFIER_SHIFT
#define L WLR_MODIFIER_LOGO
#define ANY (C | A | S | L)

enum {
	ALWAYS = 1 << 0, /* also under a menu, a grab or an exclusive layer surface */
	VIEW = 1 << 1,   /* only with a focused window: otherwise the client's key */
	GROUP = 1 << 2,  /* only while that window is in a tab group */
	/* A letter by its lower case, or Caps Lock would turn the binding off. */
	FOLD = 1 << 3,
};

struct binding {
	uint32_t mods, mask;    /* of ANY: those in mask are as in mods, the rest as it likes */
	xkb_keysym_t sym, last; /* last: any key from sym to it, arg counting up from sym's */
	unsigned flags;
	enum key_action action;
	int arg;
};

/* The first that fits is taken. */
static const struct binding bindings[] = {
	/* These two work whatever else is going on, or there is no way out. */
	{ 0, 0, XKB_KEY_XF86Switch_VT_1, XKB_KEY_XF86Switch_VT_12, ALWAYS, KEY_SWITCH_VT, 1 },
	{ C | A, C | A, XKB_KEY_BackSpace, 0, ALWAYS, KEY_QUIT, 0 },

	{ C | A, C | A, XKB_KEY_Return, 0, 0, KEY_TERMINAL, 0 },
	{ L, L, XKB_KEY_Return, 0, 0, KEY_TERMINAL, 0 },
	{ L, ANY, XKB_KEY_Escape, 0, 0, KEY_SCREEN_SAVER, 0 },
	/* Print: a rectangle to the clipboard, the more common need; Shift for the whole
	 * screen, Ctrl to a file instead. Shift+Print and Ctrl+Print still give Print in
	 * the usual layouts, Alt+Print gives Sys_Req. */
	{ 0, ANY, XKB_KEY_Print, 0, 0, KEY_SCREENSHOT, 0 },
	{ S, ANY, XKB_KEY_Print, 0, 0, KEY_SCREENSHOT, KEY_SHOT_WHOLE },
	{ C, ANY, XKB_KEY_Print, 0, 0, KEY_SCREENSHOT, KEY_SHOT_FILE },
	{ C | S, ANY, XKB_KEY_Print, 0, 0, KEY_SCREENSHOT, KEY_SHOT_FILE | KEY_SHOT_WHOLE },

	/* Tabs, while there are any: otherwise these are the client's. */
	{ L, ANY, XKB_KEY_g, 0, VIEW | FOLD, KEY_TAB_JOIN, 0 },
	{ L | S, ANY, XKB_KEY_g, 0, VIEW | FOLD, KEY_TAB_DETACH, 0 },
	{ L, L | A | C, XKB_KEY_Tab, 0, GROUP, KEY_TAB_NEXT, 0 },
	{ L, L | A | C, XKB_KEY_ISO_Left_Tab, 0, GROUP, KEY_TAB_PREVIOUS, 0 },
	{ L, ANY, XKB_KEY_1, XKB_KEY_9, GROUP, KEY_TAB_NTH, 0 },

	{ L, ANY, XKB_KEY_t, 0, FOLD, KEY_TILE_TOGGLE, 0 },
	/* As in Windows 11, see snaprules.c; with Shift against that edge. */
	{ L, ANY, XKB_KEY_Left, 0, 0, KEY_SNAP, SNAP_DIR_LEFT },
	{ L, ANY, XKB_KEY_Right, 0, 0, KEY_SNAP, SNAP_DIR_RIGHT },
	{ L, ANY, XKB_KEY_Up, 0, 0, KEY_SNAP, SNAP_DIR_UP },
	{ L, ANY, XKB_KEY_Down, 0, 0, KEY_SNAP, SNAP_DIR_DOWN },
	{ L | S, ANY, XKB_KEY_Left, 0, 0, KEY_EDGE, SNAP_DIR_LEFT },
	{ L | S, ANY, XKB_KEY_Right, 0, 0, KEY_EDGE, SNAP_DIR_RIGHT },
	{ L | S, ANY, XKB_KEY_Up, 0, 0, KEY_EDGE, SNAP_DIR_UP },
	{ L | S, ANY, XKB_KEY_Down, 0, 0, KEY_EDGE, SNAP_DIR_DOWN },

	/* mwm's: Shift+Escape and Alt+space post the window menu. */
	{ S, ANY, XKB_KEY_Escape, 0, VIEW, KEY_WINDOW_MENU, 0 },
	{ A, A | C, XKB_KEY_Tab, 0, 0, KEY_CYCLE, 0 },
	{ A, A | C, XKB_KEY_ISO_Left_Tab, 0, 0, KEY_CYCLE_BACK, 0 },
	{ A, A | C, XKB_KEY_Escape, 0, 0, KEY_CYCLE_BACK, 0 },
	{ A, A | C, XKB_KEY_space, 0, 0, KEY_WINDOW_MENU, 0 },
	{ A, A | C, XKB_KEY_F3, 0, 0, KEY_MENU_ACTION, ACT_LOWER },
	{ A, A | C, XKB_KEY_F4, 0, 0, KEY_MENU_ACTION, ACT_CLOSE },
	{ A, A | C, XKB_KEY_F5, 0, 0, KEY_MENU_ACTION, ACT_RESTORE },
	{ A, A | C, XKB_KEY_F7, 0, 0, KEY_MENU_ACTION, ACT_MOVE },
	{ A, A | C, XKB_KEY_F8, 0, 0, KEY_MENU_ACTION, ACT_SIZE },
	{ A, A | C, XKB_KEY_F9, 0, 0, KEY_MENU_ACTION, ACT_MINIMIZE },
	{ A, A | C, XKB_KEY_F10, 0, 0, KEY_MENU_ACTION, ACT_TOGGLE_MAXIMIZE },
	{ A, A | C, XKB_KEY_F11, 0, 0, KEY_FULLSCREEN, 0 },
};

struct key_command keys_lookup(uint32_t mods, uint32_t sym, bool has_view, bool in_group)
{
	for (size_t i = 0; i < sizeof(bindings) / sizeof(*bindings); i++) {
		const struct binding *b = &bindings[i];
		if ((mods & b->mask) != b->mods)
			continue;
		if ((b->flags & VIEW) && !has_view)
			continue;
		if ((b->flags & GROUP) && !(has_view && in_group))
			continue;
		xkb_keysym_t key = (b->flags & FOLD) ? xkb_keysym_to_lower(sym) : sym;
		if (key < b->sym || key > (b->last ? b->last : b->sym))
			continue;
		return (struct key_command){ .action = b->action,
			.arg = b->arg + (int)(key - b->sym), .always = b->flags & ALWAYS };
	}
	return (struct key_command){ .action = KEY_NONE };
}
