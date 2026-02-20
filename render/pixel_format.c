#include <assert.h>
#include <drm_fourcc.h>
#include <wlr/util/log.h>
#include "render/pixel_format.h"

const struct wlr_pixel_format_info *drm_get_pixel_format_info(uint32_t fmt) {
	for (size_t i = 0; i < pixel_format_info_len; ++i) {
		if (pixel_format_info[i].drm_format == fmt) {
			return &pixel_format_info[i];
		}
	}

	return NULL;
}

uint32_t convert_wl_shm_format_to_drm(enum wl_shm_format fmt) {
	switch (fmt) {
	case WL_SHM_FORMAT_XRGB8888:
		return DRM_FORMAT_XRGB8888;
	case WL_SHM_FORMAT_ARGB8888:
		return DRM_FORMAT_ARGB8888;
	default:
		return (uint32_t)fmt;
	}
}

enum wl_shm_format convert_drm_format_to_wl_shm(uint32_t fmt) {
	switch (fmt) {
	case DRM_FORMAT_XRGB8888:
		return WL_SHM_FORMAT_XRGB8888;
	case DRM_FORMAT_ARGB8888:
		return WL_SHM_FORMAT_ARGB8888;
	default:
		return (enum wl_shm_format)fmt;
	}
}

uint32_t pixel_format_info_pixels_per_block(const struct wlr_pixel_format_info *info) {
	return info->block_width * info->block_height;
}

static int32_t div_round_up(int32_t dividend, int32_t divisor) {
	int32_t quotient = dividend / divisor;
	if (dividend % divisor != 0) {
		quotient++;
	}
	return quotient;
}

int32_t pixel_format_info_min_stride(const struct wlr_pixel_format_info *fmt, int32_t width) {
	int32_t pixels_per_block = (int32_t)pixel_format_info_pixels_per_block(fmt);
	int32_t bytes_per_block = (int32_t)fmt->bytes_per_block;
	if (width > INT32_MAX / bytes_per_block) {
		wlr_log(WLR_DEBUG, "Invalid width %d (overflow)", width);
		return 0;
	}
	return div_round_up(width * bytes_per_block, pixels_per_block);
}

bool pixel_format_info_check_stride(const struct wlr_pixel_format_info *fmt,
		int32_t stride, int32_t width) {
	int32_t bytes_per_block = (int32_t)fmt->bytes_per_block;
	if (stride % bytes_per_block != 0) {
		wlr_log(WLR_DEBUG, "Invalid stride %d (incompatible with %d "
			"bytes-per-block)", stride, bytes_per_block);
		return false;
	}

	int32_t min_stride = pixel_format_info_min_stride(fmt, width);
	if (min_stride <= 0) {
		return false;
	} else if (stride < min_stride) {
		wlr_log(WLR_DEBUG, "Invalid stride %d (too small for %d "
			"bytes-per-block and width %d)", stride, bytes_per_block, width);
		return false;
	}

	return true;
}

bool pixel_format_has_alpha(uint32_t fmt) {
	return !pixel_format_is_opaque(fmt);
}

uint32_t pixel_format_get_opaque_substitute(uint32_t fmt) {
	switch (fmt) {
	case DRM_FORMAT_ARGB8888:
		return DRM_FORMAT_XRGB8888;
	case DRM_FORMAT_ABGR8888:
		return DRM_FORMAT_XBGR8888;
	case DRM_FORMAT_RGBA8888:
		return DRM_FORMAT_RGBX8888;
	case DRM_FORMAT_BGRA8888:
		return DRM_FORMAT_BGRX8888;
	case DRM_FORMAT_RGBA4444:
		return DRM_FORMAT_RGBX4444;
	case DRM_FORMAT_BGRA4444:
		return DRM_FORMAT_BGRX4444;
	case DRM_FORMAT_RGBA5551:
		return DRM_FORMAT_RGBX5551;
	case DRM_FORMAT_BGRA5551:
		return DRM_FORMAT_BGRX5551;
	case DRM_FORMAT_ARGB1555:
		return DRM_FORMAT_XRGB1555;
	case DRM_FORMAT_ARGB2101010:
		return DRM_FORMAT_XRGB2101010;
	case DRM_FORMAT_ABGR2101010:
		return DRM_FORMAT_XBGR2101010;
	case DRM_FORMAT_ABGR16161616F:
		return DRM_FORMAT_XBGR16161616F;
	case DRM_FORMAT_ABGR16161616:
		return DRM_FORMAT_XBGR16161616;
	}
	return DRM_FORMAT_INVALID;
}
