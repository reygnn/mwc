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
			&wl_data_device_manager_interface, 1);
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

static void offer_mime(void *data, struct wl_data_offer *o, const char *type)
{
	if (!mime || !strcmp(type, "text/plain;charset=utf-8")) {
		free(mime);
		mime = strdup(type);
	}
}

static const struct wl_data_offer_listener offer_listener = { .offer = offer_mime };

static void dd_offer(void *data, struct wl_data_device *dd, struct wl_data_offer *o)
{
	free(mime);
	mime = NULL;
	wl_data_offer_add_listener(o, &offer_listener, NULL);
}

static void dd_enter(void *data, struct wl_data_device *dd, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o)
{
}

static void dd_leave(void *data, struct wl_data_device *dd)
{
}

static void dd_motion(void *data, struct wl_data_device *dd, uint32_t t, wl_fixed_t x,
	wl_fixed_t y)
{
}

static void dd_drop(void *data, struct wl_data_device *dd)
{
}

static void dd_selection(void *data, struct wl_data_device *dd, struct wl_data_offer *o)
{
	if (!o || !mime)
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
	if (argc != 2) {
		fprintf(stderr, "usage: badclient MODE\n");
		return 2;
	}
	const char *mode = argv[1];
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

	struct wl_surface *surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg, &xdg_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xdg);
	xdg_toplevel_add_listener(top, &top_listener, NULL);
	xdg_toplevel_set_app_id(top, "bad");
	wl_surface_commit(surface);
	wait_configure();
	xdg_surface_ack_configure(xdg, configure_serial);
	wl_display_roundtrip(display);

	if (!strcmp(mode, "remap")) {
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
