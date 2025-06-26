/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_RENDER_DRM_SYNCOBJ_MERGER_H
#define WLR_RENDER_DRM_SYNCOBJ_MERGER_H

#include <wayland-server-core.h>

/**
 * Accumulate timeline points, to have a destination timeline point be
 * signalled when all inputs are
 */
struct wlr_drm_syncobj_merger {
	int n_ref;
	struct wlr_drm_syncobj_timeline *dst_timeline;
	uint64_t dst_point;
	int sync_fd;
};

/**
 * Create a new merger.
 *
 * The given timeline point will be signalled when all input points are
 * signalled and the merger is released.
 */
struct wlr_drm_syncobj_merger *wlr_drm_syncobj_merger_create(
	struct wlr_drm_syncobj_timeline *dst_timeline, uint64_t dst_point);
/**
 * Unreference merger. Target timeline point is materialized when the merger is
 * dropped
 */
void wlr_drm_syncobj_merger_unref(struct wlr_drm_syncobj_merger *merger);
/**
 * Add a new timeline point to wait for.
 *
 * If the point is not materialized, the supplied event loop is used to schedule
 * a wait.
 */
bool wlr_drm_syncobj_merger_add(struct wlr_drm_syncobj_merger *merger,
	struct wlr_drm_syncobj_timeline *dst_timeline, uint64_t dst_point,
	struct wl_event_loop* loop);

#endif