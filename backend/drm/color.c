#include <stdlib.h>

#include "backend/drm/color.h"
#include "backend/drm/drm.h"
#include "render/color.h"

static struct wlr_color_transform_lut_3x1d *create_identity_3x1dlut(size_t dim) {
	if (dim == 0) {
		return NULL;
	}
	uint16_t *lut = malloc(dim * sizeof(lut[0]));
	if (lut == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < dim; i++) {
		float x = (float)i / (dim - 1);
		lut[i] = (uint16_t)roundf(UINT16_MAX * x);
	}
	struct wlr_color_transform *out = wlr_color_transform_init_lut_3x1d(dim, lut, lut, lut);
	free(lut);
	return color_transform_lut_3x1d_from_base(out);
}

static bool drm_crtc_color_transform_init_lut_3x1d(struct wlr_drm_crtc_color_transform *out,
		size_t dim) {
	if (out->lut_3x1d != NULL) {
		return true;
	}
	out->lut_3x1d = create_identity_3x1dlut(dim);
	return out->lut_3x1d != NULL;
}

static bool drm_crtc_color_transform_convert(struct wlr_drm_crtc_color_transform *out,
		struct wlr_color_transform *in, size_t lut_3x1d_dim) {
	switch (in->type) {
	case COLOR_TRANSFORM_INVERSE_EOTF:;
		struct wlr_color_transform_inverse_eotf *inv_eotf =
			wlr_color_transform_inverse_eotf_from_base(in);

		if (!drm_crtc_color_transform_init_lut_3x1d(out, lut_3x1d_dim)) {
			return false;
		}

		for (size_t i = 0; i < 3; i++) {
			for (size_t j = 0; j < lut_3x1d_dim; j++) {
				size_t offset = i * lut_3x1d_dim + j;
				float v = (float)out->lut_3x1d->lut_3x1d[offset] / UINT16_MAX;
				v = wlr_color_transfer_function_eval_inverse_eotf(inv_eotf->tf, v);
				out->lut_3x1d->lut_3x1d[offset] = (uint16_t)roundf(UINT16_MAX * v);
			}
		}

		return true;
	case COLOR_TRANSFORM_LUT_3X1D:;
		struct wlr_color_transform_lut_3x1d *lut_3x1d =
			color_transform_lut_3x1d_from_base(in);
		if (lut_3x1d->dim != lut_3x1d_dim) {
			return false;
		}

		if (!drm_crtc_color_transform_init_lut_3x1d(out, lut_3x1d_dim)) {
			return false;
		}

		// TODO: we loose precision when the input color transform is a lone 3×1D LUT
		for (size_t i = 0; i < 3 * lut_3x1d->dim; i++) {
			out->lut_3x1d->lut_3x1d[i] *= lut_3x1d->lut_3x1d[i];
		}

		return true;
	case COLOR_TRANSFORM_MATRIX:
		return false; // TODO: add support for CTM
	case COLOR_TRANSFORM_LCMS2:
		return false; // unsupported
	case COLOR_TRANSFORM_PIPELINE:;
		struct wlr_color_transform_pipeline *pipeline = wl_container_of(in, pipeline, base);

		for (size_t i = 0; i < pipeline->len; i++) {
			if (!drm_crtc_color_transform_convert(out, pipeline->transforms[i], lut_3x1d_dim)) {
				return false;
			}
		}

		return true;
	}
	abort(); // unreachable
}

static void addon_destroy(struct wlr_addon *addon) {
	struct wlr_drm_crtc_color_transform *tr = wl_container_of(addon, tr, addon);
	if (tr->lut_3x1d != NULL) {
		wlr_color_transform_unref(&tr->lut_3x1d->base);
	}
	free(tr);
}

static const struct wlr_addon_interface addon_impl = {
	.name = "wlr_drm_crtc_color_transform",
	.destroy = addon_destroy,
};

static struct wlr_drm_crtc_color_transform *drm_crtc_color_transform_create(
		struct wlr_drm_backend *backend, struct wlr_drm_crtc *crtc,
		struct wlr_color_transform *base) {
	struct wlr_drm_crtc_color_transform *tr = calloc(1, sizeof(*tr));
	if (tr == NULL) {
		return NULL;
	}

	tr->base = base;
	wlr_addon_init(&tr->addon, &base->addons, crtc, &addon_impl);

	size_t lut_3x1d_dim = drm_crtc_get_gamma_lut_size(backend, crtc);
	tr->failed = !drm_crtc_color_transform_convert(tr, base, lut_3x1d_dim);

	return tr;
}

struct wlr_drm_crtc_color_transform *drm_crtc_color_transform_import(
		struct wlr_drm_backend *backend, struct wlr_drm_crtc *crtc,
		struct wlr_color_transform *base) {
	struct wlr_drm_crtc_color_transform *tr;
	struct wlr_addon *addon = wlr_addon_find(&base->addons, crtc, &addon_impl);
	if (addon != NULL) {
		tr = wl_container_of(addon, tr, addon);
	} else {
		tr = drm_crtc_color_transform_create(backend, crtc, base);
		if (tr == NULL) {
			return NULL;
		}
	}

	if (tr->failed) {
		// We failed to convert the color transform to a 3×1D LUT. Keep the
		// addon attached so that we remember that this color transform cannot
		// be imported next time a commit contains it.
		return NULL;
	}

	wlr_color_transform_ref(tr->base);
	return tr;
}

void drm_crtc_color_transform_unref(struct wlr_drm_crtc_color_transform *tr) {
	if (tr == NULL) {
		return;
	}
	wlr_color_transform_unref(tr->base);
}
