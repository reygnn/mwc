/*
 * Simulated mouse and keyboard for the headless tests. Reads one command per
 * line from stdin and answers "ok" once mwc has processed it:
 *
 *   move X Y           absolute pointer position
 *   down|up|click B    B is left, middle or right
 *   key COMBO          press and release, e.g. "alt+F4", "shift+Escape", "q"
 *   hold|release MOD   keep alt, ctrl, shift or super down across commands
 *   caps on|off        lock or unlock Caps
 *   wheel N            turn the wheel N steps
 *
 * Usage: vinput WIDTH HEIGHT (the size of the headless output)
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-unstable-v1-client.h"
#include "wlr-virtual-pointer-unstable-v1-client.h"

/* Modifier masks of the standard "us" keymap compiled below. */
#define MASK_SHIFT 1u
#define MASK_CTRL 4u
#define MASK_ALT 8u
#define MASK_SUPER 64u

static struct wl_display *display;
static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *kb_manager;
static struct zwlr_virtual_pointer_manager_v1 *ptr_manager;
static struct zwp_virtual_keyboard_v1 *keyboard;
static struct zwlr_virtual_pointer_v1 *pointer;
static uint32_t held_mods;
static int width, height;

static const struct {
	const char *name;
	uint32_t code;
} keys[] = {
	{ "Tab", KEY_TAB }, { "Escape", KEY_ESC }, { "space", KEY_SPACE },
	{ "Return", KEY_ENTER }, { "BackSpace", KEY_BACKSPACE },
	{ "Left", KEY_LEFT }, { "Right", KEY_RIGHT }, { "Up", KEY_UP },
	{ "Down", KEY_DOWN },
	{ "F1", KEY_F1 }, { "F2", KEY_F2 }, { "F3", KEY_F3 }, { "F4", KEY_F4 },
	{ "F5", KEY_F5 }, { "F6", KEY_F6 }, { "F7", KEY_F7 }, { "F8", KEY_F8 },
	{ "F9", KEY_F9 }, { "F10", KEY_F10 }, { "F11", KEY_F11 },
	{ "a", KEY_A }, { "b", KEY_B }, { "c", KEY_C }, { "d", KEY_D },
	{ "e", KEY_E }, { "f", KEY_F }, { "g", KEY_G }, { "h", KEY_H },
	{ "i", KEY_I }, { "j", KEY_J }, { "k", KEY_K }, { "l", KEY_L },
	{ "m", KEY_M }, { "n", KEY_N }, { "o", KEY_O }, { "p", KEY_P },
	{ "q", KEY_Q }, { "r", KEY_R }, { "s", KEY_S }, { "t", KEY_T },
	{ "u", KEY_U }, { "v", KEY_V }, { "w", KEY_W }, { "x", KEY_X },
	{ "y", KEY_Y }, { "z", KEY_Z },
};

static const struct {
	const char *name;
	uint32_t code, mask;
} mods[] = {
	{ "alt", KEY_LEFTALT, MASK_ALT },
	{ "ctrl", KEY_LEFTCTRL, MASK_CTRL },
	{ "shift", KEY_LEFTSHIFT, MASK_SHIFT },
	{ "super", KEY_LEFTMETA, MASK_SUPER },
};

static uint32_t now_msec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void registry_global(void *data, struct wl_registry *registry,
	uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_seat_interface.name) == 0)
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	else if (strcmp(interface,
			zwp_virtual_keyboard_manager_v1_interface.name) == 0)
		kb_manager = wl_registry_bind(registry, name,
			&zwp_virtual_keyboard_manager_v1_interface, 1);
	else if (strcmp(interface,
			zwlr_virtual_pointer_manager_v1_interface.name) == 0)
		ptr_manager = wl_registry_bind(registry, name,
			&zwlr_virtual_pointer_manager_v1_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *registry,
	uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* A virtual keyboard must upload its keymap before the first key. */
static void send_keymap(void)
{
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = { .layout = "us" };
	struct xkb_keymap *map = xkb_keymap_new_from_names(ctx, &names,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!map) {
		fprintf(stderr, "vinput: cannot compile the keymap\n");
		exit(1);
	}
	char *text = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t size = strlen(text) + 1;
	int fd = memfd_create("vinput-keymap", MFD_CLOEXEC);
	if (fd < 0 || write(fd, text, size) != (ssize_t)size) {
		perror("vinput: keymap");
		exit(1);
	}
	zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
		fd, size);
	close(fd);
	free(text);
	xkb_keymap_unref(map);
	xkb_context_unref(ctx);
}

static void send_key(uint32_t code, bool pressed)
{
	zwp_virtual_keyboard_v1_key(keyboard, now_msec(), code,
		pressed ? WL_KEYBOARD_KEY_STATE_PRESSED :
		WL_KEYBOARD_KEY_STATE_RELEASED);
}

/* The compositor doesn't track modifiers of a virtual keyboard itself. */
static uint32_t locked_mods;

static void set_mod(int idx, bool pressed)
{
	if (pressed)
		held_mods |= mods[idx].mask;
	else
		held_mods &= ~mods[idx].mask;
	send_key(mods[idx].code, pressed);
	zwp_virtual_keyboard_v1_modifiers(keyboard, held_mods, 0, locked_mods, 0);
}

static int find_mod(const char *name)
{
	for (size_t i = 0; i < sizeof(mods) / sizeof(*mods); i++) {
		if (strcmp(mods[i].name, name) == 0)
			return i;
	}
	return -1;
}

static int find_key(const char *name, uint32_t *code)
{
	for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
		if (strcmp(keys[i].name, name) == 0) {
			*code = keys[i].code;
			return 0;
		}
	}
	return -1;
}

static const char *do_key(char *combo)
{
	int pressed[4], n = 0;
	char *last = strrchr(combo, '+');
	char *name = last ? last + 1 : combo;
	uint32_t code;
	if (find_key(name, &code) != 0)
		return "unknown key";
	if (last) {
		*last = '\0';
		char *save = NULL;
		for (char *m = strtok_r(combo, "+", &save); m;
				m = strtok_r(NULL, "+", &save)) {
			int idx = find_mod(m);
			if (idx < 0 || n == 4)
				return "unknown modifier";
			pressed[n++] = idx;
		}
	}
	for (int i = 0; i < n; i++)
		set_mod(pressed[i], true);
	send_key(code, true);
	send_key(code, false);
	for (int i = n - 1; i >= 0; i--)
		set_mod(pressed[i], false);
	return NULL;
}

static const char *do_button(const char *name, bool press, bool release)
{
	uint32_t button;
	if (strcmp(name, "left") == 0)
		button = BTN_LEFT;
	else if (strcmp(name, "middle") == 0)
		button = BTN_MIDDLE;
	else if (strcmp(name, "right") == 0)
		button = BTN_RIGHT;
	else
		return "unknown button";
	if (press) {
		zwlr_virtual_pointer_v1_button(pointer, now_msec(), button,
			WL_POINTER_BUTTON_STATE_PRESSED);
		zwlr_virtual_pointer_v1_frame(pointer);
	}
	if (release) {
		zwlr_virtual_pointer_v1_button(pointer, now_msec(), button,
			WL_POINTER_BUTTON_STATE_RELEASED);
		zwlr_virtual_pointer_v1_frame(pointer);
	}
	return NULL;
}

static const char *run_command(char *line)
{
	char *save = NULL;
	char *cmd = strtok_r(line, " \n", &save);
	char *a = cmd ? strtok_r(NULL, " \n", &save) : NULL;
	char *b = a ? strtok_r(NULL, " \n", &save) : NULL;
	if (!cmd)
		return "empty command";
	if (strcmp(cmd, "move") == 0 && a && b) {
		/* Aim at the pixel centre: an exact X/width lands just below
		 * X after the compositor's floating-point mapping. */
		/* A real mouse stops at the screen's edge, too. */
		int x = atoi(a), y = atoi(b);
		x = x < 0 ? 0 : x >= width ? width - 1 : x;
		y = y < 0 ? 0 : y >= height ? height - 1 : y;
		zwlr_virtual_pointer_v1_motion_absolute(pointer, now_msec(),
			2 * x + 1, 2 * y + 1, 2 * width, 2 * height);
		zwlr_virtual_pointer_v1_frame(pointer);
		return NULL;
	}
	if (!a)
		return "missing argument";
	if (strcmp(cmd, "down") == 0)
		return do_button(a, true, false);
	if (strcmp(cmd, "up") == 0)
		return do_button(a, false, true);
	if (strcmp(cmd, "click") == 0)
		return do_button(a, true, true);
	if (strcmp(cmd, "key") == 0)
		return do_key(a);
	if (strcmp(cmd, "wheel") == 0 && a) {
		zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
		zwlr_virtual_pointer_v1_axis_discrete(pointer, now_msec(),
			WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(15 * atoi(a)), atoi(a));
		zwlr_virtual_pointer_v1_frame(pointer);
		return NULL;
	}
	if (strcmp(cmd, "caps") == 0) {
		locked_mods = strcmp(a, "on") == 0 ? 2 : 0; /* Lock is the second modifier */
		zwp_virtual_keyboard_v1_modifiers(keyboard, held_mods, 0, locked_mods, 0);
		return NULL;
	}
	if (strcmp(cmd, "hold") == 0 || strcmp(cmd, "release") == 0) {
		int idx = find_mod(a);
		if (idx < 0)
			return "unknown modifier";
		set_mod(idx, cmd[0] == 'h');
		return NULL;
	}
	return "unknown command";
}

int main(int argc, char *argv[])
{
	if (argc != 3) {
		fprintf(stderr, "usage: vinput WIDTH HEIGHT\n");
		return 2;
	}
	width = atoi(argv[1]);
	height = atoi(argv[2]);

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "vinput: cannot connect to the compositor\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!seat || !kb_manager || !ptr_manager) {
		fprintf(stderr, "vinput: virtual input protocols are missing, "
			"is MWC_TEST_STATE set?\n");
		return 1;
	}
	keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
		kb_manager, seat);
	pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
		ptr_manager, seat);
	send_keymap();
	wl_display_roundtrip(display);

	char line[256];
	setvbuf(stdout, NULL, _IOLBF, 0);
	while (fgets(line, sizeof(line), stdin)) {
		const char *error = run_command(line);
		if (wl_display_roundtrip(display) < 0) {
			/* Normal after a key that quits the compositor. */
			printf("gone\n");
			return 0;
		}
		if (error)
			printf("error: %s\n", error);
		else
			printf("ok\n");
	}
	return 0;
}
