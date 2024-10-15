#include <assert.h>
#include <drm_fourcc.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/util/log.h>
#include "backend/drm/drm.h"
#include "backend/drm/fb.h"
#include "backend/drm/renderer.h"
#include "backend/backend.h"
#include "render/drm_format_set.h"
#include "render/allocator/allocator.h"
#include "render/pixel_format.h"
#include "render/wlr_renderer.h"

void finish_drm_surface(struct wlr_drm_surface *surf) {
	if (!surf || !surf->renderer) {
		return;
	}

	wlr_drm_syncobj_timeline_unref(surf->timeline);
	wlr_swapchain_destroy(surf->swapchain);

	*surf = (struct wlr_drm_surface){0};
}

bool init_drm_surface(struct wlr_drm_surface *surf,
		struct wlr_drm_renderer *renderer, int width, int height,
		const struct wlr_drm_format *drm_format) {
	if (surf->swapchain != NULL && surf->swapchain->width == width &&
			surf->swapchain->height == height) {
		return true;
	}

	finish_drm_surface(surf);

	surf->swapchain = wlr_swapchain_create(renderer->allocator, width, height,
			drm_format);
	if (surf->swapchain == NULL) {
		wlr_log(WLR_ERROR, "Failed to create swapchain");
		return false;
	}

	int drm_fd = wlr_renderer_get_drm_fd(renderer->wlr_rend);
	if (renderer->wlr_rend->features.timeline && drm_fd >= 0) {
		surf->timeline = wlr_drm_syncobj_timeline_create(drm_fd);
		if (surf->timeline == NULL) {
			finish_drm_surface(surf);
			wlr_log(WLR_ERROR, "Failed to create DRM syncobj timeline");
			return false;
		}
	}

	surf->renderer = renderer;

	return true;
}

bool drm_plane_pick_render_format(struct wlr_drm_plane *plane,
		struct wlr_drm_format *fmt, struct wlr_drm_renderer *renderer) {
	const struct wlr_drm_format_set *render_formats =
		wlr_renderer_get_render_formats(renderer->wlr_rend);
	if (render_formats == NULL) {
		wlr_log(WLR_ERROR, "Failed to get render formats");
		return false;
	}

	const struct wlr_drm_format_set *plane_formats = &plane->formats;

	uint32_t format = DRM_FORMAT_ARGB8888;
	if (!wlr_drm_format_set_get(&plane->formats, format)) {
		const struct wlr_pixel_format_info *format_info =
			drm_get_pixel_format_info(format);
		assert(format_info != NULL &&
			format_info->opaque_substitute != DRM_FORMAT_INVALID);
		format = format_info->opaque_substitute;
	}

	const struct wlr_drm_format *render_format =
		wlr_drm_format_set_get(render_formats, format);
	if (render_format == NULL) {
		wlr_log(WLR_DEBUG, "Renderer doesn't support format 0x%"PRIX32, format);
		return false;
	}

	const struct wlr_drm_format *plane_format =
		wlr_drm_format_set_get(plane_formats, format);
	if (plane_format == NULL) {
		wlr_log(WLR_DEBUG, "Plane %"PRIu32" doesn't support format 0x%"PRIX32,
			plane->id, format);
		return false;
	}

	if (!wlr_drm_format_intersect(fmt, plane_format, render_format)) {
		wlr_log(WLR_DEBUG, "Failed to intersect plane and render "
			"modifiers for format 0x%"PRIX32, format);
		return false;
	}

	if (fmt->len == 0) {
		wlr_drm_format_finish(fmt);
		wlr_log(WLR_DEBUG, "Failed to find matching plane and renderer modifiers");
		return false;
	}

	return true;
}
