#include <assert.h>
#include <drm_fourcc.h>
#include <pixman.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/util/log.h>

int main(int argc, char *argv[]) {
	struct wl_event_loop *ev = wl_event_loop_create();
	assert(ev);

	wlr_log_init(WLR_INFO, NULL);

	struct wlr_backend *backend = wlr_backend_autocreate(ev, NULL);
	assert(backend);

	struct wlr_renderer *renderer = wlr_renderer_autocreate(backend);
	assert(renderer);

	struct wlr_allocator *allocator = wlr_allocator_autocreate(backend, renderer);
	assert(allocator);

	const struct wlr_drm_format_set *formats = wlr_renderer_get_texture_formats(renderer,
		allocator->buffer_caps);

	// TODO: swapchain size from argument
	// TODO: swapchain format from argument?
	struct wlr_swapchain *swapchain = wlr_swapchain_create(allocator, 1920, 1080,
		wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888));
	assert(swapchain);

	const int iters = 16;
	for (int i = 1; i <= iters; ++i) {
		pixman_region32_t clip;
		pixman_region32_init(&clip);

		for (int ii = 0; ii < i * 10; ++ii) {
			pixman_region32_union_rect(&clip, &clip, 0, ii, ii, 1);
		}

		struct wlr_render_timer *timer = wlr_render_timer_create(renderer);
		assert(timer);

		const struct wlr_buffer_pass_options options = {
			.timer = timer,
		};

		struct wlr_buffer *buffer = wlr_swapchain_acquire(swapchain);
		assert(buffer);

		struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, buffer, &options);

		struct wlr_render_rect_options data = {
			.box = {
				.x = 0, .y = 0,
				.width = i * 10, .height = i * 10,
			},
			.clip = &clip,
			.color = { .r = 1, .g = 0.5, .b = 0.1, .a = 0.5 },
		};

		const int rects = 1 << i;

		for (int j = 0; j < rects; ++j) {
			wlr_render_pass_add_rect(pass, &data);
		}

		wlr_render_pass_submit(pass);

		wlr_buffer_unlock(buffer);

		while (!wlr_render_timer_available(timer));

		const int ns = wlr_render_timer_get_duration_ns(timer);
		wlr_log(WLR_INFO, "wlr_render_pass iteration %d/%d (%d rects): %d ns", i, iters, rects, ns);

		wlr_render_timer_destroy(timer);

		pixman_region32_fini(&clip);
	}

	wlr_swapchain_destroy(swapchain);
	wlr_allocator_destroy(allocator);
	wlr_renderer_destroy(renderer);
	wlr_backend_destroy(backend);

	wl_event_loop_destroy(ev);

	return 0;
}
