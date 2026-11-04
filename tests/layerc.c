/*
 * A minimal layer-shell client for the tests: a solid rectangle, as a panel or
 * wallpaper tool would show one. Prints what it receives, one event per line.
 *
 *   layerc key=value ...
 *     layer=background|bottom|top|overlay   default top
 *     anchor=top,bottom,left,right          default none
 *     size=WxH                              default 100x100, 0 stretches
 *     exclusive=N                           exclusive zone, default 0
 *     keyboard=none|exclusive|on-demand     default none
 *     color=rrggbb                          default ff00ff
 *     popup=rrggbb                          also open a 100x50 popup, like a
 *                                           panel's menu
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client.h"
#include "xdg-shell-client.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct xdg_wm_base *wm_base;

static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static uint32_t color = 0xff00ff, popup_color;
static bool want_popup, closed;

static struct wl_surface *popup_surface;
static struct xdg_surface *popup_xdg;

static void attach_color(struct wl_surface *target, int w, int h, uint32_t rgb)
{
	size_t size = w * h * 4;
	int fd = memfd_create("layerc", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) != 0) {
		perror("layerc: buffer");
		exit(1);
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (size_t i = 0; i < size / 4; i++)
		px[i] = 0xff000000 | rgb;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(target, buffer, 0, 0);
	wl_surface_damage_buffer(target, 0, 0, w, h);
	wl_surface_commit(target);
}

/* ---- popup ---------------------------------------------------------------- */

static void popup_xdg_configure(void *data, struct xdg_surface *xdg,
	uint32_t serial)
{
	xdg_surface_ack_configure(xdg, serial);
	attach_color(popup_surface, 100, 50, popup_color);
	printf("popup-configured\n");
}

static const struct xdg_surface_listener popup_xdg_listener = {
	.configure = popup_xdg_configure,
};

static void popup_configure(void *data, struct xdg_popup *popup, int32_t x,
	int32_t y, int32_t w, int32_t h)
{
}

static void popup_done(void *data, struct xdg_popup *popup)
{
	printf("popup-done\n");
}

static void popup_repositioned(void *data, struct xdg_popup *popup,
	uint32_t token)
{
}

static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure,
	.popup_done = popup_done,
	.repositioned = popup_repositioned,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

/* A popup of a layer surface has no xdg parent: get_popup assigns it later. */
static void open_popup(int parent_w, int parent_h)
{
	popup_surface = wl_compositor_create_surface(compositor);
	popup_xdg = xdg_wm_base_get_xdg_surface(wm_base, popup_surface);
	xdg_surface_add_listener(popup_xdg, &popup_xdg_listener, NULL);
	struct xdg_positioner *pos = xdg_wm_base_create_positioner(wm_base);
	xdg_positioner_set_size(pos, 100, 50);
	xdg_positioner_set_anchor_rect(pos, 0, 0, parent_w, parent_h);
	xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
	xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(pos,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
	struct xdg_popup *popup = xdg_surface_get_popup(popup_xdg, NULL, pos);
	xdg_popup_add_listener(popup, &popup_listener, NULL);
	zwlr_layer_surface_v1_get_popup(layer_surface, popup);
	xdg_positioner_destroy(pos);
	wl_surface_commit(popup_surface);
}

/* ---- layer surface -------------------------------------------------------- */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
	uint32_t serial, uint32_t w, uint32_t h)
{
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	attach_color(surface, w, h, color);
	printf("configured %ux%u\n", w, h);
	if (want_popup) {
		want_popup = false;
		open_popup(w, h);
	}
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	printf("closed\n");
	closed = true;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

/* ---- input, logged for the test ------------------------------------------- */

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	printf("pointer-enter\n");
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s)
{
	printf("pointer-leave\n");
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x,
	wl_fixed_t y)
{
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
	uint32_t button, uint32_t state)
{
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		printf("button\n");
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis,
	wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
};

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t format, int32_t fd,
	uint32_t size)
{
	close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s, struct wl_array *keys)
{
	printf("keyboard-enter\n");
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s)
{
	printf("keyboard-leave\n");
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t,
	uint32_t key, uint32_t state)
{
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		printf("key %u\n", key);
}
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial,
	uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat,
};

static void seat_capabilities(void *d, struct wl_seat *s, uint32_t caps)
{
	static bool have_pointer, have_keyboard;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !have_pointer) {
		have_pointer = true;
		wl_pointer_add_listener(wl_seat_get_pointer(s), &pointer_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !have_keyboard) {
		have_keyboard = true;
		wl_keyboard_add_listener(wl_seat_get_keyboard(s), &keyboard_listener,
			NULL);
	}
}

static void seat_name(void *d, struct wl_seat *s, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
	uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	else if (strcmp(interface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
		layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 4); /* 4 has on-demand */
	else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
	uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static uint32_t parse_layer(const char *v)
{
	if (strcmp(v, "background") == 0)
		return ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
	if (strcmp(v, "bottom") == 0)
		return ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
	if (strcmp(v, "overlay") == 0)
		return ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	return ZWLR_LAYER_SHELL_V1_LAYER_TOP;
}

static uint32_t parse_anchor(const char *v)
{
	uint32_t a = 0;
	if (strstr(v, "top"))
		a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	if (strstr(v, "bottom"))
		a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	if (strstr(v, "left"))
		a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	if (strstr(v, "right"))
		a |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	return a;
}

int main(int argc, char *argv[])
{
	uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP, anchor = 0;
	uint32_t keyboard = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
	unsigned w = 100, h = 100;
	int exclusive = 0;
	for (int i = 1; i < argc; i++) {
		char *v = strchr(argv[i], '=');
		if (!v) {
			fprintf(stderr, "layerc: expected key=value: %s\n", argv[i]);
			return 2;
		}
		*v++ = '\0';
		if (strcmp(argv[i], "layer") == 0)
			layer = parse_layer(v);
		else if (strcmp(argv[i], "anchor") == 0)
			anchor = parse_anchor(v);
		else if (strcmp(argv[i], "size") == 0)
			sscanf(v, "%ux%u", &w, &h);
		else if (strcmp(argv[i], "exclusive") == 0)
			exclusive = atoi(v);
		else if (strcmp(argv[i], "color") == 0)
			color = strtoul(v, NULL, 16);
		else if (strcmp(argv[i], "popup") == 0) {
			popup_color = strtoul(v, NULL, 16);
			want_popup = true;
		} else if (strcmp(argv[i], "keyboard") == 0) {
			if (strcmp(v, "exclusive") == 0)
				keyboard = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
			else if (strcmp(v, "on-demand") == 0)
				keyboard = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "layerc: cannot connect to the compositor\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !layer_shell || !wm_base) {
		fprintf(stderr, "layerc: the compositor lacks a needed global\n");
		return 1;
	}

	surface = wl_compositor_create_surface(compositor);
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface,
		NULL, layer, "layerc");
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_size(layer_surface, w, h);
	zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, exclusive);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, keyboard);
	wl_surface_commit(surface);

	while (!closed && wl_display_dispatch(display) != -1) {
	}
	return 0;
}
