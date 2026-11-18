/*
 * A window that uses wp_fifo_v1, as SDL 3 and Mesa's FIFO swap do.
 *
 *   fifoc fifo N      N updates at once, each with set_barrier and wait_barrier
 *   fifoc plain N     the same without the protocol, for comparison
 *   fifoc release N   N updates with the barrier, then the wp_fifo_v1 goes
 *   fifoc quit N      N updates with the barrier, then exit while they are held
 *   fifoc hold N      fullscreen, N updates with the barrier, print "held" and stay
 *   fifoc minimized N minimized, N updates with the barrier
 *   fifoc unseen N    the same on the monitor as it is, for one switched off
 *   fifoc twice       a second wp_fifo_v1 for the same surface
 *   fifoc gone        set_barrier after the surface was destroyed
 *
 * The first three send all updates without waiting, each with a frame callback, and
 * print: refreshes=R, how many refresh periods of the output passed from the first
 * update until the last callback came. Held back by the barrier that is about N-1;
 * without it, the updates replace each other and it is one or two.
 * Off screen there are no frame callbacks: minimized and unseen go by the release of
 * each update's own buffer, which comes once the update is applied, or the next one,
 * and print seconds=S, from the first update until the buffer of the one before last
 * came back. Held back by the barrier, about a second each, that is N-3 to N-2.
 * twice and gone print the protocol error they get: error=wp_fifo_manager_v1:0 or
 * error=wp_fifo_v1:0. Exits 0 on success, 1 on failure, 2 on a usage error.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "fifo-v1-client.h"
#include "xdg-shell-client.h"

#define SIZE 64

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_fifo_manager_v1 *fifo_manager;
static struct wl_output *output;
static int32_t refresh_mhz;
static bool configured;
static int callbacks_done;
static double last_done;
static int outputs_entered;
static double released[64]; /* when each update's buffer came back, 0 before */

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
	int32_t pw, int32_t ph, int32_t subpixel, const char *make, const char *model,
	int32_t transform)
{
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w,
	int32_t h, int32_t refresh)
{
	if (flags & WL_OUTPUT_MODE_CURRENT)
		refresh_mhz = refresh;
}

static void output_done(void *data, struct wl_output *o)
{
}

static void output_scale(void *data, struct wl_output *o, int32_t factor)
{
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name))
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	else if (!strcmp(interface, wl_shm_interface.name))
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	} else if (!strcmp(interface, wp_fifo_manager_v1_interface.name))
		fifo_manager = wl_registry_bind(registry, name,
			&wp_fifo_manager_v1_interface, 1);
	else if (!strcmp(interface, wl_output_interface.name) && !output) {
		output = wl_registry_bind(registry, name, &wl_output_interface, 2);
		wl_output_add_listener(output, &output_listener, NULL);
	}
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static void surface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	configured = true;
}

static const struct xdg_surface_listener surface_listener = { surface_configure };

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
	struct wl_array *states)
{
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	wl_callback_destroy(cb);
	callbacks_done++;
	last_done = now_ms();
}

static const struct wl_callback_listener frame_listener = { frame_done };

static void surface_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
	outputs_entered++;
}

static void surface_leave(void *data, struct wl_surface *s, struct wl_output *o)
{
	outputs_entered--;
}

static const struct wl_surface_listener wl_surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
};

static void buffer_release(void *data, struct wl_buffer *buffer)
{
	double *when = data;
	if (!*when)
		*when = now_ms();
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

static struct wl_buffer *make_buffer(void)
{
	int stride = SIZE * 4, size = stride * SIZE;
	int fd = memfd_create("fifoc", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0)
		return NULL;
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED)
		return NULL;
	for (int i = 0; i < SIZE * SIZE; i++)
		pixels[i] = 0xff336699;
	munmap(pixels, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, SIZE, SIZE, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

/* N updates with the barrier while nobody sees the window; see the top. */
static int off_screen_updates(struct wl_display *display, struct wl_surface *surface,
	struct xdg_toplevel *toplevel, bool minimize, int n)
{
	if (minimize) {
		xdg_toplevel_set_minimized(toplevel);
		wl_surface_commit(surface);
		while (outputs_entered > 0)
			if (wl_display_dispatch(display) < 0)
				return 1;
	}
	struct wl_buffer *buffers[64];
	for (int i = 0; i < n; i++) {
		buffers[i] = make_buffer();
		if (!buffers[i])
			return 1;
		wl_buffer_add_listener(buffers[i], &buffer_listener, &released[i]);
	}
	struct wp_fifo_v1 *fifo = wp_fifo_manager_v1_get_fifo(fifo_manager, surface);
	double start = now_ms();
	for (int i = 0; i < n; i++) {
		wp_fifo_v1_set_barrier(fifo);
		wp_fifo_v1_wait_barrier(fifo);
		wl_surface_attach(surface, buffers[i], 0, 0);
		wl_surface_damage_buffer(surface, 0, 0, SIZE, SIZE);
		wl_surface_commit(surface);
	}
	wl_display_flush(display);
	while (!released[n - 2])
		if (wl_display_dispatch(display) < 0)
			return 1;
	printf("seconds=%.2f\n", (released[n - 2] - start) / 1000);
	return 0;
}

static int usage(void)
{
	fprintf(stderr, "usage: fifoc fifo|plain|release|quit|hold|minimized|unseen N | "
		"twice | gone\n");
	return 2;
}

/* Prints the protocol error the connection died with, if any. */
static int report_error(struct wl_display *display)
{
	const struct wl_interface *iface = NULL;
	uint32_t id;
	if (wl_display_get_error(display) != EPROTO) {
		fprintf(stderr, "fifoc: no protocol error\n");
		return 1;
	}
	uint32_t code = wl_display_get_protocol_error(display, &iface, &id);
	printf("error=%s:%u\n", iface ? iface->name : "?", code);
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc < 2)
		return usage();
	const char *mode = argv[1];
	int n = argc > 2 ? atoi(argv[2]) : 0;
	bool off_screen = !strcmp(mode, "minimized") || !strcmp(mode, "unseen");
	bool updates = !strcmp(mode, "fifo") || !strcmp(mode, "plain") ||
		!strcmp(mode, "release") || !strcmp(mode, "quit") || !strcmp(mode, "hold") ||
		off_screen;
	if (updates ? n < 2 : strcmp(mode, "twice") && strcmp(mode, "gone"))
		return usage();
	if (off_screen && n > (int)(sizeof(released) / sizeof(*released)))
		return usage();

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "fifoc: no Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm_base || !output) {
		fprintf(stderr, "fifoc: compositor, shm, xdg_wm_base or an output missing\n");
		return 1;
	}
	if (!fifo_manager) {
		fprintf(stderr, "fifoc: no wp_fifo_manager_v1\n");
		return 1;
	}

	struct wl_surface *surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(surface, &wl_surface_listener, NULL);

	if (!strcmp(mode, "twice")) {
		wp_fifo_manager_v1_get_fifo(fifo_manager, surface);
		wp_fifo_manager_v1_get_fifo(fifo_manager, surface);
		wl_display_roundtrip(display);
		return report_error(display);
	}
	if (!strcmp(mode, "gone")) {
		struct wp_fifo_v1 *fifo = wp_fifo_manager_v1_get_fifo(fifo_manager, surface);
		wl_surface_destroy(surface);
		wp_fifo_v1_set_barrier(fifo);
		wl_display_roundtrip(display);
		return report_error(display);
	}

	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &surface_listener, NULL);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "fifoc");
	xdg_toplevel_set_app_id(toplevel, "fifoc");
	if (!strcmp(mode, "hold"))
		xdg_toplevel_set_fullscreen(toplevel, NULL);
	wl_surface_commit(surface);
	while (!configured)
		if (wl_display_dispatch(display) < 0)
			return 1;

	struct wl_buffer *buffer = make_buffer();
	if (!buffer)
		return 1;
	/* Map the window first and let it settle, so the updates below find it in
	 * the scene and on the output. */
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, SIZE, SIZE);
	wl_callback_add_listener(wl_surface_frame(surface), &frame_listener, NULL);
	wl_surface_commit(surface);
	/* On a monitor that is off nothing is drawn, nor called back. */
	while (callbacks_done < 1 && strcmp(mode, "unseen"))
		if (wl_display_dispatch(display) < 0)
			return 1;
	callbacks_done = 0;
	usleep(200 * 1000);
	if (off_screen)
		return off_screen_updates(display, surface, toplevel, !strcmp(mode, "minimized"),
			n);

	struct wp_fifo_v1 *fifo = NULL;
	if (strcmp(mode, "plain"))
		fifo = wp_fifo_manager_v1_get_fifo(fifo_manager, surface);
	double start = now_ms();
	for (int i = 0; i < n; i++) {
		if (fifo) {
			wp_fifo_v1_set_barrier(fifo);
			wp_fifo_v1_wait_barrier(fifo);
		}
		wl_surface_attach(surface, buffer, 0, 0);
		wl_surface_damage_buffer(surface, 0, 0, SIZE, SIZE);
		wl_callback_add_listener(wl_surface_frame(surface), &frame_listener, NULL);
		wl_surface_commit(surface);
	}
	if (!strcmp(mode, "quit")) {
		wl_display_roundtrip(display);
		printf("quit with %d of %d callbacks\n", callbacks_done, n);
		wl_display_disconnect(display);
		return 0;
	}
	if (!strcmp(mode, "hold")) {
		wl_display_roundtrip(display);
		printf("held\n");
		fflush(stdout);
		while (wl_display_dispatch(display) >= 0)
			;
		return 0;
	}
	if (!strcmp(mode, "release")) {
		wl_display_roundtrip(display);
		wp_fifo_v1_destroy(fifo);
	}
	wl_display_flush(display);
	while (callbacks_done < n)
		if (wl_display_dispatch(display) < 0) {
			fprintf(stderr, "fifoc: connection lost after %d callbacks\n",
				callbacks_done);
			return 1;
		}

	double period = 1000000.0 / (refresh_mhz > 0 ? refresh_mhz : 60000);
	printf("refreshes=%.1f (%.1f ms, refresh %.2f Hz)\n", (last_done - start) / period,
		last_done - start, 1000.0 / period);
	wl_display_disconnect(display);
	return 0;
}
