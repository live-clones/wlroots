#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <xf86drmMode.h>
#include "backend/drm/drm.h"
#include "backend/drm/fb.h"
#include "backend/drm/iface.h"
#include "backend/drm/util.h"
#include "render/color.h"

static char *atomic_commit_flags_str(uint32_t flags) {
	const char *const l[] = {
		(flags & DRM_MODE_PAGE_FLIP_EVENT) ? "PAGE_FLIP_EVENT" : NULL,
		(flags & DRM_MODE_PAGE_FLIP_ASYNC) ? "PAGE_FLIP_ASYNC" : NULL,
		(flags & DRM_MODE_ATOMIC_TEST_ONLY) ? "ATOMIC_TEST_ONLY" : NULL,
		(flags & DRM_MODE_ATOMIC_NONBLOCK) ? "ATOMIC_NONBLOCK" : NULL,
		(flags & DRM_MODE_ATOMIC_ALLOW_MODESET) ? "ATOMIC_ALLOW_MODESET" : NULL,
	};

	char *buf = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&buf, &size);
	if (f == NULL) {
		return NULL;
	}

	for (size_t i = 0; i < sizeof(l) / sizeof(l[0]); i++) {
		if (l[i] == NULL) {
			continue;
		}
		if (ftell(f) > 0) {
			fprintf(f, " | ");
		}
		fprintf(f, "%s", l[i]);
	}

	if (ftell(f) == 0) {
		fprintf(f, "none");
	}

	fclose(f);

	return buf;
}

struct atomic {
	drmModeAtomicReq *req;
	bool failed;
};

static void atomic_begin(struct atomic *atom) {
	*atom = (struct atomic){0};

	atom->req = drmModeAtomicAlloc();
	if (!atom->req) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		atom->failed = true;
		return;
	}
}

static bool atomic_commit(struct atomic *atom, struct wlr_drm_backend *drm,
		const struct wlr_drm_device_state *state,
		struct wlr_drm_page_flip *page_flip, uint32_t flags) {
	if (atom->failed) {
		return false;
	}

	int ret = drmModeAtomicCommit(drm->fd, atom->req, flags, page_flip);
	if (ret != 0) {
		enum wlr_log_importance log_level = WLR_ERROR;
		if (flags & DRM_MODE_ATOMIC_TEST_ONLY) {
			log_level = WLR_DEBUG;
		}

		if (state->connectors_len == 1) {
			struct wlr_drm_connector *conn = state->connectors[0].connector;
			wlr_drm_conn_log_errno(conn, log_level, "Atomic commit failed");
		} else {
			wlr_log_errno(log_level, "Atomic commit failed");
		}
		char *flags_str = atomic_commit_flags_str(flags);
		wlr_log(WLR_DEBUG, "(Atomic commit flags: %s)",
			flags_str ? flags_str : "<error>");
		free(flags_str);
		return false;
	}

	return true;
}

static void atomic_finish(struct atomic *atom) {
	drmModeAtomicFree(atom->req);
}

static void atomic_add(struct atomic *atom, uint32_t id, uint32_t prop, uint64_t val) {
	if (!atom->failed && drmModeAtomicAddProperty(atom->req, id, prop, val) < 0) {
		wlr_log_errno(WLR_ERROR, "Failed to add atomic DRM property");
		atom->failed = true;
	}
}

static bool create_mode_blob(struct wlr_drm_connector *conn,
		const struct wlr_drm_connector_state *state, uint32_t *blob_id) {
	if (!state->active) {
		*blob_id = 0;
		return true;
	}

	if (drmModeCreatePropertyBlob(conn->backend->fd, &state->mode,
			sizeof(drmModeModeInfo), blob_id)) {
		wlr_log_errno(WLR_ERROR, "Unable to create mode property blob");
		return false;
	}

	return true;
}

static bool create_gamma_lut_blob(struct wlr_drm_backend *drm,
		size_t size, const uint16_t *lut, uint32_t *blob_id) {
	if (size == 0) {
		*blob_id = 0;
		return true;
	}

	struct drm_color_lut *gamma = malloc(size * sizeof(*gamma));
	if (gamma == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate gamma table");
		return false;
	}

	const uint16_t *r = lut;
	const uint16_t *g = lut + size;
	const uint16_t *b = lut + 2 * size;
	for (size_t i = 0; i < size; i++) {
		gamma[i].red = r[i];
		gamma[i].green = g[i];
		gamma[i].blue = b[i];
	}

	if (drmModeCreatePropertyBlob(drm->fd, gamma,
			size * sizeof(*gamma), blob_id) != 0) {
		wlr_log_errno(WLR_ERROR, "Unable to create gamma LUT property blob");
		free(gamma);
		return false;
	}
	free(gamma);

	return true;
}

bool create_fb_damage_clips_blob(struct wlr_drm_backend *drm,
		int width, int height, const pixman_region32_t *damage, uint32_t *blob_id) {
	pixman_region32_t clipped;
	pixman_region32_init(&clipped);
	pixman_region32_intersect_rect(&clipped, damage, 0, 0, width, height);

	int rects_len;
	const pixman_box32_t *rects = pixman_region32_rectangles(&clipped, &rects_len);

	int ret;
	if (rects_len > 0) {
		ret = drmModeCreatePropertyBlob(drm->fd, rects, sizeof(*rects) * rects_len, blob_id);
	} else {
		ret = 0;
		*blob_id = 0;
	}
	pixman_region32_fini(&clipped);
	if (ret != 0) {
		wlr_log_errno(WLR_ERROR, "Failed to create FB_DAMAGE_CLIPS property blob");
		return false;
	}

	return true;
}

static uint8_t convert_cta861_eotf(enum wlr_color_transfer_function tf) {
	switch (tf) {
	case WLR_COLOR_TRANSFER_FUNCTION_SRGB:
		abort(); // unsupported
	case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		return 2;
	case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
		abort(); // unsupported
	case WLR_COLOR_TRANSFER_FUNCTION_GAMMA22:
		abort(); // unsupported
	case WLR_COLOR_TRANSFER_FUNCTION_BT1886:
		abort(); // unsupported
	}
	abort(); // unreachable
}

static uint16_t convert_cta861_color_coord(double v) {
	if (v < 0) {
		v = 0;
	}
	if (v > 1) {
		v = 1;
	}
	return (uint16_t)round(v * 50000);
}

static bool create_hdr_output_metadata_blob(struct wlr_drm_backend *drm,
		const struct wlr_output_image_description *img_desc, uint32_t *blob_id) {
	if (img_desc == NULL) {
		*blob_id = 0;
		return true;
	}

	struct hdr_output_metadata metadata = {
		.metadata_type = 0,
		.hdmi_metadata_type1 = {
			.eotf = convert_cta861_eotf(img_desc->transfer_function),
			.metadata_type = 0,
			.display_primaries = {
				{
					.x = convert_cta861_color_coord(img_desc->mastering_display_primaries.red.x),
					.y = convert_cta861_color_coord(img_desc->mastering_display_primaries.red.y),
				},
				{
					.x = convert_cta861_color_coord(img_desc->mastering_display_primaries.green.x),
					.y = convert_cta861_color_coord(img_desc->mastering_display_primaries.green.y),
				},
				{
					.x = convert_cta861_color_coord(img_desc->mastering_display_primaries.blue.x),
					.y = convert_cta861_color_coord(img_desc->mastering_display_primaries.blue.y),
				},
			},
			.white_point = {
				.x = convert_cta861_color_coord(img_desc->mastering_display_primaries.white.x),
				.y = convert_cta861_color_coord(img_desc->mastering_display_primaries.white.y),
			},
			.max_display_mastering_luminance = img_desc->mastering_luminance.max,
			.min_display_mastering_luminance = img_desc->mastering_luminance.min * 0.0001,
			.max_cll = img_desc->max_cll,
			.max_fall = img_desc->max_fall,
		},
	};
	if (drmModeCreatePropertyBlob(drm->fd, &metadata, sizeof(metadata), blob_id) != 0) {
		wlr_log_errno(WLR_ERROR, "Failed to create HDR_OUTPUT_METADATA property");
		return false;
	}
	return true;
}

static uint64_t convert_primaries_to_colorspace(uint32_t primaries) {
	switch (primaries) {
	case 0:
		return 0; // Default
	case WLR_COLOR_NAMED_PRIMARIES_BT2020:
		return 9; // BT2020_RGB
	}
	abort(); // unreachable
}

static uint64_t max_bpc_for_format(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		return 10;
	case DRM_FORMAT_XBGR16161616F:
	case DRM_FORMAT_ABGR16161616F:
	case DRM_FORMAT_XBGR16161616:
	case DRM_FORMAT_ABGR16161616:
		return 16;
	default:
		return 8;
	}
}

static uint64_t pick_max_bpc(struct wlr_drm_connector *conn, struct wlr_drm_fb *fb) {
	uint32_t format = DRM_FORMAT_INVALID;
	struct wlr_dmabuf_attributes attribs = {0};
	if (wlr_buffer_get_dmabuf(fb->wlr_buf, &attribs)) {
		format = attribs.format;
	}

	uint64_t target_bpc = max_bpc_for_format(format);
	if (target_bpc < conn->max_bpc_bounds[0]) {
		target_bpc = conn->max_bpc_bounds[0];
	}
	if (target_bpc > conn->max_bpc_bounds[1]) {
		target_bpc = conn->max_bpc_bounds[1];
	}
	return target_bpc;
}

/** Convert a double to S31.32 sign-magnitude format */
static uint64_t to_fixed_s31_32(double v) {
	uint64_t u = fabs(v) * ((uint64_t)1 << 32);
	if (v < 0) {
		u |= (uint64_t)1 << 63;
	}
	return u;
}

enum colorop_setup_status {
	COLOROP_SETUP_SUCCESS,
	COLOROP_SETUP_FAILURE,
	COLOROP_SETUP_INCOMPATIBLE,
};

static enum colorop_setup_status setup_inverse_eotf_colorop(struct wlr_drm_backend *drm,
		struct wlr_drm_colorop *colorop,
		struct wlr_color_transform_inverse_eotf *inv_eotf,
		struct wlr_drm_colorop_state *state) {
	if (colorop->type != DRM_COLOROP_1D_CURVE) {
		return COLOROP_SETUP_INCOMPATIBLE;
	}

	enum wlr_drm_colorop_curve_1d_type type = (uint32_t)-1;
	switch (inv_eotf->tf) {
	case WLR_COLOR_TRANSFER_FUNCTION_SRGB:
		type = WLR_DRM_COLOROP_1D_CURVE_SRGB_INV_EOTF;
		break;
	case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		break; // TODO: account for 125x scale
	case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
		if (!colorop->props.bypass) {
			return COLOROP_SETUP_INCOMPATIBLE;
		}
		state->bypass = true;
		return COLOROP_SETUP_SUCCESS;
	case WLR_COLOR_TRANSFER_FUNCTION_GAMMA22:
		type = WLR_DRM_COLOROP_1D_CURVE_GAMMA22_INV;
		break;
	case WLR_COLOR_TRANSFER_FUNCTION_BT1886:
		break; // not supported by KMS yet
	}
	assert(type != (uint32_t)-1);

	if (!(colorop->curve_1d_types & (1 << type))) {
		return COLOROP_SETUP_INCOMPATIBLE;
	}

	state->curve_1d_type = type;
	return COLOROP_SETUP_SUCCESS;
}

static enum colorop_setup_status setup_matrix_colorop(struct wlr_drm_backend *drm,
		struct wlr_drm_colorop *colorop,
		const struct wlr_color_transform_matrix *tr_matrix,
		struct wlr_drm_colorop_state *state) {
	if (colorop->type != DRM_COLOROP_CTM_3X4) {
		return COLOROP_SETUP_INCOMPATIBLE;
	}
	const float *matrix = tr_matrix->matrix;
	struct drm_color_ctm_3x4 data = {
		.matrix = {
			to_fixed_s31_32(matrix[0]), to_fixed_s31_32(matrix[1]), to_fixed_s31_32(matrix[2]), 0,
			to_fixed_s31_32(matrix[3]), to_fixed_s31_32(matrix[4]), to_fixed_s31_32(matrix[5]), 0,
			to_fixed_s31_32(matrix[6]), to_fixed_s31_32(matrix[7]), to_fixed_s31_32(matrix[8]), 0,
		},
	};
	uint32_t blob_id = 0;
	if (drmModeCreatePropertyBlob(drm->fd, &data, sizeof(data), &blob_id) != 0) {
		wlr_log_errno(WLR_ERROR, "Failed to create DATA property");
		return COLOROP_SETUP_FAILURE;
	}
	state->data = blob_id;
	return COLOROP_SETUP_SUCCESS;
}

static enum colorop_setup_status setup_lut_3x1d_colorop(struct wlr_drm_backend *drm,
		struct wlr_drm_colorop *colorop,
		const struct wlr_color_transform_lut_3x1d *lut_3x1d,
		struct wlr_drm_colorop_state *state) {
	if (colorop->type != DRM_COLOROP_1D_LUT || colorop->size != lut_3x1d->dim) {
		return COLOROP_SETUP_INCOMPATIBLE;
	}

	size_t size = lut_3x1d->dim * sizeof(struct drm_color_lut32);
	struct drm_color_lut32 *data = malloc(size);
	if (data == NULL) {
		return COLOROP_SETUP_FAILURE;
	}

	for (size_t i = 0; i < lut_3x1d->dim; i++) {
		uint32_t factor = UINT32_MAX / UINT16_MAX;
		data[i] = (struct drm_color_lut32){
			.red = lut_3x1d->lut_3x1d[0 * lut_3x1d->dim + i] * factor,
			.green = lut_3x1d->lut_3x1d[1 * lut_3x1d->dim + i] * factor,
			.blue = lut_3x1d->lut_3x1d[2 * lut_3x1d->dim + i] * factor,
		};
	}

	uint32_t blob_id = 0;
	int ret = drmModeCreatePropertyBlob(drm->fd, data, size, &blob_id);
	free(data);
	if (ret != 0) {
		wlr_log_errno(WLR_ERROR, "Failed to create DATA property");
		return COLOROP_SETUP_FAILURE;
	}

	state->data = blob_id;
	return COLOROP_SETUP_SUCCESS;
}

static enum colorop_setup_status setup_colorop(struct wlr_drm_backend *drm,
		struct wlr_drm_colorop *colorop, struct wlr_color_transform *tr,
		struct wlr_drm_colorop_state *state) {
	switch (tr->type) {
	case COLOR_TRANSFORM_INVERSE_EOTF:;
		struct wlr_color_transform_inverse_eotf *inv_eotf =
			wl_container_of(tr, inv_eotf, base);
		return setup_inverse_eotf_colorop(drm, colorop, inv_eotf, state);
	case COLOR_TRANSFORM_LCMS2:
		return COLOROP_SETUP_INCOMPATIBLE; // TODO: setup 3D LUT
	case COLOR_TRANSFORM_LUT_3X1D:;
		struct wlr_color_transform_lut_3x1d *lut_3x1d =
			wl_container_of(tr, lut_3x1d, base);
		return setup_lut_3x1d_colorop(drm, colorop, lut_3x1d, state);
	case COLOR_TRANSFORM_MATRIX:;
		struct wlr_color_transform_matrix *matrix =
			wl_container_of(tr, matrix, base);
		return setup_matrix_colorop(drm, colorop, matrix, state);
	case COLOR_TRANSFORM_PIPELINE:
		break; // nested pipelines are not supported
	}
	return COLOROP_SETUP_INCOMPATIBLE;
}

static enum colorop_setup_status setup_color_pipeline(struct wlr_drm_backend *drm,
		struct wl_list *pipeline, struct wlr_color_transform **transforms,
		size_t transforms_len, struct wl_array *out_state_arr) {
	struct wl_array state_arr = {0};

	size_t i = 0;
	struct wlr_drm_colorop *colorop;
	wl_list_for_each(colorop, pipeline, link) {
		struct wlr_drm_colorop_state *state = wl_array_add(&state_arr, sizeof(*state));
		if (state == NULL) {
			wl_array_release(&state_arr);
			return COLOROP_SETUP_FAILURE;
		}
		state->colorop = colorop;

		enum colorop_setup_status status = COLOROP_SETUP_INCOMPATIBLE;
		if (i < transforms_len) {
			status = setup_colorop(drm, colorop, transforms[i], state);
		}
		switch (status) {
		case COLOROP_SETUP_SUCCESS:
			i++;
			break;
		case COLOROP_SETUP_FAILURE:
			return status;
		case COLOROP_SETUP_INCOMPATIBLE:
			if (!colorop->props.bypass) {
				wl_array_release(&state_arr);
				return COLOROP_SETUP_INCOMPATIBLE;
			}
			state->bypass = true;
			break;
		}
	}

	if (i < transforms_len) {
		wl_array_release(&state_arr);
		return COLOROP_SETUP_INCOMPATIBLE;
	}

	*out_state_arr = state_arr;
	return COLOROP_SETUP_SUCCESS;
}

static bool pick_plane_color_pipeline(struct wlr_drm_backend *drm,
		struct wlr_drm_plane *plane, struct wlr_drm_connector_state *state,
		uint32_t *out_pipeline_id) {
	struct wlr_color_transform *base_tr = state->base->pre_color_transform;
	struct wlr_color_transform *tr = NULL;
	if (!color_transform_compose(&tr, &base_tr, 1)) {
		return false;
	}

	if (tr == NULL) {
		*out_pipeline_id = 0;
		return true;
	}

	struct wlr_color_transform **transforms;
	size_t transforms_len;
	if (tr->type == COLOR_TRANSFORM_PIPELINE) {
		struct wlr_color_transform_pipeline *tr_pipeline =
			wl_container_of(tr, tr_pipeline, base);
		transforms = tr_pipeline->transforms;
		transforms_len = tr_pipeline->len;
	} else {
		transforms = &tr;
		transforms_len = 1;
	}

	for (size_t i = 0; i < plane->color_pipelines_len; i++) {
		struct wl_list *pipeline = &plane->color_pipelines[i];
		struct wl_array colorops = {0};
		enum colorop_setup_status status =
			setup_color_pipeline(drm, pipeline, transforms, transforms_len, &colorops);
		switch (status) {
		case COLOROP_SETUP_SUCCESS:;
			struct wlr_drm_colorop *colorop =
				wl_container_of(&pipeline->next, colorop, link);
			*out_pipeline_id = colorop->id;
			assert(state->colorops.size == 0);
			state->colorops = colorops;
			wlr_color_transform_unref(tr);
			return true;
		case COLOROP_SETUP_FAILURE:
			wlr_color_transform_unref(tr);
			return false;
		case COLOROP_SETUP_INCOMPATIBLE:
			break; // try the next pipeline
		}
	}

	wlr_color_transform_unref(tr);
	return false;
}

static void destroy_blob(struct wlr_drm_backend *drm, uint32_t id) {
	if (id == 0) {
		return;
	}
	if (drmModeDestroyPropertyBlob(drm->fd, id) != 0) {
		wlr_log_errno(WLR_ERROR, "Failed to destroy blob");
	}
}

static void commit_blob(struct wlr_drm_backend *drm,
		uint32_t *current, uint32_t next) {
	if (*current == next) {
		return;
	}
	destroy_blob(drm, *current);
	*current = next;
}

static void rollback_blob(struct wlr_drm_backend *drm,
		uint32_t *current, uint32_t next) {
	if (*current == next) {
		return;
	}
	destroy_blob(drm, next);
}

bool drm_atomic_connector_prepare(struct wlr_drm_connector_state *state, bool modeset) {
	struct wlr_drm_connector *conn = state->connector;
	struct wlr_drm_backend *drm = conn->backend;
	struct wlr_output *output = &conn->output;
	struct wlr_drm_crtc *crtc = conn->crtc;

	uint32_t mode_id = crtc->mode_id;
	if (modeset) {
		if (!create_mode_blob(conn, state, &mode_id)) {
			return false;
		}
	}

	uint32_t gamma_lut = crtc->gamma_lut;
	if (state->base->committed & WLR_OUTPUT_STATE_POST_COLOR_TRANSFORM) {
		size_t dim = 0;
		uint16_t *lut = NULL;
		if (state->base->post_color_transform != NULL) {
			struct wlr_color_transform_lut_3x1d *tr =
				color_transform_lut_3x1d_from_base(state->base->post_color_transform);
			dim = tr->dim;
			lut = tr->lut_3x1d;
		}

		// Fallback to legacy gamma interface when gamma properties are not
		// available (can happen on older Intel GPUs that support gamma but not
		// degamma).
		if (crtc->props.gamma_lut == 0) {
			if (!drm_legacy_crtc_set_gamma(drm, crtc, dim, lut)) {
				return false;
			}
		} else {
			if (!create_gamma_lut_blob(drm, dim, lut, &gamma_lut)) {
				return false;
			}
		}
	}

	uint32_t primary_color_pipeline = crtc->primary->color_pipeline;
	if (state->base->committed & WLR_OUTPUT_STATE_PRE_COLOR_TRANSFORM) {
		if (!pick_plane_color_pipeline(drm, crtc->primary, state, &primary_color_pipeline)) {
			return false;
		}
	}

	uint32_t fb_damage_clips = 0;
	if ((state->base->committed & WLR_OUTPUT_STATE_DAMAGE) &&
			crtc->primary->props.fb_damage_clips != 0) {
		create_fb_damage_clips_blob(drm, state->primary_fb->wlr_buf->width,
			state->primary_fb->wlr_buf->height, &state->base->damage, &fb_damage_clips);
	}

	int in_fence_fd = -1;
	if (state->wait_timeline != NULL) {
		in_fence_fd = wlr_drm_syncobj_timeline_export_sync_file(state->wait_timeline,
			state->wait_point);
		if (in_fence_fd < 0) {
			return false;
		}
	}

	bool prev_vrr_enabled =
		output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;
	bool vrr_enabled = prev_vrr_enabled;
	if ((state->base->committed & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED)) {
		if (state->base->adaptive_sync_enabled && !output->adaptive_sync_supported) {
			return false;
		}
		vrr_enabled = state->base->adaptive_sync_enabled;
	}

	uint32_t colorspace = conn->colorspace;
	if (state->base->committed & WLR_OUTPUT_STATE_IMAGE_DESCRIPTION) {
		colorspace = convert_primaries_to_colorspace(
			state->base->image_description ? state->base->image_description->primaries : 0);
	}

	uint32_t hdr_output_metadata = conn->hdr_output_metadata;
	if ((state->base->committed & WLR_OUTPUT_STATE_IMAGE_DESCRIPTION) &&
			!create_hdr_output_metadata_blob(drm, state->base->image_description, &hdr_output_metadata)) {
		return false;
	}

	state->mode_id = mode_id;
	state->gamma_lut = gamma_lut;
	state->fb_damage_clips = fb_damage_clips;
	state->primary_in_fence_fd = in_fence_fd;
	state->vrr_enabled = vrr_enabled;
	state->colorspace = colorspace;
	state->hdr_output_metadata = hdr_output_metadata;
	state->primary_color_pipeline = primary_color_pipeline;
	return true;
}

void drm_atomic_connector_apply_commit(struct wlr_drm_connector_state *state) {
	struct wlr_drm_connector *conn = state->connector;
	struct wlr_drm_crtc *crtc = conn->crtc;
	struct wlr_drm_backend *drm = conn->backend;

	if (!crtc->own_mode_id) {
		crtc->mode_id = 0; // don't try to delete previous master's blobs
	}
	crtc->own_mode_id = true;
	commit_blob(drm, &crtc->mode_id, state->mode_id);
	commit_blob(drm, &crtc->gamma_lut, state->gamma_lut);
	commit_blob(drm, &conn->hdr_output_metadata, state->hdr_output_metadata);

	conn->output.adaptive_sync_status = state->vrr_enabled ?
		WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED : WLR_OUTPUT_ADAPTIVE_SYNC_DISABLED;

	destroy_blob(drm, state->fb_damage_clips);
	if (state->primary_in_fence_fd >= 0) {
		close(state->primary_in_fence_fd);
	}

	conn->colorspace = state->colorspace;
	crtc->primary->color_pipeline = state->primary_color_pipeline;

	struct wlr_drm_colorop_state *colorop_state;
	wl_array_for_each(colorop_state, &state->colorops) {
		commit_blob(drm, &colorop_state->colorop->data, colorop_state->data);
	}
	wl_array_release(&state->colorops);
}

void drm_atomic_connector_rollback_commit(struct wlr_drm_connector_state *state) {
	struct wlr_drm_connector *conn = state->connector;
	struct wlr_drm_crtc *crtc = conn->crtc;
	struct wlr_drm_backend *drm = conn->backend;

	rollback_blob(drm, &crtc->mode_id, state->mode_id);
	rollback_blob(drm, &crtc->gamma_lut, state->gamma_lut);
	rollback_blob(drm, &conn->hdr_output_metadata, state->hdr_output_metadata);

	destroy_blob(drm, state->fb_damage_clips);
	if (state->primary_in_fence_fd >= 0) {
		close(state->primary_in_fence_fd);
	}

	struct wlr_drm_colorop_state *colorop_state;
	wl_array_for_each(colorop_state, &state->colorops) {
		destroy_blob(drm, colorop_state->data);
	}
	wl_array_release(&state->colorops);
}

static void plane_disable(struct atomic *atom, struct wlr_drm_plane *plane) {
	uint32_t id = plane->id;
	const struct wlr_drm_plane_props *props = &plane->props;
	atomic_add(atom, id, props->fb_id, 0);
	atomic_add(atom, id, props->crtc_id, 0);
}

static void set_plane_props(struct atomic *atom, struct wlr_drm_backend *drm,
		struct wlr_drm_plane *plane, struct wlr_drm_fb *fb, uint32_t crtc_id,
		const struct wlr_box *dst_box,
		const struct wlr_fbox *src_box) {
	uint32_t id = plane->id;
	const struct wlr_drm_plane_props *props = &plane->props;

	if (fb == NULL) {
		wlr_log(WLR_ERROR, "Failed to acquire FB for plane %"PRIu32, plane->id);
		atom->failed = true;
		return;
	}

	// The src_* properties are in 16.16 fixed point
	atomic_add(atom, id, props->src_x, src_box->x * (1 << 16));
	atomic_add(atom, id, props->src_y, src_box->y * (1 << 16));
	atomic_add(atom, id, props->src_w, src_box->width * (1 << 16));
	atomic_add(atom, id, props->src_h, src_box->height * (1 << 16));
	atomic_add(atom, id, props->fb_id, fb->id);
	atomic_add(atom, id, props->crtc_id, crtc_id);
	atomic_add(atom, id, props->crtc_x, dst_box->x);
	atomic_add(atom, id, props->crtc_y, dst_box->y);
	atomic_add(atom, id, props->crtc_w, dst_box->width);
	atomic_add(atom, id, props->crtc_h, dst_box->height);
}

static void set_color_encoding_and_range(struct atomic *atom,
		struct wlr_drm_backend *drm, struct wlr_drm_plane *plane,
		enum wlr_color_encoding encoding, enum wlr_color_range range) {
	uint32_t id = plane->id;
	const struct wlr_drm_plane_props *props = &plane->props;

	uint32_t color_encoding;
	switch (encoding) {
	case WLR_COLOR_ENCODING_NONE:
	case WLR_COLOR_ENCODING_BT601:
		color_encoding = WLR_DRM_COLOR_YCBCR_BT601;
		break;
	case WLR_COLOR_ENCODING_BT709:
		color_encoding = WLR_DRM_COLOR_YCBCR_BT709;
		break;
	case WLR_COLOR_ENCODING_BT2020:
		color_encoding = WLR_DRM_COLOR_YCBCR_BT2020;
		break;
	default:
		wlr_log(WLR_DEBUG, "Unsupported color encoding %d", encoding);
		atom->failed = true;
		return;
	}

	if (props->color_encoding) {
		atomic_add(atom, id, props->color_encoding, color_encoding);
	} else {
		wlr_log(WLR_DEBUG, "Plane %"PRIu32" is missing the COLOR_ENCODING property",
			id);
		atom->failed = true;
		return;
	}

	uint32_t color_range;
	switch (range) {
	case WLR_COLOR_RANGE_NONE:
	case WLR_COLOR_RANGE_LIMITED:
		color_range = WLR_DRM_COLOR_YCBCR_LIMITED_RANGE;
		break;
	case WLR_COLOR_RANGE_FULL:
		color_range = WLR_DRM_COLOR_YCBCR_FULL_RANGE;
		break;
	default:
		assert(0); // Unreachable
	}

	if (props->color_range) {
		atomic_add(atom, id, props->color_range, color_range);
	} else {
		wlr_log(WLR_DEBUG, "Plane %"PRIu32" is missing the COLOR_RANGE property",
			id);
		atom->failed = true;
		return;
	}
}

static bool supports_cursor_hotspots(const struct wlr_drm_plane *plane) {
	return plane->props.hotspot_x && plane->props.hotspot_y;
}

static void set_plane_in_fence_fd(struct atomic *atom,
		struct wlr_drm_plane *plane, int sync_file_fd) {
	if (!plane->props.in_fence_fd) {
		wlr_log(WLR_ERROR, "Plane %"PRIu32 " is missing the IN_FENCE_FD property",
			plane->id);
		atom->failed = true;
		return;
	}

	atomic_add(atom, plane->id, plane->props.in_fence_fd, sync_file_fd);
}

static void atomic_connector_add(struct atomic *atom,
		struct wlr_drm_connector_state *state, bool modeset) {
	struct wlr_drm_connector *conn = state->connector;
	struct wlr_drm_backend *drm = conn->backend;
	struct wlr_drm_crtc *crtc = conn->crtc;
	bool active = state->active;

	atomic_add(atom, conn->id, conn->props.crtc_id, active ? crtc->id : 0);
	if (modeset && active && conn->props.link_status != 0) {
		atomic_add(atom, conn->id, conn->props.link_status,
			DRM_MODE_LINK_STATUS_GOOD);
	}
	if (active && conn->props.content_type != 0) {
		atomic_add(atom, conn->id, conn->props.content_type,
			DRM_MODE_CONTENT_TYPE_GRAPHICS);
	}
	if (modeset && active && conn->props.max_bpc != 0 && conn->max_bpc_bounds[1] != 0) {
		atomic_add(atom, conn->id, conn->props.max_bpc, pick_max_bpc(conn, state->primary_fb));
	}
	if (conn->props.colorspace != 0) {
		atomic_add(atom, conn->id, conn->props.colorspace, state->colorspace);
	}
	if (conn->props.hdr_output_metadata != 0) {
		atomic_add(atom, conn->id, conn->props.hdr_output_metadata, state->hdr_output_metadata);
	}
	atomic_add(atom, crtc->id, crtc->props.mode_id, state->mode_id);
	atomic_add(atom, crtc->id, crtc->props.active, active);
	if (active) {
		if (crtc->props.gamma_lut != 0) {
			atomic_add(atom, crtc->id, crtc->props.gamma_lut, state->gamma_lut);
		}
		if (crtc->props.vrr_enabled != 0) {
			atomic_add(atom, crtc->id, crtc->props.vrr_enabled, state->vrr_enabled);
		}

		set_plane_props(atom, drm, crtc->primary, state->primary_fb, crtc->id,
			&state->primary_viewport.dst_box, &state->primary_viewport.src_box);
		if (state->base->committed & WLR_OUTPUT_STATE_COLOR_REPRESENTATION) {
			set_color_encoding_and_range(atom, drm, crtc->primary,
				state->base->color_encoding, state->base->color_range);
		}
		if (crtc->primary->props.fb_damage_clips != 0) {
			atomic_add(atom, crtc->primary->id,
				crtc->primary->props.fb_damage_clips, state->fb_damage_clips);
		}
		if (state->primary_in_fence_fd >= 0) {
			set_plane_in_fence_fd(atom, crtc->primary, state->primary_in_fence_fd);
		}
		if (crtc->primary->props.color_pipeline != 0) {
			atomic_add(atom, crtc->primary->id,
				crtc->primary->props.color_pipeline, state->primary_color_pipeline);
		}
		if (crtc->cursor) {
			if (drm_connector_is_cursor_visible(conn)) {
				struct wlr_fbox cursor_src = {
					.width = state->cursor_fb->wlr_buf->width,
					.height = state->cursor_fb->wlr_buf->height,
				};
				struct wlr_box cursor_dst = {
					.x = conn->cursor_x,
					.y = conn->cursor_y,
					.width = state->cursor_fb->wlr_buf->width,
					.height = state->cursor_fb->wlr_buf->height,
				};
				set_plane_props(atom, drm, crtc->cursor, state->cursor_fb,
					crtc->id, &cursor_dst, &cursor_src);
				if (supports_cursor_hotspots(crtc->cursor)) {
					atomic_add(atom, crtc->cursor->id,
						crtc->cursor->props.hotspot_x, conn->cursor_hotspot_x);
					atomic_add(atom, crtc->cursor->id,
						crtc->cursor->props.hotspot_y, conn->cursor_hotspot_y);
				}
			} else {
				plane_disable(atom, crtc->cursor);
			}
		}

		struct wlr_drm_colorop_state *colorop_state;
		wl_array_for_each(colorop_state, &state->colorops) {
			struct wlr_drm_colorop *colorop = colorop_state->colorop;
			if (colorop->props.bypass != 0) {
				atomic_add(atom, colorop->id, colorop->props.bypass,
					colorop_state->bypass);
			}
			if (colorop->props.curve_1d_type != 0) {
				atomic_add(atom, colorop->id, colorop->props.curve_1d_type,
					colorop_state->curve_1d_type);
			}
			if (colorop->props.data != 0) {
				atomic_add(atom, colorop->id, colorop->props.data,
					colorop_state->data);
			}
		}
	} else {
		plane_disable(atom, crtc->primary);
		if (crtc->cursor) {
			plane_disable(atom, crtc->cursor);
		}
	}
}

static bool atomic_device_commit(struct wlr_drm_backend *drm,
		const struct wlr_drm_device_state *state,
		struct wlr_drm_page_flip *page_flip, uint32_t flags, bool test_only) {
	bool ok = false;

	for (size_t i = 0; i < state->connectors_len; i++) {
		if (!drm_atomic_connector_prepare(&state->connectors[i], state->modeset)) {
			goto out;
		}
	}

	struct atomic atom;
	atomic_begin(&atom);

	for (size_t i = 0; i < state->connectors_len; i++) {
		atomic_connector_add(&atom, &state->connectors[i], state->modeset);
	}

	if (test_only) {
		flags |= DRM_MODE_ATOMIC_TEST_ONLY;
	}
	if (state->modeset) {
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	}
	if (state->nonblock) {
		flags |= DRM_MODE_ATOMIC_NONBLOCK;
	}

	ok = atomic_commit(&atom, drm, state, page_flip, flags);
	atomic_finish(&atom);

out:
	for (size_t i = 0; i < state->connectors_len; i++) {
		struct wlr_drm_connector_state *conn_state = &state->connectors[i];
		if (ok && !test_only) {
			drm_atomic_connector_apply_commit(conn_state);
		} else {
			drm_atomic_connector_rollback_commit(conn_state);
		}
	}
	return ok;
}

const struct wlr_drm_interface atomic_iface = {
	.commit = atomic_device_commit,
};
