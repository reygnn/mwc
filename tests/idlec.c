/*
 * Idle for the tests: watches for it as a screen locker does, or keeps it away as a
 * video player does.
 *
 *   idlec watch MS          an ext-idle-notify-v1 notification with that timeout;
 *                           prints "idle" and "resumed" as they come
 *   idlec inhibit           a mapped 100x100 window with an idle inhibitor
 *   idlec inhibit-unmapped  an inhibitor on a surface that never appears
 *
 * The inhibiting ones print "inhibiting" once set up. All run until killed. Exits 1
 * if a global is missing, 2 on a usage error.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ext-idle-notify-v1-client.h"
#include "idle-inhibit-unstable-v1-client.h"
#include "xdg-shell-client.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct ext_idle_notifier_v1 *notifier;
static struct zwp_idle_inhibit_manager_v1 *inhibit_manager;
static bool configured;

static void global(void *data, struct wl_registry *r, uint32_t name,
	const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name))
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(interface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, wl_seat_interface.name) && !seat)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(interface, xdg_wm_base_interface.name))
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	else if (!strcmp(interface, ext_idle_notifier_v1_interface.name))
		notifier = wl_registry_bind(r, name, &ext_idle_notifier_v1_interface, 1);
	else if (!strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name))
		inhibit_manager = wl_registry_bind(r, name,
			&zwp_idle_inhibit_manager_v1_interface, 1);
}

static void global_remove(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static void idled(void *data, struct ext_idle_notification_v1 *n)
{
	printf("idle\n");
	fflush(stdout);
}

static void resumed(void *data, struct ext_idle_notification_v1 *n)
{
	printf("resumed\n");
	fflush(stdout);
}

static const struct ext_idle_notification_v1_listener notification_listener = {
	idled, resumed,
};

static void ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { ping };

static void surface_configure(void *data, struct xdg_surface *x, uint32_t serial)
{
	xdg_surface_ack_configure(x, serial);
	configured = true;
}

static const struct xdg_surface_listener surface_listener = { surface_configure };

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
	struct wl_array *states)
{
}

static void top_close(void *data, struct xdg_toplevel *t)
{
}

static const struct xdg_toplevel_listener top_listener = {
	.configure = top_configure,
	.close = top_close,
};

static struct wl_buffer *grey_buffer(int w, int h)
{
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("idlec", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) != 0)
		return NULL;
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED)
		return NULL;
	for (int i = 0; i < w * h; i++)
		px[i] = 0xff606060;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

int main(int argc, char *argv[])
{
	if (argc < 2 || (!strcmp(argv[1], "watch") && argc < 3)) {
		fprintf(stderr, "usage: idlec watch MS | inhibit | inhibit-unmapped\n");
		return 2;
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "idlec: cannot connect\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!strcmp(argv[1], "watch")) {
		if (!notifier || !seat) {
			fprintf(stderr, "idlec: no ext_idle_notifier_v1\n");
			return 1;
		}
		struct ext_idle_notification_v1 *n = ext_idle_notifier_v1_get_idle_notification(
			notifier, atoi(argv[2]), seat);
		ext_idle_notification_v1_add_listener(n, &notification_listener, NULL);
	} else if (!strcmp(argv[1], "inhibit") || !strcmp(argv[1], "inhibit-unmapped")) {
		if (!inhibit_manager || !compositor || !shm || !wm_base) {
			fprintf(stderr, "idlec: a global is missing\n");
			return 1;
		}
		struct wl_surface *surface = wl_compositor_create_surface(compositor);
		if (!strcmp(argv[1], "inhibit")) {
			xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
			struct xdg_surface *x = xdg_wm_base_get_xdg_surface(wm_base, surface);
			xdg_surface_add_listener(x, &surface_listener, NULL);
			struct xdg_toplevel *t = xdg_surface_get_toplevel(x);
			xdg_toplevel_add_listener(t, &top_listener, NULL);
			xdg_toplevel_set_app_id(t, "idlec");
			wl_surface_commit(surface);
			while (!configured && wl_display_dispatch(display) != -1) {
			}
			wl_surface_attach(surface, grey_buffer(100, 100), 0, 0);
			wl_surface_commit(surface);
		}
		zwp_idle_inhibit_manager_v1_create_inhibitor(inhibit_manager, surface);
		wl_display_roundtrip(display);
		printf("inhibiting\n");
		fflush(stdout);
	} else {
		fprintf(stderr, "idlec: unknown mode %s\n", argv[1]);
		return 2;
	}
	while (wl_display_dispatch(display) != -1) {
	}
	return 0;
}
