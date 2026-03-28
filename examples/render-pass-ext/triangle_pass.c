#include <assert.h>
#include <wlr/config.h>
#include <wlr/render/pixman.h>
#include <wlr/render/pass.h>
#if WLR_HAS_GLES2_RENDERER
#include <wlr/render/gles2.h>
#endif
#if WLR_HAS_VULKAN_RENDERER
#include <wlr/render/vulkan.h>
#endif
#include <wlr/render/interface.h>
#include <wlr/util/log.h>

#include "triangle_pass.h"
#include "pixman/triangle_pass.h"
#if WLR_HAS_GLES2_RENDERER
#include "gles2/triangle_pass.h"
#endif

void render_triangle_pass_init(struct render_triangle_pass *pass,
		const struct render_triangle_pass_impl *impl) {
	assert(impl->render);
	*pass = (struct render_triangle_pass){
		.impl = impl,
	};

	wl_signal_init(&pass->events.destroy);
}

void render_triangle_pass_destroy(struct render_triangle_pass *pass) {
	if (pass == NULL) {
		return;
	}

	pass->impl->destroy(pass);
}

struct render_triangle_pass *get_or_create_render_triangle_pass(
		struct wlr_renderer *renderer) {
	if (renderer == NULL) {
		return NULL;
	}

	if (renderer->data == NULL) {
		struct render_triangle_pass *pass = NULL;
		if (wlr_renderer_is_pixman(renderer)) {
			pass = pixman_render_triangle_pass_create(renderer);
		}

#if WLR_HAS_GLES2_RENDERER
		else if (wlr_renderer_is_gles2(renderer)) {
			pass = gles2_render_triangle_pass_create(renderer);
		}
#endif

// #if WLR_HAS_VULKAN_RENDERER
// 		else if (wlr_renderer_is_vk(renderer)) {
// 			pass = wlr_vk_render_triangle_pass_create(renderer);
// 		}
// #endif

		renderer->data = pass;
		return pass;
	} else {
		return renderer->data;
	}
}

void render_triangle_pass_add(struct wlr_render_pass *render_pass,
	const struct custom_render_triangle_options *options) {
	struct wlr_renderer *renderer =
		wlr_get_wlr_renderer_from_render_pass(render_pass);

	struct render_triangle_pass *pass = renderer->data;
	if (pass == NULL) {
		wlr_log(WLR_ERROR, "No triangle pass is available for this renderer");
		return;
	}
	return pass->impl->render(render_pass, options);
}

void custom_render_triangle_options_get_box(const struct custom_render_triangle_options *options,
		const struct wlr_buffer *buffer, struct wlr_box *box) {
	if (wlr_box_empty(&options->box)) {
		*box = (struct wlr_box){
			.width = buffer->width,
			.height = buffer->height,
		};

		return;
	}

	*box = options->box;
}

