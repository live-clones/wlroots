#ifndef BACKEND_DRN_COLOR_H
#define BACKEND_DRN_COLOR_H

#include <wlr/util/addon.h>

struct wlr_drm_backend;
struct wlr_drm_crtc;
struct wlr_color_transform;

struct wlr_drm_crtc_color_transform {
	struct wlr_color_transform *base;
	struct wlr_addon addon; // wlr_color_transform.addons
	bool failed;
	struct wlr_color_transform_lut_3x1d *lut_3x1d;
};

struct wlr_drm_crtc_color_transform *drm_crtc_color_transform_import(
	struct wlr_drm_backend *backend, struct wlr_drm_crtc *crtc,
	struct wlr_color_transform *base);

void drm_crtc_color_transform_unref(struct wlr_drm_crtc_color_transform *tr);

#endif
