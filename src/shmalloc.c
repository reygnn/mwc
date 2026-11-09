/*
 * shmalloc.c - a shared memory allocator for wlroots before 0.19.3
 *
 * The shm allocator of wlroots 0.19.2 frees a buffer without sending its destroy
 * signal (fixed upstream in 16cb509a, "render/allocator: add missing
 * wlr_buffer_finish() in destroy impls", in 0.19.3). The pixman renderer and the
 * scene's damage rings keep an entry for every buffer they saw and drop it on
 * that signal, so each buffer freed by a mode change or an unplugged output
 * leaves one behind. When they are torn down at exit, they write into the freed
 * buffers, and glibc aborts with "corrupted size vs. prev_size". This is the
 * same allocator with the signal, used where wlroots would pick its own.
 *
 * The memory comes from memfd_create() and is reserved up front: a buffer
 * larger than there is room for then fails to allocate, and the commit asking
 * for it fails, where the wlroots allocator kills mwc with SIGBUS when the
 * buffer is first touched.
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
	/* What wlroots 0.19.2 forgets: whoever keeps track of the buffer lets go. */
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
	struct shm_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;
	wlr_buffer_init(&buffer->base, &buffer_impl, width, height);
	int stride = width * bpp;
	buffer->size = (size_t)stride * height;
	buffer->shm.fd = memfd_create("mwc-shm", MFD_CLOEXEC);
	if (buffer->shm.fd < 0) {
		wlr_log_errno(WLR_ERROR, "shm allocator: memfd_create");
		free(buffer);
		return NULL;
	}
	int err = posix_fallocate(buffer->shm.fd, 0, buffer->size);
	if (err) {
		errno = err;
		wlr_log_errno(WLR_ERROR, "shm allocator: no room for %dx%d", width, height);
		close(buffer->shm.fd);
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
