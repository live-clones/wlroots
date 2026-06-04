#ifndef BACKEND_DRM_PROPERTIES_H
#define BACKEND_DRM_PROPERTIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * These types contain the property ids for several DRM objects.
 * For more details, see:
 * https://dri.freedesktop.org/docs/drm/gpu/drm-kms.html#kms-properties
 */

struct wlr_drm_connector_props {
	uint32_t edid;
	uint32_t dpms;
	uint32_t link_status; // not guaranteed to exist
	uint32_t path;
	uint32_t vrr_capable; // not guaranteed to exist
	uint32_t subconnector; // not guaranteed to exist
	uint32_t non_desktop;
	uint32_t panel_orientation; // not guaranteed to exist
	uint32_t content_type; // not guaranteed to exist
	uint32_t max_bpc; // not guaranteed to exist

	// atomic-modesetting only

	uint32_t crtc_id;
	uint32_t colorspace;
	uint32_t hdr_output_metadata;
};

struct wlr_drm_crtc_props {
	// Neither of these are guaranteed to exist
	uint32_t vrr_enabled;
	uint32_t gamma_lut;
	uint32_t gamma_lut_size;

	// atomic-modesetting only

	uint32_t active;
	uint32_t mode_id;
	uint32_t out_fence_ptr;
};

struct wlr_drm_plane_props {
	uint32_t type;
	uint32_t rotation; // Not guaranteed to exist
	uint32_t in_formats; // Not guaranteed to exist
	uint32_t size_hints; // Not guaranteed to exist

	// atomic-modesetting only

	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t crtc_x;
	uint32_t crtc_y;
	uint32_t crtc_w;
	uint32_t crtc_h;
	uint32_t fb_id;
	uint32_t crtc_id;
	uint32_t fb_damage_clips;
	uint32_t hotspot_x;
	uint32_t hotspot_y;
	uint32_t in_fence_fd;

	uint32_t color_encoding; // Not guaranteed to exist
	uint32_t color_range; // Not guaranteed to exist
	uint32_t color_pipeline; // Not guaranteed to exist
};

// Equivalent to wlr_drm_color_encoding defined in the kernel (but not exported)
enum wlr_drm_color_encoding {
	WLR_DRM_COLOR_YCBCR_BT601,
	WLR_DRM_COLOR_YCBCR_BT709,
	WLR_DRM_COLOR_YCBCR_BT2020,
};

// Equivalent to wlr_drm_color_range defined in the kernel (but not exported)
enum wlr_drm_color_range {
	WLR_DRM_COLOR_YCBCR_FULL_RANGE,
	WLR_DRM_COLOR_YCBCR_LIMITED_RANGE,
};

// Equivalent to drm_colorop_curve_1d_type defined in the kernel (but not exported)
enum wlr_drm_colorop_curve_1d_type {
	WLR_DRM_COLOROP_1D_CURVE_SRGB_EOTF,
	WLR_DRM_COLOROP_1D_CURVE_SRGB_INV_EOTF,
	WLR_DRM_COLOROP_1D_CURVE_PQ_125_EOTF,
	WLR_DRM_COLOROP_1D_CURVE_PQ_125_INV_EOTF,
	WLR_DRM_COLOROP_1D_CURVE_BT2020_INV_OETF,
	WLR_DRM_COLOROP_1D_CURVE_BT2020_OETF,
	WLR_DRM_COLOROP_1D_CURVE_GAMMA22,
	WLR_DRM_COLOROP_1D_CURVE_GAMMA22_INV,
};

struct wlr_drm_colorop_props {
	uint32_t type;
	uint32_t next;
	uint32_t bypass;
	uint32_t data; // for 1D_LUT, CTM_3X4, 3D_LUT
	uint32_t size; // for 1D_LUT, 3D_LUT
	uint32_t curve_1d_type; // for 1D_CURVE
	uint32_t multiplier; // for MULTIPLIER
};

bool get_drm_connector_props(int fd, uint32_t id,
	struct wlr_drm_connector_props *out);
bool get_drm_crtc_props(int fd, uint32_t id, struct wlr_drm_crtc_props *out);
bool get_drm_plane_props(int fd, uint32_t id, struct wlr_drm_plane_props *out);
bool get_drm_colorop_props(int fd, uint32_t id, struct wlr_drm_colorop_props *out);

bool get_drm_prop(int fd, uint32_t obj, uint32_t prop, uint64_t *ret);
void *get_drm_prop_blob(int fd, uint32_t obj, uint32_t prop, size_t *ret_len);
char *get_drm_prop_enum(int fd, uint32_t obj, uint32_t prop);

bool introspect_drm_prop_range(int fd, uint32_t prop_id,
	uint64_t *min, uint64_t *max);
bool introspect_drm_prop_enum(int fd, uint32_t prop_id, uint64_t *bitmask);

#endif
