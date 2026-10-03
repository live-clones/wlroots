#include <assert.h>
#include <math.h>
#include <string.h>
#include <wlr/render/color.h>
#include "render/color.h"
#include "util/matrix.h"

static void mat3_vec(float out[static 3], const float m[static 9], const float v[static 3]) {
	float tmp[3];
	for (int i = 0; i < 3; i++) {
		tmp[i] = m[i * 3 + 0] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2];
	}
	memcpy(out, tmp, sizeof(tmp));
}

static bool mat3_equal(const float a[static 9], const float b[static 9], float epsilon) {
	for (int i = 0; i < 9; i++) {
		if (fabsf(a[i] - b[i]) > epsilon) {
			return false;
		}
	}
	return true;
}

static bool vec3_equal(const float a[static 3], const float b[static 3], float epsilon) {
	for (int i = 0; i < 3; i++) {
		if (fabsf(a[i] - b[i]) > epsilon) {
			return false;
		}
	}
	return true;
}

static void test_same_white_uses_absolute_colorimetric(void) {
	struct wlr_color_primaries srgb, bt2020;
	wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
	wlr_color_primaries_from_named(&bt2020, WLR_COLOR_NAMED_PRIMARIES_BT2020);

	float media_relative[9], absolute[9];
	wlr_color_primaries_transform(&srgb, &bt2020, media_relative);
	wlr_color_primaries_transform_absolute_colorimetric(&srgb, &bt2020, absolute);

	assert(mat3_equal(media_relative, absolute, 1e-6f));
}

static void test_white_point_adaptation(void) {
	struct wlr_color_primaries srgb_d65, srgb_a;
	wlr_color_primaries_from_named(&srgb_d65, WLR_COLOR_NAMED_PRIMARIES_SRGB);
	wlr_color_primaries_from_named(&srgb_a, WLR_COLOR_NAMED_PRIMARIES_SRGB);

	// Illuminant A
	srgb_a.white.x = 0.44757f;
	srgb_a.white.y = 0.40745f;

	float matrix[9];
	wlr_color_primaries_transform(&srgb_a, &srgb_d65, matrix);

	float white[3] = {1, 1, 1};
	float adapted_white[3];
	mat3_vec(adapted_white, matrix, white);

	assert(vec3_equal(adapted_white, white, 1e-4f));
}

static void test_bradford_colorchecker_red(void) {
	struct wlr_color_primaries src, dst;
	wlr_color_primaries_from_named(&src, WLR_COLOR_NAMED_PRIMARIES_SRGB);
	wlr_color_primaries_from_named(&dst, WLR_COLOR_NAMED_PRIMARIES_SRGB);

	// Illuminant A -> Illuminant C, matching Lindbloom's Bradford example.
	src.white = (struct wlr_color_cie1931_xy){0.44757f, 0.40745f};
	dst.white = (struct wlr_color_cie1931_xy){0.31006f, 0.31615f};

	float src_to_xyz[9], dst_to_xyz[9], xyz_to_src[9];
	wlr_color_primaries_to_xyz(&src, src_to_xyz);
	wlr_color_primaries_to_xyz(&dst, dst_to_xyz);
	matrix_invert(xyz_to_src, src_to_xyz);

	float matrix[9];
	wlr_color_primaries_transform(&src, &dst, matrix);

	float sample_a[3] = {0.315756f, 0.162732f, 0.015905f};
	float rgb_src[3];
	mat3_vec(rgb_src, xyz_to_src, sample_a);

	float rgb_dst[3];
	mat3_vec(rgb_dst, matrix, rgb_src);

	float adapted_xyz[3];
	mat3_vec(adapted_xyz, dst_to_xyz, rgb_dst);

	float expected[3] = {0.257963f, 0.139776f, 0.058825f};
	assert(vec3_equal(adapted_xyz, expected, 1e-4f));
}

int main(void) {
	test_same_white_uses_absolute_colorimetric();
	test_white_point_adaptation();
	test_bradford_colorchecker_red();
	return 0;
}
