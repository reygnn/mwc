/*
 * shmalloc.c - mwc's shared memory allocator, used where wlroots would pick its own
 *
 * The memory comes from memfd_create(), not /dev/shm: wlroots' allocator puts its
 * buffers there with shm_open() and ftruncate() (still in 0.19.3 and 0.20.2), and
 * one larger than what is free in it kills mwc with SIGBUS when it is first
 * touched. Docker and Podman give /dev/shm 64 MiB by default, room for barely two
 * buffers of a 4K headless output for a VNC or RDP server, shared with anything
 * else that uses shm_open(). A memfd is only limited by memory (with a container's
 * memory limit); posix_fallocate() takes the pages up front, so where there isn't
 * enough the buffer fails at once, or the OOM killer acts then, rather than at the
 * first frame.
 *
 * A buffer's destroy sends its destroy signal through wlr_buffer_finish(), as
 * every allocator must: the pixman renderer and the scene's damage rings drop
 * their entry for it on that signal. wlroots' own shm allocator forgot it in
 * 0.19.2, which left freed buffers behind for glibc to abort on at exit; fixed in
 * 0.19.3 (4279ee5, "render/allocator: add missing wlr_buffer_finish() in destroy
 * impls").
 *
 * wlroots' udmabuf allocator lacks the signal in 0.19.2 as well and isn't replaced
 * here; wlroots only takes it for a renderer that wants DMA-BUFs without a DRM
 * device, which mwc's usual setups (pixman headless, a GPU with DRM) never are.
 */
#define _GNU_SOURCE
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/util/log.h>
#include "mwc.h"

struct shm_buffer {
	struct wlr_buffer base;
	struct wlr_shm_attributes shm;
	void *data;
	size_t size;
};

/* The formats the pixman renderer draws into, the only one that takes shm. */
static int bytes_per_pixel(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_BGR565:
		return 2;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		return 4;
	default:
		return 0;
	}
}

static struct shm_buffer *shm_buffer(struct wlr_buffer *wlr_buffer)
{
	struct shm_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	return buffer;
}

static void buffer_destroy(struct wlr_buffer *wlr_buffer)
{
	struct shm_buffer *buffer = shm_buffer(wlr_buffer);
	/* Whoever keeps track of the buffer lets go. */
	wlr_buffer_finish(wlr_buffer);
	munmap(buffer->data, buffer->size);
	close(buffer->shm.fd);
	free(buffer);
}

static bool buffer_get_shm(struct wlr_buffer *wlr_buffer, struct wlr_shm_attributes *shm)
{
	*shm = shm_buffer(wlr_buffer)->shm;
	return true;
}

static bool buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
	void **data, uint32_t *format, size_t *stride)
{
	struct shm_buffer *buffer = shm_buffer(wlr_buffer);
	*data = buffer->data;
	*format = buffer->shm.format;
	*stride = buffer->shm.stride;
	return true;
}

static void buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_shm = buffer_get_shm,
	.begin_data_ptr_access = buffer_begin_data_ptr_access,
	.end_data_ptr_access = buffer_end_data_ptr_access,
};

static struct wlr_buffer *allocator_create_buffer(struct wlr_allocator *allocator,
	int width, int height, const struct wlr_drm_format *format)
{
	int bpp = bytes_per_pixel(format->format);
	if (!bpp) {
		wlr_log(WLR_ERROR, "shm allocator: unsupported format 0x%08x", format->format);
		return NULL;
	}
	if (width <= 0 || height <= 0 || width > INT32_MAX / bpp)
		return NULL;
	int stride = width * bpp;
	if ((size_t)height > SIZE_MAX / (size_t)stride)
		return NULL;
	struct shm_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;
	wlr_buffer_init(&buffer->base, &buffer_impl, width, height);
	buffer->size = (size_t)stride * height;
	buffer->shm.fd = memfd_create("mwc-shm", MFD_CLOEXEC);
	if (buffer->shm.fd < 0) {
		wlr_log_errno(WLR_ERROR, "shm allocator: memfd_create");
		wlr_buffer_finish(&buffer->base);
		free(buffer);
		return NULL;
	}
	int err = posix_fallocate(buffer->shm.fd, 0, buffer->size);
	if (err) {
		errno = err;
		wlr_log_errno(WLR_ERROR, "shm allocator: no room for %dx%d", width, height);
		close(buffer->shm.fd);
		wlr_buffer_finish(&buffer->base);
		free(buffer);
		return NULL;
	}
	buffer->shm.format = format->format;
	buffer->shm.width = width;
	buffer->shm.height = height;
	buffer->shm.stride = stride;
	buffer->shm.offset = 0;
	buffer->data = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		buffer->shm.fd, 0);
	if (buffer->data == MAP_FAILED) {
		wlr_log_errno(WLR_ERROR, "shm allocator: mmap");
		close(buffer->shm.fd);
		wlr_buffer_finish(&buffer->base);
		free(buffer);
		return NULL;
	}
	return &buffer->base;
}

static void allocator_destroy(struct wlr_allocator *allocator)
{
	free(allocator);
}

static const struct wlr_allocator_interface allocator_impl = {
	.create_buffer = allocator_create_buffer,
	.destroy = allocator_destroy,
};

struct wlr_allocator *shm_allocator_create(void)
{
	struct wlr_allocator *allocator = calloc(1, sizeof(*allocator));
	if (!allocator)
		return NULL;
	wlr_allocator_init(allocator, &allocator_impl,
		WLR_BUFFER_CAP_DATA_PTR | WLR_BUFFER_CAP_SHM);
	return allocator;
}
