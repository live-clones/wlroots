#include <drm_fourcc.h>
#include <stdlib.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/gles2.h>
#include <wlr/backend/session.h>
#include <fcntl.h>
#include <stdio.h>
#include <pixman.h>
#include <unistd.h>

#define BENCH_NUM 100000

struct wlr_allocator *wlr_gbm_allocator_create(int drm_fd);

const struct wlr_drm_format_set *wlr_renderer_get_render_formats(
	struct wlr_renderer *renderer);

static void render_rect(struct wlr_render_pass *pass, void *_data) {
	struct wlr_render_rect_options *data = _data;

	wlr_render_pass_add_rect(pass, data);
}

static void run_bench(struct wlr_renderer *renderer, struct wlr_buffer *buffer,
		void (*bench)(struct wlr_render_pass *, void *), void *data) {
	struct wlr_render_timer *timer = wlr_render_timer_create(renderer);
	//struct timespec start, end;
	//clock_gettime(CLOCK_MONOTONIC_RAW, &start);

	struct wlr_buffer_pass_options options = {
		.timer = timer,
	};

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, buffer, &options);

	for (int i = 0; i < BENCH_NUM; i++) {
		bench(pass, data);
	}

	wlr_render_pass_submit(pass);

	//clock_gettime(CLOCK_MONOTONIC_RAW, &end);

	//uint64_t delta_us = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
	printf("%d\n", wlr_render_timer_get_duration_ns(timer));

	wlr_render_timer_destroy(timer);
}

int main(int argc, char *argv[]) {
	const char *render_device = getenv("RENDER_DEVICE");
	if (render_device == NULL) {
		render_device = "/dev/dri/card0";
	}

	int drm_fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);

	struct wlr_allocator *alloc = wlr_gbm_allocator_create(drm_fd);
	struct wlr_renderer *renderer = wlr_gles2_renderer_create_with_drm_fd(drm_fd);

	const struct wlr_drm_format_set *set = wlr_renderer_get_render_formats(renderer);
	struct wlr_buffer *buffer = wlr_allocator_create_buffer(alloc, 1920, 1080, wlr_drm_format_set_get(set, DRM_FORMAT_ARGB8888));

	for (int i = 0; i < 10; i++) {
		pixman_region32_t clip;
		pixman_region32_init(&clip);

		for (int ii = 0; ii < i * 10; ii++) {
			pixman_region32_union_rect(&clip, &clip, 0, ii, ii, 1);
		}

		struct wlr_render_rect_options data = {
			.box = {
				.x = 0, .y = 0,
				.width = i * 10, .height = i * 10,
			},
			.clip = &clip,
			.color = { .r = 1, .g = 0.5, .b = 0.1, .a = 0.5 },
		};

		run_bench(renderer, buffer, &render_rect, &data);

		pixman_region32_fini(&clip);
	}

	close(drm_fd);

	return 0;
}
