#include <assert.h>
#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>
#include "types/wlr_output.h"
#include "types/wlr_scene.h"
#include "util/array.h"
#include "util/time.h"

#define DMABUF_FEEDBACK_DEBOUNCE_FRAMES  30
#define HIGHLIGHT_DAMAGE_FADEOUT_TIME   250

struct highlight_region {
	pixman_region32_t region;
	struct timespec when;
	struct wl_list link;
};

struct render_data {
	enum wl_output_transform transform;
	float scale;
	struct wlr_box logical;
	int trans_width, trans_height;

	struct wlr_scene_output *output;

	struct wlr_render_pass *render_pass;
	pixman_region32_t damage;
};

static void logical_to_buffer_coords(pixman_region32_t *region, const struct render_data *data,
		bool round_up) {
	enum wl_output_transform transform = wlr_output_transform_invert(data->transform);
	scale_region(region, data->scale, round_up);
	wlr_region_transform(region, region, transform, data->trans_width, data->trans_height);
}

void output_to_buffer_coords(pixman_region32_t *damage, struct wlr_output *output) {
	int width, height;
	wlr_output_transformed_resolution(output, &width, &height);

	wlr_region_transform(damage, damage,
		wlr_output_transform_invert(output->transform), width, height);
}

static int scale_length(int length, int offset, float scale) {
	return round((offset + length) * scale) - round(offset * scale);
}

static void scale_box(struct wlr_box *box, float scale) {
	box->width = scale_length(box->width, box->x, scale);
	box->height = scale_length(box->height, box->y, scale);
	box->x = round(box->x * scale);
	box->y = round(box->y * scale);
}

static void transform_output_box(struct wlr_box *box, const struct render_data *data) {
	enum wl_output_transform transform = wlr_output_transform_invert(data->transform);
	scale_box(box, data->scale);
	wlr_box_transform(box, box, transform, data->trans_width, data->trans_height);
}

void scene_output_damage(struct wlr_scene_output *scene_output, const pixman_region32_t *damage) {
	struct wlr_output *output = scene_output->output;

	pixman_region32_t clipped;
	pixman_region32_init(&clipped);
	pixman_region32_intersect_rect(&clipped, damage, 0, 0, output->width, output->height);

	if (!pixman_region32_empty(&clipped)) {
		wlr_output_schedule_frame(scene_output->output);
		wlr_damage_ring_add(&scene_output->damage_ring, &clipped);

		pixman_region32_union(&scene_output->pending_commit_damage,
			&scene_output->pending_commit_damage, &clipped);
	}

	pixman_region32_fini(&clipped);
}

static void scene_output_damage_whole(struct wlr_scene_output *scene_output) {
	struct wlr_output *output = scene_output->output;

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, output->width, output->height);
	scene_output_damage(scene_output, &damage);
	pixman_region32_fini(&damage);
}

static struct wlr_texture *scene_buffer_get_texture(
		struct wlr_scene_buffer *scene_buffer, struct wlr_renderer *renderer) {
	if (scene_buffer->buffer == NULL || scene_buffer->texture != NULL) {
		return scene_buffer->texture;
	}

	struct wlr_client_buffer *client_buffer =
		wlr_client_buffer_get(scene_buffer->buffer);
	if (client_buffer != NULL) {
		return client_buffer->texture;
	}

	struct wlr_texture *texture =
		wlr_texture_from_buffer(renderer, scene_buffer->buffer);
	if (texture != NULL && scene_buffer->own_buffer) {
		scene_buffer->own_buffer = false;
		wlr_buffer_unlock(scene_buffer->buffer);
	}
	scene_buffer_set_texture(scene_buffer, texture);
	return texture;
}

struct render_list_entry {
	struct wlr_scene_node *node;
	bool highlight_transparent_region;
	int x, y;
};

static void scene_entry_render(struct render_list_entry *entry, const struct render_data *data) {
	struct wlr_scene_node *node = entry->node;

	pixman_region32_t render_region;
	pixman_region32_init(&render_region);
	pixman_region32_copy(&render_region, &node->visible);
	pixman_region32_translate(&render_region, -data->logical.x, -data->logical.y);
	logical_to_buffer_coords(&render_region, data, true);
	pixman_region32_intersect(&render_region, &render_region, &data->damage);
	if (pixman_region32_empty(&render_region)) {
		pixman_region32_fini(&render_region);
		return;
	}

	int x = entry->x - data->logical.x;
	int y = entry->y - data->logical.y;

	struct wlr_box dst_box = {
		.x = x,
		.y = y,
	};
	scene_node_get_size(node, &dst_box.width, &dst_box.height);
	transform_output_box(&dst_box, data);

	pixman_region32_t opaque;
	pixman_region32_init(&opaque);
	scene_node_opaque_region(node, x, y, &opaque);
	logical_to_buffer_coords(&opaque, data, false);
	pixman_region32_subtract(&opaque, &render_region, &opaque);

	switch (node->type) {
	case WLR_SCENE_NODE_TREE:
		assert(false);
		break;
	case WLR_SCENE_NODE_RECT:;
		struct wlr_scene_rect *scene_rect = wlr_scene_rect_from_node(node);

		wlr_render_pass_add_rect(data->render_pass, &(struct wlr_render_rect_options){
			.box = dst_box,
			.color = {
				.r = scene_rect->color[0],
				.g = scene_rect->color[1],
				.b = scene_rect->color[2],
				.a = scene_rect->color[3],
			},
			.clip = &render_region,
		});
		break;
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);

		if (scene_buffer->is_single_pixel_buffer) {
			// Render the buffer as a rect, this is likely to be more efficient
			wlr_render_pass_add_rect(data->render_pass, &(struct wlr_render_rect_options){
				.box = dst_box,
				.color = {
					.r = (float)scene_buffer->single_pixel_buffer_color[0] / (float)UINT32_MAX,
					.g = (float)scene_buffer->single_pixel_buffer_color[1] / (float)UINT32_MAX,
					.b = (float)scene_buffer->single_pixel_buffer_color[2] / (float)UINT32_MAX,
					.a = (float)scene_buffer->single_pixel_buffer_color[3] /
						(float)UINT32_MAX * scene_buffer->opacity,
				},
				.clip = &render_region,
			});
			break;
		}

		struct wlr_texture *texture = scene_buffer_get_texture(scene_buffer,
			data->output->output->renderer);
		if (texture == NULL) {
			scene_output_damage(data->output, &render_region);
			break;
		}

		enum wl_output_transform transform =
			wlr_output_transform_invert(scene_buffer->transform);
		transform = wlr_output_transform_compose(transform, data->transform);

		wlr_render_pass_add_texture(data->render_pass, &(struct wlr_render_texture_options) {
			.texture = texture,
			.src_box = scene_buffer->src_box,
			.dst_box = dst_box,
			.transform = transform,
			.clip = &render_region,
			.alpha = &scene_buffer->opacity,
			.filter_mode = scene_buffer->filter_mode,
			.blend_mode = !data->output->scene->calculate_visibility ||
					!pixman_region32_empty(&opaque) ?
				WLR_RENDER_BLEND_MODE_PREMULTIPLIED : WLR_RENDER_BLEND_MODE_NONE,
			.wait_timeline = scene_buffer->wait_timeline,
			.wait_point = scene_buffer->wait_point,
		});

		struct wlr_scene_output_sample_event sample_event = {
			.output = data->output,
			.direct_scanout = false,
		};
		wl_signal_emit_mutable(&scene_buffer->events.output_sample, &sample_event);

		if (entry->highlight_transparent_region) {
			wlr_render_pass_add_rect(data->render_pass, &(struct wlr_render_rect_options){
				.box = dst_box,
				.color = { .r = 0, .g = 0.3, .b = 0, .a = 0.3 },
				.clip = &opaque,
			});
		}

		break;
	}

	pixman_region32_fini(&opaque);
	pixman_region32_fini(&render_region);
}

static void scene_output_handle_destroy(struct wlr_addon *addon) {
	struct wlr_scene_output *scene_output =
		wl_container_of(addon, scene_output, addon);
	wlr_scene_output_destroy(scene_output);
}

static const struct wlr_addon_interface output_addon_impl = {
	.name = "wlr_scene_output",
	.destroy = scene_output_handle_destroy,
};

static void scene_node_output_update(struct wlr_scene_node *node,
		struct wl_list *outputs, struct wlr_scene_output *ignore,
		struct wlr_scene_output *force) {
	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_node_output_update(child, outputs, ignore, force);
		}
		return;
	}

	update_node_update_outputs(node, outputs, ignore, force);
}

static void scene_output_update_geometry(struct wlr_scene_output *scene_output,
		bool force_update) {
	scene_output_damage_whole(scene_output);

	scene_node_output_update(&scene_output->scene->tree.node,
			&scene_output->scene->outputs, NULL, force_update ? scene_output : NULL);
}

static void scene_output_handle_commit(struct wl_listener *listener, void *data) {
	struct wlr_scene_output *scene_output = wl_container_of(listener,
		scene_output, output_commit);
	struct wlr_output_event_commit *event = data;
	const struct wlr_output_state *state = event->state;

	// if the output has been committed with a certain damage, we know that region
	// will be acknowledged by the backend so we don't need to keep track of it
	// anymore
	if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
		if (state->committed & WLR_OUTPUT_STATE_DAMAGE) {
			pixman_region32_subtract(&scene_output->pending_commit_damage,
				&scene_output->pending_commit_damage, &state->damage);
		} else {
			pixman_region32_fini(&scene_output->pending_commit_damage);
			pixman_region32_init(&scene_output->pending_commit_damage);
		}
	}

	bool force_update = state->committed & (
		WLR_OUTPUT_STATE_TRANSFORM |
		WLR_OUTPUT_STATE_SCALE |
		WLR_OUTPUT_STATE_SUBPIXEL);

	if (force_update || state->committed & (WLR_OUTPUT_STATE_MODE |
			WLR_OUTPUT_STATE_ENABLED)) {
		scene_output_update_geometry(scene_output, force_update);
	}

	if (scene_output->scene->debug_damage_option == WLR_SCENE_DEBUG_DAMAGE_HIGHLIGHT &&
			!wl_list_empty(&scene_output->damage_highlight_regions)) {
		wlr_output_schedule_frame(scene_output->output);
	}

	// Next time the output is enabled, try to re-apply the gamma LUT
	if (scene_output->scene->gamma_control_manager_v1 &&
			(state->committed & WLR_OUTPUT_STATE_ENABLED) &&
			!scene_output->output->enabled) {
		scene_output->gamma_lut_changed = true;
	}
}

static void scene_output_handle_damage(struct wl_listener *listener, void *data) {
	struct wlr_scene_output *scene_output = wl_container_of(listener,
		scene_output, output_damage);
	struct wlr_output *output = scene_output->output;
	struct wlr_output_event_damage *event = data;

	int width, height;
	wlr_output_transformed_resolution(output, &width, &height);

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	pixman_region32_copy(&damage, event->damage);
	wlr_region_transform(&damage, &damage,
		wlr_output_transform_invert(output->transform), width, height);
	scene_output_damage(scene_output, &damage);
	pixman_region32_fini(&damage);
}

static void scene_output_handle_needs_frame(struct wl_listener *listener, void *data) {
	struct wlr_scene_output *scene_output = wl_container_of(listener,
		scene_output, output_needs_frame);
	wlr_output_schedule_frame(scene_output->output);
}

struct wlr_scene_output *wlr_scene_output_create(struct wlr_scene *scene,
		struct wlr_output *output) {
	struct wlr_scene_output *scene_output = calloc(1, sizeof(*scene_output));
	if (scene_output == NULL) {
		return NULL;
	}

	scene_output->output = output;
	scene_output->scene = scene;
	wlr_addon_init(&scene_output->addon, &output->addons, scene, &output_addon_impl);

	wlr_damage_ring_init(&scene_output->damage_ring);
	pixman_region32_init(&scene_output->pending_commit_damage);
	wl_list_init(&scene_output->damage_highlight_regions);

	int prev_output_index = -1;
	struct wl_list *prev_output_link = &scene->outputs;

	struct wlr_scene_output *current_output;
	wl_list_for_each(current_output, &scene->outputs, link) {
		if (prev_output_index + 1 != current_output->index) {
			break;
		}

		prev_output_index = current_output->index;
		prev_output_link = &current_output->link;
	}

	int drm_fd = wlr_backend_get_drm_fd(output->backend);
	if (drm_fd >= 0 && output->backend->features.timeline &&
			output->renderer != NULL && output->renderer->features.timeline) {
		scene_output->in_timeline = wlr_drm_syncobj_timeline_create(drm_fd);
		if (scene_output->in_timeline == NULL) {
			return NULL;
		}
	}

	scene_output->index = prev_output_index + 1;
	assert(scene_output->index < 64);
	wl_list_insert(prev_output_link, &scene_output->link);

	wl_signal_init(&scene_output->events.destroy);

	scene_output->output_commit.notify = scene_output_handle_commit;
	wl_signal_add(&output->events.commit, &scene_output->output_commit);

	scene_output->output_damage.notify = scene_output_handle_damage;
	wl_signal_add(&output->events.damage, &scene_output->output_damage);

	scene_output->output_needs_frame.notify = scene_output_handle_needs_frame;
	wl_signal_add(&output->events.needs_frame, &scene_output->output_needs_frame);

	scene_output_update_geometry(scene_output, false);

	return scene_output;
}

static void highlight_region_destroy(struct highlight_region *damage) {
	wl_list_remove(&damage->link);
	pixman_region32_fini(&damage->region);
	free(damage);
}

void wlr_scene_output_destroy(struct wlr_scene_output *scene_output) {
	if (scene_output == NULL) {
		return;
	}

	wl_signal_emit_mutable(&scene_output->events.destroy, NULL);

	scene_node_output_update(&scene_output->scene->tree.node,
		&scene_output->scene->outputs, scene_output, NULL);

	assert(wl_list_empty(&scene_output->events.destroy.listener_list));

	struct highlight_region *damage, *tmp_damage;
	wl_list_for_each_safe(damage, tmp_damage, &scene_output->damage_highlight_regions, link) {
		highlight_region_destroy(damage);
	}

	wlr_addon_finish(&scene_output->addon);
	wlr_damage_ring_finish(&scene_output->damage_ring);
	pixman_region32_fini(&scene_output->pending_commit_damage);
	wl_list_remove(&scene_output->link);
	wl_list_remove(&scene_output->output_commit.link);
	wl_list_remove(&scene_output->output_damage.link);
	wl_list_remove(&scene_output->output_needs_frame.link);
	wlr_drm_syncobj_timeline_unref(scene_output->in_timeline);
	wl_array_release(&scene_output->render_list);
	free(scene_output);
}

struct wlr_scene_output *wlr_scene_get_scene_output(struct wlr_scene *scene,
		struct wlr_output *output) {
	struct wlr_addon *addon =
		wlr_addon_find(&output->addons, scene, &output_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct wlr_scene_output *scene_output =
		wl_container_of(addon, scene_output, addon);
	return scene_output;
}

void wlr_scene_output_set_position(struct wlr_scene_output *scene_output,
		int lx, int ly) {
	if (scene_output->x == lx && scene_output->y == ly) {
		return;
	}

	scene_output->x = lx;
	scene_output->y = ly;

	scene_output_update_geometry(scene_output, false);
}

static bool scene_node_invisible(struct wlr_scene_node *node) {
	if (node->type == WLR_SCENE_NODE_TREE) {
		return true;
	} else if (node->type == WLR_SCENE_NODE_RECT) {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);

		return rect->color[3] == 0.f;
	} else if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);

		return buffer->buffer == NULL && buffer->texture == NULL;
	}

	return false;
}

struct render_list_constructor_data {
	struct wlr_box box;
	struct wl_array *render_list;
	bool calculate_visibility;
	bool highlight_transparent_region;
	bool fractional_scale;
};

static bool scene_buffer_is_black_opaque(struct wlr_scene_buffer *scene_buffer) {
	return scene_buffer->is_single_pixel_buffer &&
		scene_buffer->single_pixel_buffer_color[0] == 0 &&
		scene_buffer->single_pixel_buffer_color[1] == 0 &&
		scene_buffer->single_pixel_buffer_color[2] == 0 &&
		scene_buffer->single_pixel_buffer_color[3] == UINT32_MAX &&
		scene_buffer->opacity == 1.0;
}

static bool construct_render_list_iterator(struct wlr_scene_node *node,
		int lx, int ly, void *_data) {
	struct render_list_constructor_data *data = _data;

	if (scene_node_invisible(node)) {
		return false;
	}

	// While rendering, the background should always be black. If we see a
	// black rect, we can ignore rendering everything under the rect, and
	// unless fractional scale is used even the rect itself (to avoid running
	// into issues regarding damage region expansion).
	if (node->type == WLR_SCENE_NODE_RECT && data->calculate_visibility &&
			(!data->fractional_scale || data->render_list->size == 0)) {
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		float *black = (float[4]){ 0.f, 0.f, 0.f, 1.f };

		if (memcmp(rect->color, black, sizeof(float) * 4) == 0) {
			return false;
		}
	}

	// Apply the same special-case to black opaque single-pixel buffers
	if (node->type == WLR_SCENE_NODE_BUFFER && data->calculate_visibility &&
			(!data->fractional_scale || data->render_list->size == 0)) {
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);

		if (scene_buffer_is_black_opaque(scene_buffer)) {
			return false;
		}
	}

	pixman_region32_t intersection;
	pixman_region32_init(&intersection);
	pixman_region32_intersect_rect(&intersection, &node->visible,
			data->box.x, data->box.y,
			data->box.width, data->box.height);
	if (pixman_region32_empty(&intersection)) {
		pixman_region32_fini(&intersection);
		return false;
	}

	pixman_region32_fini(&intersection);

	struct render_list_entry *entry = wl_array_add(data->render_list, sizeof(*entry));
	if (!entry) {
		return false;
	}

	*entry = (struct render_list_entry){
		.node = node,
		.x = lx,
		.y = ly,
		.highlight_transparent_region = data->highlight_transparent_region,
	};

	return false;
}

static void scene_buffer_send_dmabuf_feedback(const struct wlr_scene *scene,
		struct wlr_scene_buffer *scene_buffer,
		const struct wlr_linux_dmabuf_feedback_v1_init_options *options) {
	if (!scene->linux_dmabuf_v1) {
		return;
	}

	struct wlr_scene_surface *surface = wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!surface) {
		return;
	}

	// compare to the previous options so that we don't send
	// duplicate feedback events.
	if (memcmp(options, &scene_buffer->prev_feedback_options, sizeof(*options)) == 0) {
		return;
	}

	scene_buffer->prev_feedback_options = *options;

	struct wlr_linux_dmabuf_feedback_v1 feedback = {0};
	if (!wlr_linux_dmabuf_feedback_v1_init_with_options(&feedback, options)) {
		return;
	}

	wlr_linux_dmabuf_v1_set_surface_feedback(scene->linux_dmabuf_v1,
		surface->surface, &feedback);

	wlr_linux_dmabuf_feedback_v1_finish(&feedback);
}

enum scene_direct_scanout_result {
	// This scene node is not a candidate for scanout
	SCANOUT_INELIGIBLE,

	// This scene node is a candidate for scanout, but is currently
	// incompatible due to e.g. buffer mismatch, and if possible we'd like to
	// resolve this incompatibility.
	SCANOUT_CANDIDATE,

	// Scanout is successful.
	SCANOUT_SUCCESS,
};

static enum scene_direct_scanout_result scene_entry_try_direct_scanout(
		struct render_list_entry *entry, struct wlr_output_state *state,
		const struct render_data *data) {
	struct wlr_scene_output *scene_output = data->output;
	struct wlr_scene_node *node = entry->node;

	if (!scene_output->scene->direct_scanout) {
		return SCANOUT_INELIGIBLE;
	}

	if (node->type != WLR_SCENE_NODE_BUFFER) {
		return SCANOUT_INELIGIBLE;
	}

	if (state->committed & (WLR_OUTPUT_STATE_MODE |
			WLR_OUTPUT_STATE_ENABLED |
			WLR_OUTPUT_STATE_RENDER_FORMAT)) {
		// Legacy DRM will explode if we try to modeset with a direct scanout buffer
		return SCANOUT_INELIGIBLE;
	}

	if (!wlr_output_is_direct_scanout_allowed(scene_output->output)) {
		return SCANOUT_INELIGIBLE;
	}

	struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
	if (buffer->buffer == NULL) {
		return SCANOUT_INELIGIBLE;
	}

	// The native size of the buffer after any transform is applied
	int default_width = buffer->buffer->width;
	int default_height = buffer->buffer->height;
	wlr_output_transform_coords(buffer->transform, &default_width, &default_height);
	struct wlr_fbox default_box = {
		.width = default_width,
		.height = default_height,
	};

	if (buffer->transform != data->transform) {
		return SCANOUT_INELIGIBLE;
	}

	// We want to ensure optimal buffer selection, but as direct-scanout can be enabled and disabled
	// on a frame-by-frame basis, we wait for a few frames to send the new format recommendations.
	// Maybe we should only send feedback in this case if tests fail.
	if (scene_output->dmabuf_feedback_debounce >= DMABUF_FEEDBACK_DEBOUNCE_FRAMES
			&& buffer->primary_output == scene_output) {
		struct wlr_linux_dmabuf_feedback_v1_init_options options = {
			.main_renderer = scene_output->output->renderer,
			.scanout_primary_output = scene_output->output,
		};

		scene_buffer_send_dmabuf_feedback(scene_output->scene, buffer, &options);
	}

	struct wlr_output_state pending;
	wlr_output_state_init(&pending);
	if (!wlr_output_state_copy(&pending, state)) {
		return SCANOUT_CANDIDATE;
	}

	if (!wlr_fbox_empty(&buffer->src_box) &&
			!wlr_fbox_equal(&buffer->src_box, &default_box)) {
		pending.buffer_src_box = buffer->src_box;
	}

	// Translate the position from scene coordinates to output coordinates
	pending.buffer_dst_box.x = entry->x - scene_output->x;
	pending.buffer_dst_box.y = entry->y - scene_output->y;

	scene_node_get_size(node, &pending.buffer_dst_box.width, &pending.buffer_dst_box.height);
	transform_output_box(&pending.buffer_dst_box, data);

	struct wlr_buffer *wlr_buffer = buffer->buffer;
	struct wlr_client_buffer *client_buffer = wlr_client_buffer_get(wlr_buffer);
	if (client_buffer != NULL && client_buffer->source != NULL && client_buffer->source->n_locks > 0) {
		wlr_buffer = client_buffer->source;
	}

	wlr_output_state_set_buffer(&pending, wlr_buffer);
	if (buffer->wait_timeline != NULL) {
		wlr_output_state_set_wait_timeline(&pending, buffer->wait_timeline, buffer->wait_point);
	}
	if (!wlr_output_test_state(scene_output->output, &pending)) {
		wlr_output_state_finish(&pending);
		return SCANOUT_CANDIDATE;
	}

	wlr_output_state_copy(state, &pending);
	wlr_output_state_finish(&pending);

	struct wlr_scene_output_sample_event sample_event = {
		.output = scene_output,
		.direct_scanout = true,
	};
	wl_signal_emit_mutable(&buffer->events.output_sample, &sample_event);
	return SCANOUT_SUCCESS;
}

bool wlr_scene_output_needs_frame(struct wlr_scene_output *scene_output) {
	return scene_output->output->needs_frame ||
		!pixman_region32_empty(&scene_output->pending_commit_damage) ||
		scene_output->gamma_lut_changed;
}

bool wlr_scene_output_commit(struct wlr_scene_output *scene_output,
		const struct wlr_scene_output_state_options *options) {
	if (!wlr_scene_output_needs_frame(scene_output)) {
		return true;
	}

	bool ok = false;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (!wlr_scene_output_build_state(scene_output, &state, options)) {
		goto out;
	}

	ok = wlr_output_commit_state(scene_output->output, &state);
	if (!ok) {
		goto out;
	}

out:
	wlr_output_state_finish(&state);
	return ok;
}

static void scene_output_state_attempt_gamma(struct wlr_scene_output *scene_output,
		struct wlr_output_state *state) {
	if (!scene_output->gamma_lut_changed) {
		return;
	}

	struct wlr_output_state gamma_pending = {0};
	if (!wlr_output_state_copy(&gamma_pending, state)) {
		return;
	}

	if (!wlr_gamma_control_v1_apply(scene_output->gamma_lut, &gamma_pending)) {
		wlr_output_state_finish(&gamma_pending);
		return;
	}

	scene_output->gamma_lut_changed = false;
	if (!wlr_output_test_state(scene_output->output, &gamma_pending)) {
		wlr_gamma_control_v1_send_failed_and_destroy(scene_output->gamma_lut);

		scene_output->gamma_lut = NULL;
		wlr_output_state_finish(&gamma_pending);
		return;
	}

	wlr_output_state_copy(state, &gamma_pending);
	wlr_output_state_finish(&gamma_pending);
}

bool wlr_scene_output_build_state(struct wlr_scene_output *scene_output,
		struct wlr_output_state *state, const struct wlr_scene_output_state_options *options) {
	struct wlr_scene_output_state_options default_options = {0};
	if (!options) {
		options = &default_options;
	}
	struct wlr_scene_timer *timer = options->timer;
	struct timespec start_time;
	if (timer) {
		clock_gettime(CLOCK_MONOTONIC, &start_time);
		wlr_scene_timer_finish(timer);
		*timer = (struct wlr_scene_timer){0};
	}

	if ((state->committed & WLR_OUTPUT_STATE_ENABLED) && !state->enabled) {
		// if the state is being disabled, do nothing.
		return true;
	}

	struct wlr_output *output = scene_output->output;
	enum wlr_scene_debug_damage_option debug_damage =
		scene_output->scene->debug_damage_option;

	struct render_data render_data = {
		.transform = output->transform,
		.scale = output->scale,
		.logical = { .x = scene_output->x, .y = scene_output->y },
		.output = scene_output,
	};

	int resolution_width, resolution_height;
	output_pending_resolution(output, state,
		&resolution_width, &resolution_height);

	if (state->committed & WLR_OUTPUT_STATE_TRANSFORM) {
		if (render_data.transform != state->transform) {
			scene_output_damage_whole(scene_output);
		}

		render_data.transform = state->transform;
	}

	if (state->committed & WLR_OUTPUT_STATE_SCALE) {
		if (render_data.scale != state->scale) {
			scene_output_damage_whole(scene_output);
		}

		render_data.scale = state->scale;
	}

	render_data.trans_width = resolution_width;
	render_data.trans_height = resolution_height;
	wlr_output_transform_coords(render_data.transform,
		&render_data.trans_width, &render_data.trans_height);

	render_data.logical.width = render_data.trans_width / render_data.scale;
	render_data.logical.height = render_data.trans_height / render_data.scale;

	struct render_list_constructor_data list_con = {
		.box = render_data.logical,
		.render_list = &scene_output->render_list,
		.calculate_visibility = scene_output->scene->calculate_visibility,
		.highlight_transparent_region = scene_output->scene->highlight_transparent_region,
		.fractional_scale = floor(render_data.scale) != render_data.scale,
	};

	list_con.render_list->size = 0;
	scene_nodes_in_box(&scene_output->scene->tree.node, &list_con.box,
		construct_render_list_iterator, &list_con);
	array_realloc(list_con.render_list, list_con.render_list->size);

	struct render_list_entry *list_data = list_con.render_list->data;
	int list_len = list_con.render_list->size / sizeof(*list_data);

	if (debug_damage == WLR_SCENE_DEBUG_DAMAGE_RERENDER) {
		scene_output_damage_whole(scene_output);
	}

	struct timespec now;
	if (debug_damage == WLR_SCENE_DEBUG_DAMAGE_HIGHLIGHT) {
		struct wl_list *regions = &scene_output->damage_highlight_regions;
		clock_gettime(CLOCK_MONOTONIC, &now);

		// add the current frame's damage if there is damage
		if (!pixman_region32_empty(&scene_output->damage_ring.current)) {
			struct highlight_region *current_damage = calloc(1, sizeof(*current_damage));
			if (current_damage) {
				pixman_region32_init(&current_damage->region);
				pixman_region32_copy(&current_damage->region,
					&scene_output->damage_ring.current);
				current_damage->when = now;
				wl_list_insert(regions, &current_damage->link);
			}
		}

		pixman_region32_t acc_damage;
		pixman_region32_init(&acc_damage);
		struct highlight_region *damage, *tmp_damage;
		wl_list_for_each_safe(damage, tmp_damage, regions, link) {
			// remove overlaping damage regions
			pixman_region32_subtract(&damage->region, &damage->region, &acc_damage);
			pixman_region32_union(&acc_damage, &acc_damage, &damage->region);

			// if this damage is too old or has nothing in it, get rid of it
			struct timespec time_diff;
			timespec_sub(&time_diff, &now, &damage->when);
			if (timespec_to_msec(&time_diff) >= HIGHLIGHT_DAMAGE_FADEOUT_TIME ||
					pixman_region32_empty(&damage->region)) {
				highlight_region_destroy(damage);
			}
		}

		scene_output_damage(scene_output, &acc_damage);
		pixman_region32_fini(&acc_damage);
	}

	wlr_output_state_set_damage(state, &scene_output->pending_commit_damage);

	// We only want to try direct scanout if:
	// - There is only one entry in the render list
	// - There are no color transforms that need to be applied
	// - Damage highlight debugging is not enabled
	enum scene_direct_scanout_result scanout_result = SCANOUT_INELIGIBLE;
	if (options->color_transform == NULL && list_len == 1
			&& debug_damage != WLR_SCENE_DEBUG_DAMAGE_HIGHLIGHT) {
		scanout_result = scene_entry_try_direct_scanout(&list_data[0], state, &render_data);
	}

	if (scanout_result == SCANOUT_INELIGIBLE) {
		if (scene_output->dmabuf_feedback_debounce > 0) {
			// We cannot scan out, so count down towards sending composition dmabuf feedback
			scene_output->dmabuf_feedback_debounce--;
		}
	} else if (scene_output->dmabuf_feedback_debounce < DMABUF_FEEDBACK_DEBOUNCE_FRAMES) {
		// We either want to scan out or successfully scanned out, so count up towards sending
		// scanout dmabuf feedback
		scene_output->dmabuf_feedback_debounce++;
	}

	bool scanout = scanout_result == SCANOUT_SUCCESS;
	if (scene_output->prev_scanout != scanout) {
		scene_output->prev_scanout = scanout;
		wlr_log(WLR_DEBUG, "Direct scan-out %s",
			scanout ? "enabled" : "disabled");
	}

	if (scanout) {
		scene_output_state_attempt_gamma(scene_output, state);

		if (timer) {
			struct timespec end_time, duration;
			clock_gettime(CLOCK_MONOTONIC, &end_time);
			timespec_sub(&duration, &end_time, &start_time);
			timer->pre_render_duration = timespec_to_nsec(&duration);
		}
		return true;
	}

	struct wlr_swapchain *swapchain = options->swapchain;
	if (!swapchain) {
		if (!wlr_output_configure_primary_swapchain(output, state, &output->swapchain)) {
			return false;
		}

		swapchain = output->swapchain;
	}

	struct wlr_buffer *buffer = wlr_swapchain_acquire(swapchain);
	if (buffer == NULL) {
		return false;
	}

	assert(buffer->width == resolution_width && buffer->height == resolution_height);

	if (timer) {
		timer->render_timer = wlr_render_timer_create(output->renderer);

		struct timespec end_time, duration;
		clock_gettime(CLOCK_MONOTONIC, &end_time);
		timespec_sub(&duration, &end_time, &start_time);
		timer->pre_render_duration = timespec_to_nsec(&duration);
	}

	scene_output->in_point++;
	struct wlr_render_pass *render_pass = wlr_renderer_begin_buffer_pass(output->renderer, buffer,
			&(struct wlr_buffer_pass_options){
		.timer = timer ? timer->render_timer : NULL,
		.color_transform = options->color_transform,
		.signal_timeline = scene_output->in_timeline,
		.signal_point = scene_output->in_point,
	});
	if (render_pass == NULL) {
		wlr_buffer_unlock(buffer);
		return false;
	}

	render_data.render_pass = render_pass;

	pixman_region32_init(&render_data.damage);
	wlr_damage_ring_rotate_buffer(&scene_output->damage_ring, buffer,
		&render_data.damage);

	pixman_region32_t background;
	pixman_region32_init(&background);
	pixman_region32_copy(&background, &render_data.damage);

	// Cull areas of the background that are occluded by opaque regions of
	// scene nodes above. Those scene nodes will just render atop having us
	// never see the background.
	if (scene_output->scene->calculate_visibility) {
		for (int i = list_len - 1; i >= 0; i--) {
			struct render_list_entry *entry = &list_data[i];

			// We must only cull opaque regions that are visible by the node.
			// The node's visibility will have the knowledge of a black rect
			// that may have been omitted from the render list via the black
			// rect optimization. In order to ensure we don't cull background
			// rendering in that black rect region, consider the node's visibility.
			pixman_region32_t opaque;
			pixman_region32_init(&opaque);
			scene_node_opaque_region(entry->node, entry->x, entry->y, &opaque);
			pixman_region32_intersect(&opaque, &opaque, &entry->node->visible);

			pixman_region32_translate(&opaque, -scene_output->x, -scene_output->y);
			logical_to_buffer_coords(&opaque, &render_data, false);
			pixman_region32_subtract(&background, &background, &opaque);
			pixman_region32_fini(&opaque);
		}

		if (floor(render_data.scale) != render_data.scale) {
			wlr_region_expand(&background, &background, 1);

			// reintersect with the damage because we never want to render
			// outside of the damage region
			pixman_region32_intersect(&background, &background, &render_data.damage);
		}
	}

	wlr_render_pass_add_rect(render_pass, &(struct wlr_render_rect_options){
		.box = { .width = buffer->width, .height = buffer->height },
		.color = { .r = 0, .g = 0, .b = 0, .a = 1 },
		.clip = &background,
	});
	pixman_region32_fini(&background);

	for (int i = list_len - 1; i >= 0; i--) {
		struct render_list_entry *entry = &list_data[i];
		scene_entry_render(entry, &render_data);

		if (entry->node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(entry->node);

			// Direct scanout counts up to DMABUF_FEEDBACK_DEBOUNCE_FRAMES before sending new dmabuf
			// feedback, and on composition we wait until it hits zero again. If we knew that an
			// entry could never be a scanout candidate, we could send feedback to it
			// unconditionally without debounce, but for now it is all or nothing
			if (scene_output->dmabuf_feedback_debounce == 0 && buffer->primary_output == scene_output) {
				struct wlr_linux_dmabuf_feedback_v1_init_options options = {
					.main_renderer = output->renderer,
					.scanout_primary_output = NULL,
				};

				scene_buffer_send_dmabuf_feedback(scene_output->scene, buffer, &options);
			}
		}
	}

	if (debug_damage == WLR_SCENE_DEBUG_DAMAGE_HIGHLIGHT) {
		struct highlight_region *damage;
		wl_list_for_each(damage, &scene_output->damage_highlight_regions, link) {
			struct timespec time_diff;
			timespec_sub(&time_diff, &now, &damage->when);
			int64_t time_diff_ms = timespec_to_msec(&time_diff);
			float alpha = 1.0 - (double)time_diff_ms / HIGHLIGHT_DAMAGE_FADEOUT_TIME;

			wlr_render_pass_add_rect(render_pass, &(struct wlr_render_rect_options){
				.box = { .width = buffer->width, .height = buffer->height },
				.color = { .r = alpha * 0.5, .g = 0, .b = 0, .a = alpha * 0.5 },
				.clip = &damage->region,
			});
		}
	}

	wlr_output_add_software_cursors_to_render_pass(output, render_pass, &render_data.damage);
	pixman_region32_fini(&render_data.damage);

	if (!wlr_render_pass_submit(render_pass)) {
		wlr_buffer_unlock(buffer);

		// if we failed to render the buffer, it will have undefined contents
		// Trash the damage ring
		wlr_damage_ring_add_whole(&scene_output->damage_ring);
		return false;
	}

	wlr_output_state_set_buffer(state, buffer);
	wlr_buffer_unlock(buffer);

	if (scene_output->in_timeline != NULL) {
		wlr_output_state_set_wait_timeline(state, scene_output->in_timeline,
			scene_output->in_point);
	}

	scene_output_state_attempt_gamma(scene_output, state);

	return true;
}

static void scene_node_send_frame_done(struct wlr_scene_node *node,
		struct wlr_scene_output *scene_output, struct timespec *now) {
	if (!node->enabled) {
		return;
	}

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer =
			wlr_scene_buffer_from_node(node);

		if (scene_buffer->primary_output == scene_output) {
			wlr_scene_buffer_send_frame_done(scene_buffer, now);
		}
	} else if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_node_send_frame_done(child, scene_output, now);
		}
	}
}

void wlr_scene_output_send_frame_done(struct wlr_scene_output *scene_output,
		struct timespec *now) {
	scene_node_send_frame_done(&scene_output->scene->tree.node,
		scene_output, now);
}

static void scene_output_for_each_scene_buffer(const struct wlr_box *output_box,
		struct wlr_scene_node *node, int lx, int ly,
		wlr_scene_buffer_iterator_func_t user_iterator, void *user_data) {
	if (!node->enabled) {
		return;
	}

	lx += node->x;
	ly += node->y;

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_box node_box = { .x = lx, .y = ly };
		scene_node_get_size(node, &node_box.width, &node_box.height);

		struct wlr_box intersection;
		if (wlr_box_intersection(&intersection, output_box, &node_box)) {
			struct wlr_scene_buffer *scene_buffer =
				wlr_scene_buffer_from_node(node);
			user_iterator(scene_buffer, lx, ly, user_data);
		}
	} else if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_output_for_each_scene_buffer(output_box, child, lx, ly,
				user_iterator, user_data);
		}
	}
}

void wlr_scene_output_for_each_buffer(struct wlr_scene_output *scene_output,
		wlr_scene_buffer_iterator_func_t iterator, void *user_data) {
	struct wlr_box box = { .x = scene_output->x, .y = scene_output->y };
	wlr_output_effective_resolution(scene_output->output,
		&box.width, &box.height);
	scene_output_for_each_scene_buffer(&box, &scene_output->scene->tree.node, 0, 0,
		iterator, user_data);
}
