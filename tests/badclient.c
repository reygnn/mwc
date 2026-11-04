/*
 * A Wayland client that does what well-behaved ones rarely do, all of it allowed
 * by the protocol or at least not to be punished with a crash. Each mode ends with
 * the client still connected, except where noted, so a test can poke the compositor
 * afterwards. Prints "done" when it has done its deed.
 *
 *   badclient move        configured, never mapped: xdg_toplevel.move, then destroy
 *   badclient menu        the same with show_window_menu
 *   badclient popup       destroy the toplevel, then get_popup with its xdg_surface
 *   badclient nested      popup of a popup whose toplevel is gone
 *   badclient remap       map, unmap with a null buffer, ask to maximize, map again
 *   badclient paste-close on every selection: receive it, and close the pipe at once
 *
 * These map a window, and log what the pointer and keyboard do to it:
 *
 *   badclient log [APP]         only that
 *   badclient popup-grab [APP]  a press opens a popup with a grab; a press in it too
 *   badclient drag [APP]        a press starts a drag with an icon
 *   badclient drop [APP]        accepts a drag
 *   badclient move-press [APP]  a press asks for xdg_toplevel.move
 *   badclient move-late [APP]   a release asks for it with the press's serial, too late
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_data_device_manager *ddm;
static struct wl_display *display;

static const char *app = "bad";
static const char *mode;
static struct wl_surface *main_surface;
static struct xdg_surface *main_xdg;
static struct xdg_toplevel *main_top;
static struct wl_data_device *data_device;
static uint32_t pointer_enter_serial;
static struct wl_data_offer *drag_offer;

static uint32_t configure_serial;
static bool configured;
static int config_w, config_h;

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name))
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	else if (!strcmp(interface, wl_shm_interface.name))
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, xdg_wm_base_interface.name))
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 2);
	else if (!strcmp(interface, wl_seat_interface.name))
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	else if (!strcmp(interface, wl_data_device_manager_interface.name))
		ddm = wl_registry_bind(registry, name,
			&wl_data_device_manager_interface, 3);
}

static void registry_global_remove(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_global, registry_global_remove,
};

static void xdg_configure(void *data, struct xdg_surface *x, uint32_t serial)
{
	configure_serial = serial;
	configured = true;
}

static const struct xdg_surface_listener xdg_listener = { xdg_configure };

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
	struct wl_array *states)
{
	config_w = w;
	config_h = h;
}

static void top_close(void *data, struct xdg_toplevel *t)
{
}

static void top_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h)
{
}

static void top_caps(void *data, struct xdg_toplevel *t, struct wl_array *c)
{
}

static const struct xdg_toplevel_listener top_listener = {
	top_configure, top_close, top_bounds, top_caps,
};

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { ping };

static void wait_configure(void)
{
	configured = false;
	while (!configured && wl_display_dispatch(display) != -1) {
	}
}

static struct wl_buffer *make_buffer(int w, int h)
{
	size_t size = w * h * 4;
	int fd = memfd_create("badclient", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) != 0) {
		perror("badclient: buffer");
		exit(1);
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

/* ---- input, logged --------------------------------------------------------- */

static void popup_xdg_configure(void *data, struct xdg_surface *x, uint32_t serial)
{
	xdg_surface_ack_configure(x, serial);
	struct wl_surface *s = data;
	wl_surface_attach(s, make_buffer(100, 50), 0, 0);
	wl_surface_commit(s);
}

static const struct xdg_surface_listener popup_xdg_listener = { popup_xdg_configure };

static void popup_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y,
	int32_t w, int32_t h)
{
}

static void popup_done(void *d, struct xdg_popup *p)
{
	printf("popup-done\n");
}

static void popup_repositioned(void *d, struct xdg_popup *p, uint32_t token)
{
}

static const struct xdg_popup_listener popup_listener = {
	popup_configure, popup_done, popup_repositioned,
};

static void open_grabbing_popup(uint32_t serial)
{
	struct wl_surface *s = wl_compositor_create_surface(compositor);
	struct xdg_surface *x = xdg_wm_base_get_xdg_surface(wm_base, s);
	xdg_surface_add_listener(x, &popup_xdg_listener, s);
	struct xdg_positioner *p = xdg_wm_base_create_positioner(wm_base);
	xdg_positioner_set_size(p, 100, 50);
	xdg_positioner_set_anchor_rect(p, 10, 10, 1, 1);
	xdg_positioner_set_anchor(p, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
	xdg_positioner_set_gravity(p, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	struct xdg_popup *popup = xdg_surface_get_popup(x, main_xdg, p);
	xdg_popup_add_listener(popup, &popup_listener, NULL);
	xdg_popup_grab(popup, seat, serial);
	xdg_positioner_destroy(p);
	wl_surface_commit(s);
	printf("popup-open\n");
}

static void source_target(void *d, struct wl_data_source *s, const char *m)
{
}

static void source_send(void *d, struct wl_data_source *s, const char *m, int32_t fd)
{
	close(fd);
}

static void source_cancelled(void *d, struct wl_data_source *s)
{
	printf("drag-cancelled\n");
}

static void source_drop_performed(void *d, struct wl_data_source *s)
{
	printf("drag-dropped\n");
}

static void source_finished(void *d, struct wl_data_source *s)
{
	printf("drag-finished\n");
}

static void source_action(void *d, struct wl_data_source *s, uint32_t a)
{
}

static const struct wl_data_source_listener source_listener = {
	source_target, source_send, source_cancelled, source_drop_performed,
	source_finished, source_action,
};

static void start_a_drag(uint32_t serial)
{
	struct wl_data_source *source = wl_data_device_manager_create_data_source(ddm);
	wl_data_source_add_listener(source, &source_listener, NULL);
	wl_data_source_offer(source, "text/plain");
	wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
	struct wl_surface *icon = wl_compositor_create_surface(compositor);
	wl_data_device_start_drag(data_device, source, main_surface, icon, serial);
	/* Solid red, or the test can't tell it from anything else. */
	size_t size = 30 * 30 * 4;
	int fd = memfd_create("icon", MFD_CLOEXEC);
	if (ftruncate(fd, size) == 0) {
		uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		for (size_t i = 0; i < size / 4; i++)
			px[i] = 0xffff0000;
		munmap(px, size);
		struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
		wl_surface_attach(icon, wl_shm_pool_create_buffer(pool, 0, 30, 30, 120,
			WL_SHM_FORMAT_ARGB8888), 0, 0);
		wl_shm_pool_destroy(pool);
		wl_surface_commit(icon);
	}
	close(fd);
	printf("drag-start\n");
}

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	pointer_enter_serial = serial;
	printf("enter\n");
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s)
{
	printf("leave\n");
}

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x,
	wl_fixed_t y)
{
	printf("motion %d %d\n", wl_fixed_to_int(x), wl_fixed_to_int(y));
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
	uint32_t button, uint32_t state)
{
	static uint32_t press_serial;
	bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
	printf("button %u %s\n", button, down ? "down" : "up");
	if (down)
		press_serial = serial;
	if (down && !strcmp(mode, "popup-grab"))
		open_grabbing_popup(serial);
	else if (down && !strcmp(mode, "drag"))
		start_a_drag(serial);
	else if (down && !strcmp(mode, "move-press"))
		xdg_toplevel_move(main_top, seat, serial);
	else if (!down && !strcmp(mode, "move-late"))
		xdg_toplevel_move(main_top, seat, press_serial);
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
	.button = ptr_button, .axis = ptr_axis,
};

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s)
{
	close(fd);
}

static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
	struct wl_array *keys)
{
	printf("kb-enter\n");
}

static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
	printf("kb-leave\n");
}

static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key,
	uint32_t state)
{
	printf("key %u %s\n", key, state == WL_KEYBOARD_KEY_STATE_PRESSED ? "down" : "up");
}

static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
	uint32_t lat, uint32_t lock, uint32_t group)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave, .key = kb_key,
	.modifiers = kb_modifiers,
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
		wl_keyboard_add_listener(wl_seat_get_keyboard(s), &keyboard_listener, NULL);
	}
}

static void seat_name(void *d, struct wl_seat *s, const char *n)
{
}

static const struct wl_seat_listener seat_listener = { seat_capabilities, seat_name };

static struct xdg_surface *make_popup(struct xdg_surface *parent)
{
	struct wl_surface *s = wl_compositor_create_surface(compositor);
	struct xdg_surface *x = xdg_wm_base_get_xdg_surface(wm_base, s);
	xdg_surface_add_listener(x, &xdg_listener, NULL);
	struct xdg_positioner *p = xdg_wm_base_create_positioner(wm_base);
	xdg_positioner_set_size(p, 50, 50);
	xdg_positioner_set_anchor_rect(p, 0, 0, 10, 10);
	xdg_surface_get_popup(x, parent, p);
	xdg_positioner_destroy(p);
	wl_surface_commit(s);
	wl_display_roundtrip(display);
	return x;
}

/* ---- the clipboard --------------------------------------------------------- */

static char *mime;
static bool paste_closes;

static void offer_mime(void *data, struct wl_data_offer *o, const char *type)
{
	if (!mime || !strcmp(type, "text/plain;charset=utf-8")) {
		free(mime);
		mime = strdup(type);
	}
}

static void offer_source_actions(void *data, struct wl_data_offer *o, uint32_t actions)
{
}

static void offer_action(void *data, struct wl_data_offer *o, uint32_t action)
{
}

static const struct wl_data_offer_listener offer_listener = {
	offer_mime, offer_source_actions, offer_action,
};

static void dd_offer(void *data, struct wl_data_device *dd, struct wl_data_offer *o)
{
	free(mime);
	mime = NULL;
	wl_data_offer_add_listener(o, &offer_listener, NULL);
}

static void dd_enter(void *data, struct wl_data_device *dd, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o)
{
	printf("dnd-enter\n");
	drag_offer = o;
	if (o) {
		wl_data_offer_accept(o, serial, "text/plain");
		wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
			WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
	}
}

static void dd_leave(void *data, struct wl_data_device *dd)
{
	printf("dnd-leave\n");
}

static void dd_motion(void *data, struct wl_data_device *dd, uint32_t t, wl_fixed_t x,
	wl_fixed_t y)
{
}

static void dd_drop(void *data, struct wl_data_device *dd)
{
	printf("dnd-drop\n");
	if (drag_offer)
		wl_data_offer_finish(drag_offer);
}

static void dd_selection(void *data, struct wl_data_device *dd, struct wl_data_offer *o)
{
	if (!o || !mime || !paste_closes)
		return;
	int fds[2];
	if (pipe(fds) != 0)
		return;
	wl_data_offer_receive(o, mime, fds[1]);
	close(fds[1]);
	close(fds[0]);
	printf("done\n");
}

static const struct wl_data_device_listener dd_listener = {
	dd_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection,
};

/* ---------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "usage: badclient MODE [APP]\n");
		return 2;
	}
	mode = argv[1];
	if (argc > 2)
		app = argv[2];
	setvbuf(stdout, NULL, _IOLBF, 0);
	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "badclient: cannot connect to the compositor\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	bool logging = !strcmp(mode, "log") || !strcmp(mode, "popup-grab") ||
		!strcmp(mode, "drag") || !strcmp(mode, "drop") ||
		!strcmp(mode, "move-press") || !strcmp(mode, "move-late");
	if (logging)
		wl_seat_add_listener(seat, &seat_listener, NULL);

	struct wl_surface *surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg, &xdg_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xdg);
	xdg_toplevel_add_listener(top, &top_listener, NULL);
	xdg_toplevel_set_app_id(top, app);
	main_surface = surface;
	main_xdg = xdg;
	main_top = top;
	wl_surface_commit(surface);
	wait_configure();
	xdg_surface_ack_configure(xdg, configure_serial);
	wl_display_roundtrip(display);

	if (logging) {
		data_device = wl_data_device_manager_get_data_device(ddm, seat);
		wl_data_device_add_listener(data_device, &dd_listener, NULL);
		wl_surface_attach(surface, make_buffer(300, 200), 0, 0);
		wl_surface_commit(surface);
		printf("up\n");
	} else if (!strcmp(mode, "remap")) {
		wl_surface_attach(surface, make_buffer(200, 100), 0, 0);
		wl_surface_commit(surface);
		wl_display_roundtrip(display);
		sleep(1);
		/* A null buffer unmaps, and the surface starts over. */
		wl_surface_attach(surface, NULL, 0, 0);
		wl_surface_commit(surface);
		wl_display_roundtrip(display);
		/* All of its state is gone, the app_id too. */
		xdg_toplevel_set_app_id(top, "bad");
		xdg_toplevel_unset_maximized(top);
		xdg_toplevel_set_maximized(top);
		wl_display_roundtrip(display);
		wl_surface_commit(surface);
		wait_configure();
		xdg_surface_ack_configure(xdg, configure_serial);
		printf("configured %dx%d\n", config_w, config_h);
		wl_surface_attach(surface, make_buffer(config_w > 0 ? config_w : 200,
			config_h > 0 ? config_h : 100), 0, 0);
		wl_surface_commit(surface);
	} else if (!strcmp(mode, "paste-close")) {
		paste_closes = true;
		wl_surface_attach(surface, make_buffer(200, 100), 0, 0);
		wl_surface_commit(surface);
		struct wl_data_device *dd =
			wl_data_device_manager_get_data_device(ddm, seat);
		wl_data_device_add_listener(dd, &dd_listener, NULL);
	} else {
		if (!strcmp(mode, "move"))
			xdg_toplevel_move(top, seat, 0);
		else if (!strcmp(mode, "menu"))
			xdg_toplevel_show_window_menu(top, seat, 0, 0, 0);
		wl_display_roundtrip(display);
		struct xdg_surface *popup = NULL;
		if (!strcmp(mode, "nested"))
			popup = make_popup(xdg);
		xdg_toplevel_destroy(top);
		wl_display_roundtrip(display);
		if (!strcmp(mode, "popup"))
			make_popup(xdg);
		else if (!strcmp(mode, "nested"))
			make_popup(popup);
		printf("done\n");
	}
	while (wl_display_dispatch(display) != -1) {
	}
	return 0;
}
