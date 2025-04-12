#include <assert.h>
#include <string.h>
#include <wayland-server-protocol.h>
#include <wlr/util/box.h>
#include "util/matrix.h"

void matrix_multiply(float mat[static 9], const float a[static 9],
		const float b[static 9]) {
	float product[9];

	product[0] = a[0]*b[0] + a[1]*b[3] + a[2]*b[6];
	product[1] = a[0]*b[1] + a[1]*b[4] + a[2]*b[7];
	product[2] = a[0]*b[2] + a[1]*b[5] + a[2]*b[8];

	product[3] = a[3]*b[0] + a[4]*b[3] + a[5]*b[6];
	product[4] = a[3]*b[1] + a[4]*b[4] + a[5]*b[7];
	product[5] = a[3]*b[2] + a[4]*b[5] + a[5]*b[8];

	product[6] = a[6]*b[0] + a[7]*b[3] + a[8]*b[6];
	product[7] = a[6]*b[1] + a[7]*b[4] + a[8]*b[7];
	product[8] = a[6]*b[2] + a[7]*b[5] + a[8]*b[8];

	memcpy(mat, product, sizeof(product));
}

static const float transforms[][9] = {
	[WL_OUTPUT_TRANSFORM_NORMAL] = {
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_90] = {
		0.0f, 1.0f, 0.0f,
		-1.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_180] = {
		-1.0f, 0.0f, 1.0f,
		0.0f, -1.0f, 1.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_270] = {
		0.0f, -1.0f, 1.0f,
		1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED] = {
		-1.0f, 0.0f, 1.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED_90] = {
		0.0f, 1.0f, 0.0f,
		1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED_180] = {
		1.0f, 0.0f, 0.0f,
		0.0f, -1.0f, 1.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED_270] = {
		0.0f, -1.0f, 1.0f,
		-1.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 1.0f,
	},
};

void matrix_transform(float mat[static 9],
		enum wl_output_transform transform) {
	matrix_multiply(mat, mat, transforms[transform]);
}

void matrix_projection(float mat[static 9], int width, int height) {
	struct wlr_fbox fbox = {
		.x = -1.0f,
		.y = -1.0f,
		.width = 2.0f / width,
		.height = 2.0f / height,
	};

	float trans[9];
	matrix_project_fbox(trans, &fbox);
	matrix_multiply(mat, trans, mat);
}

void matrix_project_fbox(float mat[static 9], const struct wlr_fbox *box) {
	mat[0] = box->width;
	mat[1] = 0.0f;
	mat[2] = box->x;

	mat[3] = 0.0f;
	mat[4] = box->height;
	mat[5] = box->y;

	mat[6] = 0.0f;
	mat[7] = 0.0f;
	mat[8] = 1.0f;
}

void matrix_project_box(float mat[static 9], const struct wlr_box *box) {
	struct wlr_fbox fbox = {
		.x = box->x,
		.y = box->y,
		.width = box->width,
		.height = box->height,
	};

	matrix_project_fbox(mat, &fbox);
}

void matrix_invert(float out[static 9], float m[static 9]) {
	float a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];

	// See: https://en.wikipedia.org/wiki/Determinant
	float det = a*e*i + b*f*g + c*d*h - c*e*g - b*d*i - a*f*h;
	assert(det != 0);
	float inv_det = 1 / det;

	// See: https://en.wikipedia.org/wiki/Invertible_matrix#Inversion_of_3_%C3%97_3_matrices
	float result[] = {
		inv_det * (e*i - f*h),
		inv_det * -(b*i - c*h),
		inv_det * (b*f - c*e),
		inv_det * -(d*i - f*g),
		inv_det * (a*i - c*g),
		inv_det * -(a*f - c*d),
		inv_det * (d*h - e*g),
		inv_det * -(a*h - b*g),
		inv_det * (a*e - b*d),
	};
	memcpy(out, result, sizeof(result));
}
