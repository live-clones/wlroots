#ifndef RENDER_ALLOCATOR_GBM_H
#define RENDER_ALLOCATOR_GBM_H

#include <gbm.h>
#include <wlr/render/dmabuf.h>
#include <wlr/types/wlr_buffer.h>
#include "render/allocator/allocator.h"

struct wlr_gbm_buffer {
	struct wlr_buffer base;

	struct wl_list link; // wlr_gbm_allocator.buffers

	struct gbm_bo *gbm_bo; // NULL if the gbm_device has been destroyed
	struct wlr_dmabuf_attributes dmabuf;
};

struct wlr_gbm_allocator {
	struct wlr_allocator base;

	int fd;
	struct gbm_device *gbm_device;
	uint32_t bo_flags;

	struct wl_list buffers; // wlr_gbm_buffer.link
};

/**
 * Creates a new GBM allocator from a DRM FD.
 *
 * bo_flags is a bitfield of enum gbm_bo_flags.
 *
 * Takes ownership over the FD.
 */
struct wlr_allocator *wlr_gbm_allocator_create_with_drm_fd(int drm_fd, uint32_t bo_flags);

#endif
