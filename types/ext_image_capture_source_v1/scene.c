#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "types/wlr_output.h"
#include "types/wlr_scene.h"

struct scene_source_interface;

struct scene_source {
	struct wlr_ext_image_capture_source_v1 base;

	const struct scene_source_interface *impl;

	struct wlr_backend backend;
	struct wlr_output output;
	struct wlr_scene_output *scene_output;

	size_t num_started;
	bool with_cursors;

	struct wl_listener scene_output_destroy;
	struct wl_listener output_frame;
};

struct scene_source_interface {
	void (*update_extents)(struct scene_source *source, struct wlr_box *extents);
};

struct scene_node_source_frame_event {
	struct wlr_ext_image_capture_source_v1_frame_event base;
	struct wlr_buffer *buffer;
	struct timespec when;
};

static size_t last_output_num = 0;

static void _get_scene_node_extents(struct wlr_scene_node *node, int lx, int ly,
		int *x_min, int *y_min, int *x_max, int *y_max) {
	switch (node->type) {
	case WLR_SCENE_NODE_TREE:;
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			_get_scene_node_extents(child, lx + child->x, ly + child->y, x_min, y_min, x_max, y_max);
		}
		break;
	case WLR_SCENE_NODE_RECT:
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_box node_box = { .x = lx, .y = ly };
		scene_node_get_size(node, &node_box.width, &node_box.height);

		if (node_box.x < *x_min) {
			*x_min = node_box.x;
		}
		if (node_box.y < *y_min) {
			*y_min = node_box.y;
		}
		if (node_box.x + node_box.width > *x_max) {
			*x_max = node_box.x + node_box.width;
		}
		if (node_box.y + node_box.height > *y_max) {
			*y_max = node_box.y + node_box.height;
		}
		break;
	}
}

static void get_scene_node_extents(struct wlr_scene_node *node, struct wlr_box *box) {
	int lx = 0, ly = 0;
	wlr_scene_node_coords(node, &lx, &ly);
	*box = (struct wlr_box){ .x = INT_MAX, .y = INT_MAX };
	int x_max = INT_MIN, y_max = INT_MIN;
	_get_scene_node_extents(node, lx, ly, &box->x, &box->y, &x_max, &y_max);
	box->width = x_max - box->x;
	box->height = y_max - box->y;
}

static void source_render(struct scene_source *source) {
	struct wlr_scene_output *scene_output = source->scene_output;

	struct wlr_box extents;
	source->impl->update_extents(source, &extents);

	if (extents.width == 0 || extents.height == 0) {
		return;
	}

	wlr_scene_output_set_position(scene_output, extents.x, extents.y);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, extents.width, extents.height, 0);
	bool ok = wlr_scene_output_build_state(scene_output, &state, NULL) &&
		wlr_output_commit_state(scene_output->output, &state);
	wlr_output_state_finish(&state);

	if (!ok) {
		// TODO: send failure
		return;
	}
}

static void source_start(struct wlr_ext_image_capture_source_v1 *base, bool with_cursors) {
	struct scene_source *source = wl_container_of(base, source, base);

	source->num_started++;
	if (source->num_started > 1) {
		return;
	}

	source->with_cursors = with_cursors;

	source_render(source);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(source->scene_output, &now);
}

static void source_stop(struct wlr_ext_image_capture_source_v1 *base) {
	struct scene_source *source = wl_container_of(base, source, base);

	source->num_started--;
	if (source->num_started > 0) {
		return;
	}

	if (source->with_cursors) {
		source->with_cursors = false;
		struct wlr_box extents;
		source->impl->update_extents(source, &extents);
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, false);
	wlr_output_commit_state(&source->output, &state);
	wlr_output_state_finish(&state);
}

static void source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	struct scene_source *source = wl_container_of(base, source, base);
	if (source->output.frame_pending) {
		wlr_output_send_frame(&source->output);
	}
	if (schedule_frame) {
		wlr_output_update_needs_frame(&source->output);
	}
}

static void source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *frame,
		struct wlr_ext_image_capture_source_v1_frame_event *base_event) {
	struct scene_source *source = wl_container_of(base, source, base);
	struct scene_node_source_frame_event *event = wl_container_of(base_event, event, base);

	if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame,
			event->buffer, source->output.renderer)) {
		wlr_ext_image_copy_capture_frame_v1_ready(frame,
			source->output.transform, &event->when);
	}
}

static const struct wlr_ext_image_capture_source_v1_interface source_impl = {
	.start = source_start,
	.stop = source_stop,
	.request_frame = source_request_frame,
	.copy_frame = source_copy_frame,
};

static const struct wlr_backend_impl backend_impl = {0};

static void source_update_buffer_constraints(struct scene_source *source,
		const struct wlr_output_state *state) {
	struct wlr_output *output = &source->output;

	if (!wlr_output_configure_primary_swapchain(output, state, &output->swapchain)) {
		return;
	}

	wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&source->base,
		output->swapchain, output->renderer);
}

static bool output_test(struct wlr_output *output, const struct wlr_output_state *state) {
	uint32_t supported =
		WLR_OUTPUT_STATE_BACKEND_OPTIONAL |
		WLR_OUTPUT_STATE_BUFFER |
		WLR_OUTPUT_STATE_ENABLED |
		WLR_OUTPUT_STATE_MODE;
	if ((state->committed & ~supported) != 0) {
		return false;
	}

	if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
		int pending_width, pending_height;
		output_pending_resolution(output, state,
			&pending_width, &pending_height);
		if (state->buffer->width != pending_width ||
				state->buffer->height != pending_height) {
			return false;
		}
		struct wlr_fbox src_box;
		output_state_get_buffer_src_box(state, &src_box);
		if (src_box.x != 0.0 || src_box.y != 0.0 ||
				src_box.width != (double)state->buffer->width ||
				src_box.height != (double)state->buffer->height) {
			return false;
		}
	}

	return true;
}

static bool output_commit(struct wlr_output *output, const struct wlr_output_state *state) {
	struct scene_source *source = wl_container_of(output, source, output);

	if ((state->committed & WLR_OUTPUT_STATE_ENABLED) && !state->enabled) {
		return true;
	}

	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		source_update_buffer_constraints(source, state);
	}

	if (!(state->committed & WLR_OUTPUT_STATE_BUFFER)) {
		wlr_log(WLR_DEBUG, "Failed to commit capture output: missing buffer");
		return false;
	}

	struct wlr_buffer *buffer = state->buffer;

	pixman_region32_t full_damage;
	pixman_region32_init_rect(&full_damage, 0, 0, buffer->width, buffer->height);

	const pixman_region32_t *damage;
	if (state->committed & WLR_OUTPUT_STATE_DAMAGE) {
		damage = &state->damage;
	} else {
		damage = &full_damage;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	struct scene_node_source_frame_event frame_event = {
		.base = { .damage = damage },
		.buffer = buffer,
		.when = now,
	};
	wl_signal_emit_mutable(&source->base.events.frame, &frame_event.base);

	pixman_region32_fini(&full_damage);

	return true;
}

static const struct wlr_output_impl output_impl = {
	.test = output_test,
	.commit = output_commit,
};

static void source_finish(struct scene_source *source) {
	wl_list_remove(&source->scene_output_destroy.link);
	wl_list_remove(&source->output_frame.link);
	wlr_ext_image_capture_source_v1_finish(&source->base);
	wlr_scene_output_destroy(source->scene_output);
	wlr_output_finish(&source->output);
	wlr_backend_finish(&source->backend);
}

static void source_handle_scene_output_destroy(struct wl_listener *listener, void *data) {
	struct scene_source *source = wl_container_of(listener, source, scene_output_destroy);
	source->scene_output = NULL;
	wl_list_remove(&source->scene_output_destroy.link);
	wl_list_init(&source->scene_output_destroy.link);
}

static void source_handle_output_frame(struct wl_listener *listener, void *data) {
	struct scene_source *source = wl_container_of(listener, source, output_frame);
	if (source->scene_output == NULL) {
		return;
	}

	if (!wlr_scene_output_needs_frame(source->scene_output)) {
		return;
	}

	// We can only emit frames with damage
	if (!pixman_region32_empty(&source->scene_output->pending_commit_damage)) {
		source_render(source);
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(source->scene_output, &now);
}

static void source_init(struct scene_source *source, struct wlr_scene *scene,
		struct wl_event_loop *event_loop, struct wlr_allocator *allocator,
		struct wlr_renderer *renderer) {
	wlr_ext_image_capture_source_v1_init(&source->base, &source_impl);

	wlr_backend_init(&source->backend, &backend_impl);
	source->backend.buffer_caps = WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM;

	wlr_output_init(&source->output, &source->backend, &output_impl, event_loop, NULL);

	size_t output_num = ++last_output_num;
	char name[64];
	snprintf(name, sizeof(name), "CAPTURE-%zu", output_num);
	wlr_output_set_name(&source->output, name);

	wlr_output_init_render(&source->output, allocator, renderer);

	source->scene_output = wlr_scene_output_create(scene, &source->output);

	source->scene_output_destroy.notify = source_handle_scene_output_destroy;
	wl_signal_add(&source->scene_output->events.destroy, &source->scene_output_destroy);

	source->output_frame.notify = source_handle_output_frame;
	wl_signal_add(&source->output.events.frame, &source->output_frame);
}

struct scene_node_source {
	struct scene_source base;
	struct wlr_scene_node *node;

	struct wl_listener node_destroy;
};

static void scene_node_source_update_extents(struct scene_source *source,
		struct wlr_box *extents) {
	struct scene_node_source *node_source = wl_container_of(source, node_source, base);
	get_scene_node_extents(node_source->node, extents);
}

static struct scene_source_interface scene_node_source_impl = {
	.update_extents = scene_node_source_update_extents,
};

static void node_source_handle_node_destroy(struct wl_listener *listener, void *data) {
	struct scene_node_source *source = wl_container_of(listener, source, node_destroy);
	wl_list_remove(&source->node_destroy.link);
	source_finish(&source->base);
	free(source);
}

struct wlr_ext_image_capture_source_v1 *wlr_ext_image_capture_source_v1_create_with_scene_node(
		struct wlr_scene_node *node, struct wl_event_loop *event_loop,
		struct wlr_allocator *allocator, struct wlr_renderer *renderer) {
	struct scene_node_source *source = calloc(1, sizeof(*source));
	if (source == NULL) {
		return NULL;
	}

	struct wlr_scene *scene = scene_node_get_root(node);

	source_init(&source->base, scene, event_loop, allocator, renderer);
	source->base.impl = &scene_node_source_impl;

	source->node = node;

	source->node_destroy.notify = node_source_handle_node_destroy;
	wl_signal_add(&node->events.destroy, &source->node_destroy);

	return &source->base.base;
}

struct scene_output_source {
	struct scene_source base;

	struct wlr_output *ref_output;
	struct wlr_output_layout *ref_output_layout;

	struct wl_listener ref_output_destroy;
	struct wl_listener ref_output_layout_destroy;
};

static void scene_output_source_update_extents(struct scene_source *source,
		struct wlr_box *extents) {
	struct scene_output_source *output_source = wl_container_of(source, output_source, base);
	wlr_output_layout_get_box(output_source->ref_output_layout, output_source->ref_output, extents);

	if (source->with_cursors) {
		wlr_output_layout_add(output_source->ref_output_layout,
			&output_source->base.output, extents->x, extents->y);
	} else {
		wlr_output_layout_remove(output_source->ref_output_layout, &output_source->base.output);
	}
}

static struct scene_source_interface scene_output_source_impl = {
	.update_extents = scene_output_source_update_extents,
};

static void output_source_destroy(struct scene_output_source *source) {
	wl_list_remove(&source->ref_output_destroy.link);
	wl_list_remove(&source->ref_output_layout_destroy.link);
	source_finish(&source->base);
	free(source);
}

static void output_source_handle_ref_output_destroy(struct wl_listener *listener, void *data) {
	struct scene_output_source *source = wl_container_of(listener, source, ref_output_destroy);
	output_source_destroy(source);
}

static void output_source_handle_ref_output_layout_destroy(struct wl_listener *listener, void *data) {
	struct scene_output_source *source = wl_container_of(listener, source, ref_output_layout_destroy);
	output_source_destroy(source);
}

struct wlr_ext_image_capture_source_v1 *wlr_ext_image_capture_source_v1_create_with_scene_output(
		struct wlr_scene *scene, struct wlr_output *reference_output,
		struct wlr_output_layout *layout) {
	struct scene_output_source *source = calloc(1, sizeof(*source));
	if (source == NULL) {
		return NULL;
	}

	source_init(&source->base, scene, reference_output->event_loop,
		reference_output->allocator, reference_output->renderer);
	source->base.impl = &scene_output_source_impl;

	source->ref_output = reference_output;
	source->ref_output_layout = layout;

	source->ref_output_destroy.notify = output_source_handle_ref_output_destroy;
	wl_signal_add(&reference_output->events.destroy, &source->ref_output_destroy);

	source->ref_output_layout_destroy.notify = output_source_handle_ref_output_layout_destroy;
	wl_signal_add(&layout->events.destroy, &source->ref_output_layout_destroy);

	return &source->base.base;
}