#include <assert.h>
#include <stdlib.h>
#include <pixman.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

struct scene_output_source {
	struct wlr_ext_image_capture_source_v1 base;

	struct wlr_scene *scene;
	struct wlr_output *ref_output;
	struct wlr_output_layout *layout;

	struct wlr_backend *headless_backend;
	struct wlr_output *headless;
	struct wlr_scene_output *scene_output;

	struct wl_listener headless_frame;
	struct wl_listener headless_commit;
	struct wl_listener scene_output_destroy;
	struct wl_listener ref_output_commit;
	struct wl_listener ref_output_destroy;

	size_t num_started;
};

struct scene_output_source_frame_event {
	struct wlr_ext_image_capture_source_v1_frame_event base;
	struct wlr_buffer *buffer;
	struct timespec when;
};

static void scene_output_source_update_constraints(struct scene_output_source *source) {
	struct wlr_output *output = source->ref_output;

	if (!output->enabled) {
		return;
	}

	if (!wlr_output_configure_primary_swapchain(output, NULL, &output->swapchain)) {
		return;
	}

	wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&source->base,
		output->swapchain, output->renderer);
}

static void scene_output_source_handle_ref_output_commit(struct wl_listener *listener,
		void *data) {
	struct scene_output_source *source = wl_container_of(listener, source, ref_output_commit);
	struct wlr_output_event_commit *event = data;

	if (event->state->committed & (WLR_OUTPUT_STATE_MODE |
			WLR_OUTPUT_STATE_RENDER_FORMAT | WLR_OUTPUT_STATE_ENABLED)) {
		scene_output_source_update_constraints(source);
	}
}

static void scene_output_source_handle_scene_output_destroy(struct wl_listener *listener,
		void *data) {
	struct scene_output_source *source = wl_container_of(listener, source, scene_output_destroy);
	(void)data;
	source->scene_output = NULL;
	wl_list_remove(&source->scene_output_destroy.link);
	wl_list_init(&source->scene_output_destroy.link);
}

static void scene_output_source_handle_headless_frame(struct wl_listener *listener,
		void *data) {
	struct scene_output_source *source = wl_container_of(listener, source, headless_frame);
	(void)data;

	if (source->scene_output == NULL) {
		return;
	}

	int width = source->ref_output->width;
	int height = source->ref_output->height;
	if (width <= 0 || height <= 0) {
		return;
	}

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, width, height);
	pixman_region32_copy(&source->scene_output->pending_commit_damage, &damage);
	pixman_region32_fini(&damage);

	struct wlr_scene_output_state_options options = {
		.color_transform = NULL,
	};

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, width, height, source->ref_output->refresh);
	wlr_output_state_set_scale(&state, source->ref_output->scale);
	wlr_output_state_set_transform(&state, source->ref_output->transform);
	wlr_output_state_set_render_format(&state, source->ref_output->render_format);
	if (!wlr_scene_output_build_state(source->scene_output, &state, &options)) {
		wlr_output_state_finish(&state);
		return;
	}

	wlr_output_commit_state(source->headless, &state);
	wlr_output_state_finish(&state);
}

static void scene_output_source_handle_headless_commit(struct wl_listener *listener,
		void *data) {
	struct scene_output_source *source = wl_container_of(listener, source, headless_commit);
	struct wlr_output_event_commit *event = data;

	if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER)) {
		return;
	}

	struct wlr_buffer *buffer = event->state->buffer;
	pixman_region32_t full_damage;

	const pixman_region32_t *damage;
	if (event->state->committed & WLR_OUTPUT_STATE_DAMAGE) {
		damage = &event->state->damage;
	} else {
		pixman_region32_init_rect(&full_damage, 0, 0, buffer->width, buffer->height);
		damage = &full_damage;
	}

	struct scene_output_source_frame_event frame_event = {
		.base = {
			.damage = damage,
		},
		.buffer = buffer,
		.when = event->when,
	};
	wl_signal_emit_mutable(&source->base.events.frame, &frame_event.base);

	if (damage == &full_damage) {
		pixman_region32_fini(&full_damage);
	}
}

static void scene_output_source_start(struct wlr_ext_image_capture_source_v1 *base,
		bool with_cursors) {
	struct scene_output_source *source = wl_container_of(base, source, base);
	(void)with_cursors;

	source->num_started++;
	if (source->num_started > 1) {
		return;
	}

	bool created_backend = false;
	if (source->headless_backend == NULL) {
		source->headless_backend = wlr_headless_backend_create(source->ref_output->event_loop);
		if (source->headless_backend == NULL) {
			source->num_started--;
			return;
		}
		created_backend = true;
		if (!wlr_backend_start(source->headless_backend)) {
			wlr_backend_destroy(source->headless_backend);
			source->headless_backend = NULL;
			source->num_started--;
			return;
		}
	}

	source->headless = wlr_headless_add_output(source->headless_backend,
		source->ref_output->width, source->ref_output->height);
	if (source->headless == NULL) {
		if (created_backend) {
			wlr_backend_destroy(source->headless_backend);
			source->headless_backend = NULL;
		}
		source->num_started--;
		return;
	}

	wlr_output_init_render(source->headless,
		source->ref_output->allocator, source->ref_output->renderer);

	source->scene_output = wlr_scene_output_create(source->scene, source->headless);
	if (source->scene_output == NULL) {
		wlr_output_destroy(source->headless);
		source->headless = NULL;
		if (created_backend) {
			wlr_backend_destroy(source->headless_backend);
			source->headless_backend = NULL;
		}
		source->num_started--;
		return;
	}

	if (source->layout != NULL) {
		struct wlr_box box;
		wlr_output_layout_get_box(source->layout, source->ref_output, &box);
		wlr_scene_output_set_position(source->scene_output, box.x, box.y);
	}

	source->headless_frame.notify = scene_output_source_handle_headless_frame;
	wl_signal_add(&source->headless->events.frame, &source->headless_frame);

	source->headless_commit.notify = scene_output_source_handle_headless_commit;
	wl_signal_add(&source->headless->events.commit, &source->headless_commit);

	source->scene_output_destroy.notify = scene_output_source_handle_scene_output_destroy;
	wl_signal_add(&source->scene_output->events.destroy, &source->scene_output_destroy);

	scene_output_source_update_constraints(source);
	wl_signal_emit_mutable(&source->headless->events.frame, source->headless);
}

static void scene_output_source_stop(struct wlr_ext_image_capture_source_v1 *base) {
	struct scene_output_source *source = wl_container_of(base, source, base);
	assert(source->num_started > 0);

	source->num_started--;
	if (source->num_started > 0) {
		return;
	}

	if (source->headless != NULL) {
		wl_list_remove(&source->headless_frame.link);
		wl_list_remove(&source->headless_commit.link);
		if (source->scene_output != NULL) {
			wl_list_remove(&source->scene_output_destroy.link);
			wlr_scene_output_destroy(source->scene_output);
			source->scene_output = NULL;
		}
		wlr_output_destroy(source->headless);
		source->headless = NULL;
	}

	if (source->headless_backend != NULL) {
		wlr_backend_destroy(source->headless_backend);
		source->headless_backend = NULL;
	}
}

static void scene_output_source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	struct scene_output_source *source = wl_container_of(base, source, base);
	if (schedule_frame && source->headless != NULL) {
		wlr_output_schedule_frame(source->headless);
	}
}

static void scene_output_source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_ext_image_capture_source_v1_frame_event *base_event) {
	struct scene_output_source *source = wl_container_of(base, source, base);
	struct scene_output_source_frame_event *event = wl_container_of(base_event, event, base);

	if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame,
			event->buffer, source->ref_output->renderer)) {
		wlr_ext_image_copy_capture_frame_v1_ready(frame,
			source->ref_output->transform, &event->when);
	}
}

static const struct wlr_ext_image_capture_source_v1_interface scene_output_source_impl = {
	.start = scene_output_source_start,
	.stop = scene_output_source_stop,
	.request_frame = scene_output_source_request_frame,
	.copy_frame = scene_output_source_copy_frame,
};

static void scene_output_source_destroy(struct scene_output_source *source) {
	if (source->num_started > 0) {
		scene_output_source_stop(&source->base);
	}

	wl_list_remove(&source->ref_output_commit.link);
	wl_list_remove(&source->ref_output_destroy.link);

	wlr_ext_image_capture_source_v1_finish(&source->base);
	free(source);
}

static void scene_output_source_handle_ref_output_destroy(struct wl_listener *listener,
		void *data) {
	struct scene_output_source *source = wl_container_of(listener, source, ref_output_destroy);
	(void)data;
	scene_output_source_destroy(source);
}

struct wlr_ext_image_capture_source_v1 *wlr_ext_image_capture_source_v1_create_with_scene_output(
		struct wlr_scene *scene, struct wlr_output *reference_output,
		struct wlr_output_layout *layout) {
	struct scene_output_source *source = calloc(1, sizeof(*source));
	if (source == NULL) {
		return NULL;
	}

	*source = (struct scene_output_source){
		.scene = scene,
		.ref_output = reference_output,
		.layout = layout,
	};

	wlr_ext_image_capture_source_v1_init(&source->base, &scene_output_source_impl);

	wl_list_init(&source->headless_frame.link);
	wl_list_init(&source->headless_commit.link);
	wl_list_init(&source->scene_output_destroy.link);

	source->ref_output_commit.notify = scene_output_source_handle_ref_output_commit;
	wl_signal_add(&reference_output->events.commit, &source->ref_output_commit);

	source->ref_output_destroy.notify = scene_output_source_handle_ref_output_destroy;
	wl_signal_add(&reference_output->events.destroy, &source->ref_output_destroy);

	scene_output_source_update_constraints(source);

	return &source->base;
}
