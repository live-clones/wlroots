/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_RASTER_H
#define WLR_TYPES_WLR_RASTER_H

#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>

struct wlr_buffer;
struct wlr_texture;
struct wlr_renderer;
struct wlr_drm_syncobj_timeline;
struct wlr_surface;
struct wlr_allocator;

struct wlr_raster_source {
	struct wlr_texture *texture;
	struct wlr_allocator *allocator; // may be NULL
	struct wlr_raster *raster;
	struct wl_list link;

	struct wl_listener renderer_destroy;
	struct wl_listener allocator_destroy;
};

struct wlr_raster {
	// May be NULL
	struct wlr_buffer *buffer;

	struct wl_list sources;

	uint32_t width, height;
	bool opaque;

	struct wlr_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;

	struct {
		struct wl_signal destroy;
		struct wl_signal invalidated;
	} events;

	// private state

	size_t n_locks;

	struct wl_listener buffer_release;
};

struct wlr_raster_create_options {
	struct wlr_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
};

/**
 * Creates a new wlr_raster being backed by the given buffer. The raster will
 * not lock the given buffer meaning that once it's released, the raster will
 * NULL its buffer reference and potentially become invalid.
 * The creation function is referenced: once the creator is done with the raster,
 * wlr_raster_unlock must be called as the reference count will start at 1
 * from creation.
 *
 * Options can be NULL.
 */
struct wlr_raster *wlr_raster_create(struct wlr_buffer *buffer,
	const struct wlr_raster_create_options *options);

/**
 * Lock the raster for use. As long as the raster has at least one lock, it
 * will not be destroyed.
 */
struct wlr_raster *wlr_raster_lock(struct wlr_raster *raster);

/**
 * Unlock the raster. This must be called after wlr_raster_lock once the raster
 * has been finished being used or after creation from wlr_raster_create.
 */
void wlr_raster_unlock(struct wlr_raster *raster);

/**
 * Returns the texture allocated for this renderer. If there is none,
 * a new texture will be created and attached to this wlr_raster. Users do not
 * own the texture returned by this function and can only be used for read-only
 * purposes.
 *
 * Will return NULL if the creation was unsuccessful.
 */
struct wlr_texture *wlr_raster_obtain_texture(struct wlr_raster *raster,
	struct wlr_renderer *renderer);

/**
 * Returns the texture allocated for this renderer. If there is none,
 * a new texture will be created and attached to this wlr_raster. Users do not
 * own the texture returned by this function and can only be used for read-only
 * purposes.
 *
 * An optional allocator can be provided which will be used to allocate staging
 * buffers to blit between graphics devices if needed.
 *
 * Will return NULL if the creation was unsuccessful.
 */
struct wlr_texture *wlr_raster_obtain_texture_with_allocator(struct wlr_raster *raster,
	struct wlr_renderer *renderer, struct wlr_allocator *allocator);

/**
 * Creates a wlr_raster from a surface. This will automatically deduplicate
 * rasters if multiple are consumed from the same surface so that redundant
 * uploads are not performed. The raster returned will automatically be locked.
 * Users are required to call wlr_raster_unlock() after invoking this function.
 */
struct wlr_raster *wlr_raster_from_surface(struct wlr_surface *surface);

#endif
