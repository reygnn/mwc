/*
 * A Wayland client for the tests: a solid toplevel with server-side decorations,
 * with a minimum and maximum size if asked, and optionally a dialog belonging
 * to it. It takes whatever size it is configured to.
 *
 *   xdgc key=value ...
 *     app=NAME     app_id, required
 *     size=WxH     default 300x200
 *     min=WxH max=WxH
 *     pre=max|full ask for that state before the first commit
 *     dialog=NAME  also open a child toplevel with this app_id
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-decoration-unstable-v1-client.h"
#include "xdg-shell-client.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct zxdg_decoration_manager_v1 *deco_manager;
static int default_w = 300, default_h = 200, min_w, min_h, max_w, max_h;
static int open_windows;
static const char *pre_state;

struct window {
	struct wl_surface *surface;
	struct xdg_surface *xdg;
	struct xdg_toplevel *toplevel;
	int w, h;
	int natural_w, natural_h; /* what it takes when the choice is its own */
};

static struct wl_display *the_display;
#define wl_proxy_get_display_of(p) the_display

static void draw(struct window *win)
{
	size_t size = win->w * win->h * 4;
	int fd = memfd_create("xdgc", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) != 0) {
		perror("xdgc: buffer");
		exit(1);
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (size_t i = 0; i < size / 4; i++)
		px[i] = 0xff808080;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, win->w, win->h,
		win->w * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(win->surface, buffer, 0, 0);
	wl_surface_damage_buffer(win->surface, 0, 0, win->w, win->h);
	wl_surface_commit(win->surface);
}

static void xdg_configure(void *data, struct xdg_surface *xdg, uint32_t serial)
{
	xdg_surface_ack_configure(xdg, serial);
	draw(data);
}

static const struct xdg_surface_listener xdg_listener = {
	.configure = xdg_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w,
	int32_t h, struct wl_array *states)
{
	struct window *win = data;
	win->w = w > 0 ? w : win->natural_w;
	win->h = h > 0 ? h : win->natural_h;
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
	struct window *win = data;
	xdg_toplevel_destroy(win->toplevel);
	xdg_surface_destroy(win->xdg);
	wl_surface_destroy(win->surface);
	open_windows--;
}

static void toplevel_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h)
{
}

static void toplevel_caps(void *data, struct xdg_toplevel *t, struct wl_array *c)
{
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_bounds,
	.wm_capabilities = toplevel_caps,
};

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { .ping = ping };

static void registry_global(void *data, struct wl_registry *registry,
	uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0)
		compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	else if (strcmp(interface, wl_shm_interface.name) == 0)
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (strcmp(interface,
			zxdg_decoration_manager_v1_interface.name) == 0)
		deco_manager = wl_registry_bind(registry, name,
			&zxdg_decoration_manager_v1_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *registry,
	uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static struct window *open_window(const char *app, struct window *parent,
	int w, int h, bool limits)
{
	struct window *win = calloc(1, sizeof(*win));
	win->w = win->natural_w = w;
	win->h = win->natural_h = h;
	win->surface = wl_compositor_create_surface(compositor);
	win->xdg = xdg_wm_base_get_xdg_surface(wm_base, win->surface);
	xdg_surface_add_listener(win->xdg, &xdg_listener, win);
	win->toplevel = xdg_surface_get_toplevel(win->xdg);
	xdg_toplevel_add_listener(win->toplevel, &toplevel_listener, win);
	xdg_toplevel_set_app_id(win->toplevel, app);
	xdg_toplevel_set_title(win->toplevel, app);
	if (parent)
		xdg_toplevel_set_parent(win->toplevel, parent->toplevel);
	if (limits) {
		xdg_toplevel_set_min_size(win->toplevel, min_w, min_h);
		xdg_toplevel_set_max_size(win->toplevel, max_w, max_h);
	}
	struct zxdg_toplevel_decoration_v1 *deco =
		zxdg_decoration_manager_v1_get_toplevel_decoration(deco_manager,
		win->toplevel);
	zxdg_toplevel_decoration_v1_set_mode(deco,
		ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	if (limits && pre_state && !strcmp(pre_state, "max"))
		xdg_toplevel_set_maximized(win->toplevel);
	else if (limits && pre_state && !strcmp(pre_state, "full"))
		xdg_toplevel_set_fullscreen(win->toplevel, NULL);
	if (pre_state)
		wl_display_roundtrip(wl_proxy_get_display_of(win->toplevel));
	wl_surface_commit(win->surface);
	open_windows++;
	return win;
}

int main(int argc, char *argv[])
{
	const char *app = NULL, *dialog = NULL;
	for (int i = 1; i < argc; i++) {
		char *v = strchr(argv[i], '=');
		if (!v) {
			fprintf(stderr, "xdgc: expected key=value: %s\n", argv[i]);
			return 2;
		}
		*v++ = '\0';
		if (!strcmp(argv[i], "app"))
			app = v;
		else if (!strcmp(argv[i], "size"))
			sscanf(v, "%dx%d", &default_w, &default_h);
		else if (!strcmp(argv[i], "min"))
			sscanf(v, "%dx%d", &min_w, &min_h);
		else if (!strcmp(argv[i], "max"))
			sscanf(v, "%dx%d", &max_w, &max_h);
		else if (!strcmp(argv[i], "dialog"))
			dialog = v;
		else if (!strcmp(argv[i], "pre"))
			pre_state = v;
	}
	if (!app) {
		fprintf(stderr, "xdgc: app= is required\n");
		return 2;
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "xdgc: cannot connect to the compositor\n");
		return 1;
	}
	the_display = display;
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm_base || !deco_manager) {
		fprintf(stderr, "xdgc: the compositor lacks a needed global\n");
		return 1;
	}
	struct window *main_win = open_window(app, NULL, default_w, default_h, true);
	if (dialog) {
		wl_display_roundtrip(display);
		wl_display_roundtrip(display);
		open_window(dialog, main_win, 120, 80, false);
	}
	while (open_windows > 0 && wl_display_dispatch(display) != -1) {
	}
	return 0;
}
