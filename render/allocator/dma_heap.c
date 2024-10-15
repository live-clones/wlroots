#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/util/log.h>

#include "render/drm_format_set.h"
#include "render/pixel_format.h"
#include "render/allocator/dma_heap.h"

static const struct wlr_buffer_impl buffer_impl;
static const struct wlr_allocator_interface allocator_impl;

static struct wlr_dma_heap_buffer *dma_heap_buffer_from_buffer(
		struct wlr_buffer *wlr_buffer) {
	assert(wlr_buffer->impl == &buffer_impl);
	struct wlr_dma_heap_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	return buffer;
}

static struct wlr_dma_heap_allocator *dma_heap_allocator_from_allocator(
		struct wlr_allocator *wlr_allocator) {
	assert(wlr_allocator->impl == &allocator_impl);
	struct wlr_dma_heap_allocator *alloc = wl_container_of(wlr_allocator, alloc, base);
	return alloc;
}

static void buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct wlr_dma_heap_buffer *buffer = dma_heap_buffer_from_buffer(wlr_buffer);
	wlr_dmabuf_attributes_finish(&buffer->dmabuf);
	free(buffer);
}

static bool buffer_get_dmabuf(struct wlr_buffer *wlr_buffer,
		struct wlr_dmabuf_attributes *dmabuf) {
	struct wlr_dma_heap_buffer *buffer = dma_heap_buffer_from_buffer(wlr_buffer);
	*dmabuf = buffer->dmabuf;
	return true;
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_dmabuf = buffer_get_dmabuf,
};

static struct wlr_buffer *allocator_create_buffer(
		struct wlr_allocator *wlr_allocator, int width, int height,
		const struct wlr_drm_format *format) {
	struct wlr_dma_heap_allocator *alloc = dma_heap_allocator_from_allocator(wlr_allocator);

	if (!wlr_drm_format_has(format, DRM_FORMAT_MOD_INVALID) &&
			!wlr_drm_format_has(format, DRM_FORMAT_MOD_LINEAR)) {
		wlr_log(WLR_ERROR, "DMA-BUF heap allocator only supports INVALID and "
			"LINEAR modifiers");
		return NULL;
	}

	const struct wlr_pixel_format_info *info = drm_get_pixel_format_info(format->format);
	if (info == NULL) {
		wlr_log(WLR_ERROR, "Unsupported pixel format 0x%"PRIX32, format->format);
		return NULL;
	}

	struct wlr_dma_heap_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}
	wlr_buffer_init(&buffer->base, &buffer_impl, width, height);

	size_t stride = pixel_format_info_min_stride(info, width); // TODO: align?
	struct dma_heap_allocation_data alloc_data = {
		.len = stride * height,
		.fd_flags = O_CLOEXEC | O_RDWR,
	};
	int ret = ioctl(alloc->fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data);
	if (ret != 0) {
		wlr_log_errno(WLR_ERROR, "DMA_HEAP_IOCTL_ALLOC failed");
		free(buffer);
		return NULL;
	}

	buffer->dmabuf = (struct wlr_dmabuf_attributes){
		.width = width,
		.height = height,
		.format = format->format,
		.modifier = DRM_FORMAT_MOD_LINEAR,
		.n_planes = 1,
		.offset[0] = 0,
		.stride[0] = stride,
		.fd[0] = alloc_data.fd,
	};

	return &buffer->base;
}

static void allocator_destroy(struct wlr_allocator *wlr_allocator) {
	free(wlr_allocator);
}

static const struct wlr_allocator_interface allocator_impl = {
	.destroy = allocator_destroy,
	.create_buffer = allocator_create_buffer,
};

struct wlr_allocator *wlr_dma_heap_allocator_create(int heap_fd) {
	struct wlr_dma_heap_allocator *allocator = calloc(1, sizeof(*allocator));
	if (allocator == NULL) {
		return NULL;
	}
	wlr_allocator_init(&allocator->base, &allocator_impl, WLR_BUFFER_CAP_DMABUF);
	allocator->fd = heap_fd;
	return &allocator->base;
}
