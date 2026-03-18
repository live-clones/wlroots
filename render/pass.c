#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <wlr/render/interface.h>
#include <wlr/util/log.h>
#include <wlr/render/pixman.h>

#include <wlr/config.h>

#if WLR_HAS_GLES2_RENDERER
#include <wlr/render/gles2.h>
#endif

#if WLR_HAS_VULKAN_RENDERER
#include <wlr/render/vulkan.h>
#endif

void wlr_render_pass_init(struct wlr_render_pass *pass,
		const struct wlr_render_pass_impl *impl) {
	assert(impl->destroy);
	*pass = (struct wlr_render_pass){
		.impl = impl,
	};
}

void wlr_render_pass_destroy(struct wlr_render_pass *pass) {
	if (pass == NULL) {
		return;
	}

	pass->impl->destroy(pass);
}

struct wlr_renderer *wlr_get_wlr_renderer_from_render_pass(
		struct wlr_render_pass *wlr_pass) {
	return wlr_pass->impl->get_renderer(wlr_pass);
}

bool wlr_render_pass_submit(struct wlr_render_pass *render_pass) {
	struct wlr_renderer *renderer =
		wlr_get_wlr_renderer_from_render_pass(render_pass);

	return renderer->submit_pass->impl->render(render_pass);
}

void wlr_render_pass_add_texture(struct wlr_render_pass *render_pass,
		const struct wlr_render_texture_options *options) {
	// make sure the texture source box does not try and sample outside of the
	// texture
	if (!wlr_fbox_empty(&options->src_box)) {
		const struct wlr_fbox *box = &options->src_box;
		assert(box->x >= 0 && box->y >= 0 &&
		(uint32_t)(box->x + box->width) <= options->texture->width &&
		(uint32_t)(box->y + box->height) <= options->texture->height);
	}

	struct wlr_renderer *renderer =
		wlr_get_wlr_renderer_from_render_pass(render_pass);

	renderer->texture_pass->impl->render(render_pass, options);
}

void wlr_render_pass_add_rect(struct wlr_render_pass *render_pass,
		const struct wlr_render_rect_options *options) {
	assert(options->box.width >= 0 && options->box.height >= 0);
	struct wlr_renderer *renderer =
		wlr_get_wlr_renderer_from_render_pass(render_pass);

	renderer->rect_pass->impl->render(render_pass, options);
}

void wlr_render_texture_options_get_src_box(const struct wlr_render_texture_options *options,
		struct wlr_fbox *box) {
	*box = options->src_box;
	if (wlr_fbox_empty(box)) {
		*box = (struct wlr_fbox){
			.width = options->texture->width,
			.height = options->texture->height,
		};
	}
}

void wlr_render_texture_options_get_dst_box(const struct wlr_render_texture_options *options,
		struct wlr_box *box) {
	*box = options->dst_box;
	if (wlr_box_empty(box)) {
		box->width = options->texture->width;
		box->height = options->texture->height;
	}
}

float wlr_render_texture_options_get_alpha(const struct wlr_render_texture_options *options) {
	if (options->alpha == NULL) {
		return 1;
	}
	return *options->alpha;
}

void wlr_render_rect_options_get_box(const struct wlr_render_rect_options *options,
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

void wlr_render_rect_pass_init(struct wlr_render_rect_pass *render_pass,
	const struct wlr_render_rect_pass_impl *impl) {
	assert(impl->render);
	*render_pass = (struct wlr_render_rect_pass){
		.impl = impl,
	};
	wl_signal_init(&render_pass->events.destroy);
}

void wlr_render_rect_pass_destroy(struct wlr_render_rect_pass *render_pass) {
	if (render_pass == NULL) {
		return;
	}

	wl_signal_emit_mutable(&render_pass->events.destroy, NULL);
	assert(wl_list_empty(&render_pass->events.destroy.listener_list));

	if (render_pass->impl->destroy != NULL) {
		render_pass->impl->destroy(render_pass);
	} else {
		free(render_pass);
	}
}

struct wlr_render_rect_pass *get_or_create_render_rect_pass(
		struct wlr_renderer *renderer) {
	if (renderer == NULL) {
		return NULL;
	}

	if (renderer->rect_pass == NULL) {
		struct wlr_render_rect_pass *pass = NULL;
		if (wlr_renderer_is_pixman(renderer)) {
			pass = wlr_pixman_render_rect_pass_create();
		}

#if WLR_HAS_GLES2_RENDERER
		else if (wlr_renderer_is_gles2(renderer)) {
			pass = wlr_gles2_render_rect_pass_create(renderer);
		}
#endif

#if WLR_HAS_VULKAN_RENDERER
		else if (wlr_renderer_is_vk(renderer)) {
			pass = wlr_vk_render_rect_pass_create();
		}
#endif

		renderer->rect_pass = pass;
		return pass;
	} else {
		return renderer->rect_pass;
	}
}

void wlr_render_texture_pass_init(struct wlr_render_texture_pass *render_pass,
			const struct wlr_render_texture_pass_impl *impl) {
		assert(impl->render);
		*render_pass = (struct wlr_render_texture_pass){
			.impl = impl,
		};
		wl_signal_init(&render_pass->events.destroy);
}
void wlr_render_texture_pass_destroy(struct wlr_render_texture_pass *render_pass) {
	if (render_pass == NULL) {
		return;
	}

	wl_signal_emit_mutable(&render_pass->events.destroy, NULL);
	assert(wl_list_empty(&render_pass->events.destroy.listener_list));

	if (render_pass->impl->destroy != NULL) {
		render_pass->impl->destroy(render_pass);
	}
}

struct wlr_render_texture_pass *get_or_create_render_texture_pass(
		struct wlr_renderer *renderer) {
	if (renderer == NULL) {
		return NULL;
	}

	if (renderer->texture_pass == NULL) {
		struct wlr_render_texture_pass *pass = NULL;
		if (wlr_renderer_is_pixman(renderer)) {
			pass = wlr_pixman_render_texture_pass_create();
		}

#if WLR_HAS_GLES2_RENDERER
		else if (wlr_renderer_is_gles2(renderer)) {
			pass = wlr_gles2_render_texture_pass_create(renderer);
		}
#endif

#if WLR_HAS_VULKAN_RENDERER
		else if (wlr_renderer_is_vk(renderer)) {
			pass = wlr_vk_render_texture_pass_create();
		}
#endif

		renderer->texture_pass = pass;
		return pass;
	} else {
		return renderer->texture_pass;
	}
}

void wlr_render_submit_pass_init(struct wlr_render_submit_pass *pass,
		const struct wlr_render_submit_pass_impl *impl) {
	assert(impl->render);
	*pass = (struct wlr_render_submit_pass){
		.impl = impl,
	};
	wl_signal_init(&pass->events.destroy);
}

void wlr_render_submit_pass_destroy(struct wlr_render_submit_pass *pass) {
	if (pass == NULL) {
		return;
	}

	wl_signal_emit_mutable(&pass->events.destroy, NULL);
	assert(wl_list_empty(&pass->events.destroy.listener_list));

	if (pass->impl->destroy != NULL) {
		pass->impl->destroy(pass);
	}
}

struct wlr_render_submit_pass *get_or_create_render_submit_pass(
		struct wlr_renderer *renderer) {
	if (renderer == NULL) {
		return NULL;
	}

	if (renderer->submit_pass == NULL) {
		struct wlr_render_submit_pass *pass = NULL;
		if (wlr_renderer_is_pixman(renderer)) {
			pass = wlr_pixman_render_submit_pass_create();
		}

#if WLR_HAS_GLES2_RENDERER
		else if (wlr_renderer_is_gles2(renderer)) {
			pass = wlr_gles2_render_submit_pass_create();
		}
#endif

#if WLR_HAS_VULKAN_RENDERER
		else if (wlr_renderer_is_vk(renderer)) {
			pass = wlr_vk_render_submit_pass_create();
		}
#endif

		if (pass == NULL) {
			wlr_log_errno(WLR_ERROR, "Failed to allocate wlr_render_submit_pass");
			return NULL;
		}

		renderer->submit_pass = pass;
		return pass;
	} else {
		return renderer->submit_pass;
	}
}
