#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/color.h>
#include "render/color.h"
#include "util/matrix.h"

// See H.273 ColourPrimaries

static const struct wlr_color_primaries COLOR_PRIMARIES_SRGB = { // code point 1
	.red = { 0.640, 0.330 },
	.green = { 0.300, 0.600 },
	.blue = { 0.150, 0.060 },
	.white = { 0.3127, 0.3290 },
};

static const struct wlr_color_primaries COLOR_PRIMARIES_BT2020 = { // code point 9
	.red = { 0.708, 0.292 },
	.green = { 0.170, 0.797 },
	.blue = { 0.131, 0.046 },
	.white = { 0.3127, 0.3290 },
};

void wlr_color_transform_init(struct wlr_color_transform *tr, enum wlr_color_transform_type type) {
	*tr = (struct wlr_color_transform){
		.type = type,
		.ref_count = 1,
	};
	wlr_addon_set_init(&tr->addons);
}

struct wlr_color_transform *wlr_color_transform_init_linear_to_inverse_eotf(
		enum wlr_color_transfer_function tf) {
	struct wlr_color_transform_inverse_eotf *tx = calloc(1, sizeof(*tx));
	if (!tx) {
		return NULL;
	}
	wlr_color_transform_init(&tx->base, COLOR_TRANSFORM_INVERSE_EOTF);
	tx->tf = tf;
	return &tx->base;
}

struct wlr_color_transform *wlr_color_transform_init_lut_3x1d(size_t dim,
		const uint16_t *r, const uint16_t *g, const uint16_t *b) {
	uint16_t *lut_3x1d = malloc(3 * dim * sizeof(lut_3x1d[0]));
	if (lut_3x1d == NULL) {
		return NULL;
	}

	memcpy(&lut_3x1d[0 * dim], r, dim * sizeof(lut_3x1d[0]));
	memcpy(&lut_3x1d[1 * dim], g, dim * sizeof(lut_3x1d[0]));
	memcpy(&lut_3x1d[2 * dim], b, dim * sizeof(lut_3x1d[0]));

	struct wlr_color_transform_lut_3x1d *tx = calloc(1, sizeof(*tx));
	if (!tx) {
		free(lut_3x1d);
		return NULL;
	}
	wlr_color_transform_init(&tx->base, COLOR_TRANSFORM_LUT_3X1D);
	tx->lut_3x1d = lut_3x1d;
	tx->dim = dim;
	return &tx->base;
}

static void color_transform_destroy(struct wlr_color_transform *tr) {
	switch (tr->type) {
	case COLOR_TRANSFORM_INVERSE_EOTF:
		break;
	case COLOR_TRANSFORM_LCMS2:
		color_transform_lcms2_finish(color_transform_lcms2_from_base(tr));
		break;
	case COLOR_TRANSFORM_LUT_3X1D:;
		struct wlr_color_transform_lut_3x1d *lut_3x1d = color_transform_lut_3x1d_from_base(tr);
		free(lut_3x1d->lut_3x1d);
		break;
	}
	wlr_addon_set_finish(&tr->addons);
	free(tr);
}

struct wlr_color_transform *wlr_color_transform_ref(struct wlr_color_transform *tr) {
	tr->ref_count += 1;
	return tr;
}

void wlr_color_transform_unref(struct wlr_color_transform *tr) {
	if (!tr) {
		return;
	}
	assert(tr->ref_count > 0);
	tr->ref_count -= 1;
	if (tr->ref_count == 0) {
		color_transform_destroy(tr);
	}
}

struct wlr_color_transform_inverse_eotf *wlr_color_transform_inverse_eotf_from_base(
		struct wlr_color_transform *tr) {
	assert(tr->type == COLOR_TRANSFORM_INVERSE_EOTF);
	struct wlr_color_transform_inverse_eotf *inverse_eotf = wl_container_of(tr, inverse_eotf, base);
	return inverse_eotf;
}

struct wlr_color_transform_lut_3x1d *color_transform_lut_3x1d_from_base(
		struct wlr_color_transform *tr) {
	assert(tr->type == COLOR_TRANSFORM_LUT_3X1D);
	struct wlr_color_transform_lut_3x1d *lut_3x1d = wl_container_of(tr, lut_3x1d, base);
	return lut_3x1d;
}

static float lut_1d_get(const uint16_t *lut, size_t len, size_t i) {
	if (i > len) {
		i = len - 1;
	}
	return (float) lut[i] / UINT16_MAX;
}

static float lut_1d_eval(const uint16_t *lut, size_t len, float x) {
	double pos = x * (len - 1);
	double int_part;
	double frac_part = modf(pos, &int_part);
	size_t i = (size_t) int_part;
	double a = lut_1d_get(lut, len, i);
	double b = lut_1d_get(lut, len, i + 1);
	return a * (1 - frac_part) + b * frac_part;
}

void color_transform_lut_3x1d_eval(struct wlr_color_transform_lut_3x1d *tr,
		float out[static 3], const float in[static 3]) {
	for (size_t i = 0; i < 3; i++) {
		out[i] = lut_1d_eval(&tr->lut_3x1d[tr->dim * i], tr->dim, in[i]);
	}
}

void wlr_color_primaries_from_named(struct wlr_color_primaries *out,
		enum wlr_color_named_primaries named) {
	switch (named) {
	case WLR_COLOR_NAMED_PRIMARIES_SRGB:
		*out = COLOR_PRIMARIES_SRGB;
		return;
	case WLR_COLOR_NAMED_PRIMARIES_BT2020:
		*out = COLOR_PRIMARIES_BT2020;
		return;
	}
	abort();
}

static void multiply_matrix_vector(float out[static 3], float m[static 9], float v[static 3]) {
	float result[3] = {
		m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
		m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
		m[6] * v[0] + m[7] * v[1] + m[8] * v[2],
	};
	memcpy(out, result, sizeof(result));
}

static void xy_to_xyz(float out[static 3], struct wlr_color_cie1931_xy src) {
	if (src.y == 0) {
		out[0] = out[1] = out[2] = 0;
		return;
	}

	out[0] = src.x / src.y;
	out[1] = 1;
	out[2] = (1 - src.x - src.y) / src.y;
}

void wlr_color_primaries_to_xyz(const struct wlr_color_primaries *primaries, float matrix[static 9]) {
	// See: http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html

	float r[3], g[3], b[3], w[3];
	xy_to_xyz(r, primaries->red);
	xy_to_xyz(g, primaries->green);
	xy_to_xyz(b, primaries->blue);
	xy_to_xyz(w, primaries->white);

	float xyz_matrix[9] = {
		r[0], g[0], b[0],
		r[1], g[1], b[1],
		r[2], g[2], b[2],
	};
	matrix_invert(xyz_matrix, xyz_matrix);

	float S[3];
	multiply_matrix_vector(S, xyz_matrix, w);

	float result[] = {
		S[0] * r[0], S[1] * g[0], S[2] * b[0],
		S[0] * r[1], S[1] * g[1], S[2] * b[1],
		S[0] * r[2], S[1] * g[2], S[2] * b[2],
	};
	memcpy(matrix, result, sizeof(result));
}

void wlr_color_transfer_function_get_default_luminance(enum wlr_color_transfer_function tf,
		struct wlr_color_luminances *lum) {
	switch (tf) {
	case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		*lum = (struct wlr_color_luminances){
			.min = 0.005,
			.max = 10000,
			.reference = 203,
		};
		break;
	default:
		*lum = (struct wlr_color_luminances){
			.min = 0.2,
			.max = 80,
			.reference = 80,
		};
		break;
	}
}
