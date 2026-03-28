#include <stdlib.h>
#include <assert.h>

#include <wlr/render/pixman.h>
#include <wlr/util/log.h>

#include "render/pixman.h"
#include "triangle_pass.h"

static bool triangle_contains(float px, float py,
		float x0, float y0, float x1, float y1, float x2, float y2,
		float *w0, float *w1, float *w2) {
	float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
	if (denom == 0.0f) {
		return false;
	}

	*w0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / denom;
	*w1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / denom;
	*w2 = 1.0f - *w0 - *w1;
	return *w0 >= 0.0f && *w1 >= 0.0f && *w2 >= 0.0f;
}

static void custom_triangle_pixman_render(struct wlr_render_pass *render_pass,
		const struct custom_render_triangle_options *options) {
	struct wlr_pixman_render_pass *pixman_pass =
		wlr_pixman_render_pass_from_render_pass(render_pass);
	pixman_image_t *dst = pixman_pass->buffer->image;
	int w = options->box.width;
	int h = options->box.height;
	if (w <= 0 || h <= 0) {
		pixman_image_set_clip_region32(dst, NULL);
		return;
	}

	uint32_t *pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
	if (pixels == NULL) {
		pixman_image_set_clip_region32(dst, NULL);
		return;
	}

	float x0 = w * 0.50f, y0 = h * 0.10f;
	float x1 = w * 0.10f, y1 = h * 0.90f;
	float x2 = w * 0.90f, y2 = h * 0.90f;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float w0, w1, w2;
			if (!triangle_contains(x + 0.5f, y + 0.5f,
					x0, y0, x1, y1, x2, y2, &w0, &w1, &w2)) {
				continue;
			}

			uint8_t r = (uint8_t)(w0 * 255.0f);
			uint8_t g = (uint8_t)(w1 * 255.0f);
			uint8_t b = (uint8_t)(w2 * 255.0f);
			pixels[y * w + x] = 0xFF000000u | ((uint32_t)r << 16) |
				((uint32_t)g << 8) | (uint32_t)b;
		}
	}

	pixman_image_t *triangle = pixman_image_create_bits(PIXMAN_a8r8g8b8,
		w, h, pixels, w * sizeof(uint32_t));
	pixman_image_composite32(PIXMAN_OP_OVER, triangle, NULL, dst,
		0, 0, 0, 0, options->box.x, options->box.y, w, h);
	pixman_image_unref(triangle);
	pixman_image_set_clip_region32(dst, NULL);
	free(pixels);
}

static void custom_triangle_pixman_destroy(struct render_triangle_pass *pass) {
	struct pixman_render_triangle_pass *pixman_pass =
		pixman_render_triangle_pass_from_pass(pass);
	free(pixman_pass);
}

static const struct render_triangle_pass_impl render_triangle_pass_impl = {
	.destroy = custom_triangle_pixman_destroy,
	.render = custom_triangle_pixman_render,
};

struct render_triangle_pass *pixman_render_triangle_pass_create(
		struct wlr_renderer *renderer) {
	struct pixman_render_triangle_pass *pass = malloc(sizeof(*pass));
	if (pass == NULL) {
		wlr_log_errno(WLR_ERROR, "failed to allocate wlr_pixman_render_submit_pass");
		return NULL;
	}

	render_triangle_pass_init(&pass->base, &render_triangle_pass_impl);

	return &pass->base;
}

bool render_triangle_pass_is_pixman(const struct render_triangle_pass *triangle_pass) {
	return triangle_pass->impl == &render_triangle_pass_impl;
}

struct pixman_render_triangle_pass *pixman_render_triangle_pass_from_pass(
		struct render_triangle_pass *triangle_pass) {
	assert(render_triangle_pass_is_pixman(triangle_pass));
	struct pixman_render_triangle_pass *pixman_pass =
		wl_container_of(triangle_pass, pixman_pass, base);

	return pixman_pass;
}

