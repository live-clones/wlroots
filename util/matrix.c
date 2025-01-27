#include <string.h>
#include <wayland-server-protocol.h>
#include <wlr/util/box.h>
#include "util/matrix.h"

void wlr_matrix_multiply(float mat[static 9], const float a[static 9],
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

void wlr_matrix_transform(float mat[static 9],
		enum wl_output_transform transform) {
	wlr_matrix_multiply(mat, mat, transforms[transform]);
}

void matrix_projection(float mat[static 9], int width, int height) {
	memset(mat, 0, sizeof(*mat) * 9);

	mat[0] = 2.0f / width;
	mat[4] = 2.0f / height;

	// Translation
	mat[2] = -copysign(1.0f, mat[0] + mat[1]);
	mat[5] = -copysign(1.0f, mat[3] + mat[4]);

	// Identity
	mat[8] = 1.0f;
}

void wlr_matrix_project_fbox(float mat[static 9], const struct wlr_fbox *box) {
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

void wlr_matrix_project_box(float mat[static 9], const struct wlr_box *box) {
	struct wlr_fbox fbox = {
		.x = box->x,
		.y = box->y,
		.width = box->width,
		.height = box->height,
	};

	wlr_matrix_project_fbox(mat, &fbox);
}
