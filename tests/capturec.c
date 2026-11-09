/*
 * Captures an output through ext-image-copy-capture, as a VNC or RDP server does.
 *
 *   capturec frame NAME OUT.png [cursors]   one frame of the output, with the pointer
 *                                           painted in only if asked
 *   capturec cursor NAME OUT.png            the pointer's own image, as a viewer gets
 *                                           it to draw it itself
 *
 * cursor prints: cursor x=X y=Y hotspot=HX,HY w=W h=H opaque=N, with the position on
 * the output and N the pixels that aren't fully transparent. Exits 0 on success, 1 if
 * the capture failed or nothing came within five seconds, 2 on a usage error.
 */
#define _GNU_SOURCE
#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ext-image-capture-source-v1-client.h"
#include "ext-image-copy-capture-v1-client.h"

#define MAX_OUTPUTS 8

struct output {
	struct wl_output *output;
	char name[64];
};

static struct wl_shm *shm;
static struct wl_seat *seat;
static struct ext_output_image_capture_source_manager_v1 *source_manager;
static struct ext_image_copy_capture_manager_v1 *capture_manager;
static struct output outputs[MAX_OUTPUTS];
static int n_outputs;

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
	int32_t pw, int32_t ph, int32_t subpixel, const char *make, const char *model,
	int32_t transform)
{
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w,
	int32_t h, int32_t refresh)
{
}

static void output_done(void *data, struct wl_output *o)
{
}

static void output_scale(void *data, struct wl_output *o, int32_t factor)
{
}

static void output_name(void *data, struct wl_output *o, const char *name)
{
	struct output *output = data;
	snprintf(output->name, sizeof(output->name), "%s", name);
}

static void output_description(void *data, struct wl_output *o, const char *d)
{
}

static const struct wl_output_listener output_listener = {
	output_geometry, output_mode, output_done, output_scale, output_name,
	output_description,
};

static void global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	} else if (!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
		source_manager = wl_registry_bind(registry, name,
			&ext_output_image_capture_source_manager_v1_interface, 1);
	} else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
		capture_manager = wl_registry_bind(registry, name,
			&ext_image_copy_capture_manager_v1_interface, 1);
	} else if (!strcmp(interface, wl_output_interface.name) && version >= 4 &&
			n_outputs < MAX_OUTPUTS) {
		struct output *o = &outputs[n_outputs++];
		o->output = wl_registry_bind(registry, name, &wl_output_interface, 4);
		wl_output_add_listener(o->output, &output_listener, o);
	}
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

/* The session tells the size and formats, then the frame comes. */
static uint32_t buf_w, buf_h;
static int format = -1;
static bool constraints_done, stopped, frame_ready, frame_failed;

static void session_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *s,
	uint32_t w, uint32_t h)
{
	buf_w = w;
	buf_h = h;
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *s,
	uint32_t f)
{
	/* With alpha first: the pointer's image needs it. */
	if (f == WL_SHM_FORMAT_ARGB8888 || (f == WL_SHM_FORMAT_XRGB8888 && format < 0))
		format = f;
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *s,
	struct wl_array *device)
{
}

static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *s,
	uint32_t f, struct wl_array *modifiers)
{
}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *s)
{
	constraints_done = true;
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *s)
{
	stopped = true;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	session_buffer_size, session_shm_format, session_dmabuf_device,
	session_dmabuf_format, session_done, session_stopped,
};

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *f,
	uint32_t transform)
{
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *f,
	int32_t x, int32_t y, int32_t w, int32_t h)
{
}

static void frame_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *f,
	uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
}

static void frame_ready_cb(void *data, struct ext_image_copy_capture_frame_v1 *f)
{
	frame_ready = true;
}

static void frame_failed_cb(void *data, struct ext_image_copy_capture_frame_v1 *f,
	uint32_t reason)
{
	fprintf(stderr, "capturec: the frame failed, reason %u\n", reason);
	frame_failed = true;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	frame_transform, frame_damage, frame_presentation_time, frame_ready_cb,
	frame_failed_cb,
};

static bool entered;
static int cursor_x, cursor_y, hotspot_x, hotspot_y;

static void cursor_enter(void *data, struct ext_image_copy_capture_cursor_session_v1 *c)
{
	entered = true;
}

static void cursor_leave(void *data, struct ext_image_copy_capture_cursor_session_v1 *c)
{
	entered = false;
}

static void cursor_position(void *data, struct ext_image_copy_capture_cursor_session_v1 *c,
	int32_t x, int32_t y)
{
	cursor_x = x;
	cursor_y = y;
}

static void cursor_hotspot(void *data, struct ext_image_copy_capture_cursor_session_v1 *c,
	int32_t x, int32_t y)
{
	hotspot_x = x;
	hotspot_y = y;
}

static const struct ext_image_copy_capture_cursor_session_v1_listener cursor_listener = {
	cursor_enter, cursor_leave, cursor_position, cursor_hotspot,
};

static int usage(void)
{
	fprintf(stderr, "usage: capturec frame NAME OUT.png [cursors]\n"
		"       capturec cursor NAME OUT.png\n");
	return 2;
}

static bool dispatch_until(struct wl_display *display, bool *flag)
{
	while (!*flag && !stopped && !frame_failed) {
		if (wl_display_dispatch(display) == -1)
			return false;
	}
	return *flag;
}

int main(int argc, char **argv)
{
	if (argc < 4)
		return usage();
	bool cursor = !strcmp(argv[1], "cursor");
	if (!cursor && strcmp(argv[1], "frame"))
		return usage();
	bool paint_cursors = !cursor && argc == 5 && !strcmp(argv[4], "cursors");
	if (argc != 4 && !paint_cursors)
		return usage();
	alarm(5);

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "capturec: can't connect\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);
	if (!shm || !source_manager || !capture_manager || (cursor && !seat)) {
		fprintf(stderr, "capturec: ext-image-copy-capture isn't offered\n");
		return 1;
	}
	struct output *output = NULL;
	for (int i = 0; i < n_outputs; i++) {
		if (!strcmp(outputs[i].name, argv[2]))
			output = &outputs[i];
	}
	if (!output) {
		fprintf(stderr, "capturec: no output %s\n", argv[2]);
		return 1;
	}

	struct ext_image_capture_source_v1 *source =
		ext_output_image_capture_source_manager_v1_create_source(source_manager,
			output->output);
	struct ext_image_copy_capture_session_v1 *session;
	struct ext_image_copy_capture_cursor_session_v1 *cursor_session = NULL;
	if (cursor) {
		struct wl_pointer *pointer = wl_seat_get_pointer(seat);
		cursor_session = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
			capture_manager, source, pointer);
		ext_image_copy_capture_cursor_session_v1_add_listener(cursor_session,
			&cursor_listener, NULL);
		session = ext_image_copy_capture_cursor_session_v1_get_capture_session(
			cursor_session);
	} else {
		session = ext_image_copy_capture_manager_v1_create_session(capture_manager,
			source, paint_cursors ?
			EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0);
	}
	ext_image_copy_capture_session_v1_add_listener(session, &session_listener, NULL);
	if (!dispatch_until(display, &constraints_done)) {
		fprintf(stderr, "capturec: the session stopped before it began\n");
		return 1;
	}
	if (format < 0 || !buf_w || !buf_h) {
		fprintf(stderr, "capturec: no usable shm format or size\n");
		return 1;
	}

	int stride = buf_w * 4;
	size_t size = (size_t)stride * buf_h;
	int fd = memfd_create("capturec", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		perror("capturec: memfd");
		return 1;
	}
	void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		perror("capturec: mmap");
		return 1;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, buf_w, buf_h, stride,
		format);
	wl_shm_pool_destroy(pool);

	struct ext_image_copy_capture_frame_v1 *frame =
		ext_image_copy_capture_session_v1_create_frame(session);
	ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, NULL);
	ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, buf_w, buf_h);
	ext_image_copy_capture_frame_v1_capture(frame);
	if (!dispatch_until(display, &frame_ready)) {
		fprintf(stderr, "capturec: no frame\n");
		return 1;
	}
	wl_display_roundtrip(display);

	cairo_surface_t *s = cairo_image_surface_create_for_data(pixels,
		format == WL_SHM_FORMAT_ARGB8888 ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
		buf_w, buf_h, stride);
	cairo_status_t status = cairo_surface_write_to_png(s, argv[3]);
	cairo_surface_destroy(s);
	if (status != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "capturec: can't write %s\n", argv[3]);
		return 1;
	}
	if (cursor) {
		int opaque = 0;
		const uint32_t *p = pixels;
		for (size_t i = 0; i < (size_t)buf_w * buf_h; i++)
			opaque += format == WL_SHM_FORMAT_ARGB8888 ? (p[i] >> 24) != 0 : 1;
		if (!entered)
			printf("cursor none\n");
		else
			printf("cursor x=%d y=%d hotspot=%d,%d w=%u h=%u opaque=%d\n", cursor_x,
				cursor_y, hotspot_x, hotspot_y, buf_w, buf_h, opaque);
	}
	ext_image_copy_capture_frame_v1_destroy(frame);
	ext_image_copy_capture_session_v1_destroy(session);
	if (cursor_session)
		ext_image_copy_capture_cursor_session_v1_destroy(cursor_session);
	ext_image_capture_source_v1_destroy(source);
	wl_buffer_destroy(buffer);
	wl_display_disconnect(display);
	return 0;
}
