#ifndef RENDER_ALLOCATOR_DMA_HEAP_H
#define RENDER_ALLOCATOR_DMA_HEAP_H

#include <wlr/types/wlr_buffer.h>
#include "render/allocator/allocator.h"

struct wlr_dma_heap_buffer {
	struct wlr_buffer base;
	struct wlr_dmabuf_attributes dmabuf;
};

struct wlr_dma_heap_allocator {
	struct wlr_allocator base;
	int fd;
};

/**
 * Create a new DMA-BUF heap memory allocator.
 *
 * Takes ownership over the FD.
 */
struct wlr_allocator *wlr_dma_heap_allocator_create(int heap_fd);

#endif
