#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>
#include "render/color.h"
#include "types/wlr_output.h"
#include "types/wlr_scene.h"
#include "util/array.h"
#include "util/env.h"
#include "util/time.h"

#include <wlr/config.h>

#if WLR_HAS_XWAYLAND
#include <wlr/xwayland/xwayland.h>
#endif

#define DMABUF_FEEDBACK_DEBOUNCE_FRAMES  30
#define HIGHLIGHT_DAMAGE_FADEOUT_TIME   250

struct highlight_region {
	pixman_region32_t region;
	struct timespec when;
	struct wl_list link;
};

struct wlr_scene *wlr_scene_create(void) {
	struct wlr_scene *scene = calloc(1, sizeof(*scene));
	if (scene == NULL) {
		return NULL;
	}

	scene->tree = wlr_root_scene_tree_create(scene);
	if (scene->tree == NULL) {
		free(scene);
		return NULL;
	}
	scene->tree->node.data = scene;

	wl_list_init(&scene->outputs);
	wl_list_init(&scene->linux_dmabuf_v1_destroy.link);
	wl_list_init(&scene->gamma_control_manager_v1_destroy.link);
	wl_list_init(&scene->gamma_control_manager_v1_set_gamma.link);

	const char *debug_damage_options[] = {
		"none",
		"rerender",
		"highlight",
		NULL
	};

	scene->debug_damage_option = env_parse_switch("WLR_SCENE_DEBUG_DAMAGE", debug_damage_options);
	scene->direct_scanout = !env_parse_bool("WLR_SCENE_DISABLE_DIRECT_SCANOUT");
	scene->calculate_visibility = !env_parse_bool("WLR_SCENE_DISABLE_VISIBILITY");
	scene->highlight_transparent_region = env_parse_bool("WLR_SCENE_HIGHLIGHT_TRANSPARENT_REGION");

	return scene;
}

void wlr_scene_destroy(struct wlr_scene *scene) {
	if (scene == NULL) {
		return;
	}

	wlr_scene_node_destroy(&scene->tree->node);
	free(scene);
}

static void scale_region(pixman_region32_t *region, float scale, bool round_up) {
	wlr_region_scale(region, region, scale);

	if (round_up && floor(scale) != scale) {
		wlr_region_expand(region, region, 1);
	}
}

static void logical_to_buffer_coords(pixman_region32_t *region, const struct wlr_render_data *data,
		bool round_up) {
	enum wl_output_transform transform = wlr_output_transform_invert(data->transform);
	scale_region(region, data->scale, round_up);
	wlr_region_transform(region, region, transform, data->trans_width, data->trans_height);
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

static void transform_output_box(struct wlr_box *box, const struct wlr_render_data *data) {
	enum wl_output_transform transform = wlr_output_transform_invert(data->transform);
	scale_box(box, data->scale);
	wlr_box_transform(box, box, transform, data->trans_width, data->trans_height);
}

static void scene_output_damage(struct wlr_scene_output *scene_output,
		const pixman_region32_t *damage) {
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

static float get_luminance_multiplier(const struct wlr_color_luminances *src_lum,
		const struct wlr_color_luminances *dst_lum) {
	return (dst_lum->reference / src_lum->reference) * (src_lum->max / dst_lum->max);
}

static void scene_handle_linux_dmabuf_v1_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_scene *scene =
		wl_container_of(listener, scene, linux_dmabuf_v1_destroy);
	wl_list_remove(&scene->linux_dmabuf_v1_destroy.link);
	wl_list_init(&scene->linux_dmabuf_v1_destroy.link);
	scene->linux_dmabuf_v1 = NULL;
}

void wlr_scene_set_linux_dmabuf_v1(struct wlr_scene *scene,
		struct wlr_linux_dmabuf_v1 *linux_dmabuf_v1) {
	assert(scene->linux_dmabuf_v1 == NULL);
	scene->linux_dmabuf_v1 = linux_dmabuf_v1;
	scene->linux_dmabuf_v1_destroy.notify = scene_handle_linux_dmabuf_v1_destroy;
	wl_signal_add(&linux_dmabuf_v1->events.destroy, &scene->linux_dmabuf_v1_destroy);
}

static void scene_handle_gamma_control_manager_v1_set_gamma(struct wl_listener *listener,
		void *data) {
	const struct wlr_gamma_control_manager_v1_set_gamma_event *event = data;
	struct wlr_scene *scene =
		wl_container_of(listener, scene, gamma_control_manager_v1_set_gamma);
	struct wlr_scene_output *output = wlr_scene_get_scene_output(scene, event->output);
	if (!output) {
		// this scene might not own this output.
		return;
	}

	output->gamma_lut_changed = true;
	output->gamma_lut = event->control;
	wlr_color_transform_unref(output->gamma_lut_color_transform);
	output->gamma_lut_color_transform = wlr_gamma_control_v1_get_color_transform(event->control);
	wlr_output_schedule_frame(output->output);
}

static void scene_handle_gamma_control_manager_v1_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_scene *scene =
		wl_container_of(listener, scene, gamma_control_manager_v1_destroy);
	wl_list_remove(&scene->gamma_control_manager_v1_destroy.link);
	wl_list_init(&scene->gamma_control_manager_v1_destroy.link);
	wl_list_remove(&scene->gamma_control_manager_v1_set_gamma.link);
	wl_list_init(&scene->gamma_control_manager_v1_set_gamma.link);
	scene->gamma_control_manager_v1 = NULL;

	struct wlr_scene_output *output;
	wl_list_for_each(output, &scene->outputs, link) {
		output->gamma_lut_changed = false;
		output->gamma_lut = NULL;
		wlr_color_transform_unref(output->gamma_lut_color_transform);
		output->gamma_lut_color_transform = NULL;
	}
}

void wlr_scene_set_gamma_control_manager_v1(struct wlr_scene *scene,
	    struct wlr_gamma_control_manager_v1 *gamma_control) {
	assert(scene->gamma_control_manager_v1 == NULL);
	scene->gamma_control_manager_v1 = gamma_control;

	scene->gamma_control_manager_v1_destroy.notify =
		scene_handle_gamma_control_manager_v1_destroy;
	wl_signal_add(&gamma_control->events.destroy, &scene->gamma_control_manager_v1_destroy);
	scene->gamma_control_manager_v1_set_gamma.notify =
		scene_handle_gamma_control_manager_v1_set_gamma;
	wl_signal_add(&gamma_control->events.set_gamma, &scene->gamma_control_manager_v1_set_gamma);
}

static void scene_handle_color_manager_v1_destroy(struct wl_listener *listener, void *data) {
	struct wlr_scene *scene = wl_container_of(listener, scene, color_manager_v1_destroy);
	wl_list_remove(&scene->color_manager_v1_destroy.link);
	wl_list_init(&scene->color_manager_v1_destroy.link);
	scene->color_manager_v1 = NULL;
}

void wlr_scene_set_color_manager_v1(struct wlr_scene *scene, struct wlr_color_manager_v1 *manager) {
	assert(scene->color_manager_v1 == NULL);
	scene->color_manager_v1 = manager;

	scene->color_manager_v1_destroy.notify = scene_handle_color_manager_v1_destroy;
	wl_signal_add(&manager->events.destroy, &scene->color_manager_v1_destroy);
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

static void scene_output_update_geometry(struct wlr_scene_output *scene_output,
		bool force_update) {
	scene_output_damage_whole(scene_output);

	wlr_scene_node_update_outputs(&scene_output->scene->tree->node,
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
		scene_output->out_timeline = wlr_drm_syncobj_timeline_create(drm_fd);
		if (scene_output->in_timeline == NULL || scene_output->out_timeline == NULL) {
			wlr_drm_syncobj_timeline_unref(scene_output->in_timeline);
			wlr_drm_syncobj_timeline_unref(scene_output->out_timeline);
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

	wlr_scene_node_update_outputs(&scene_output->scene->tree->node,
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
	if (scene_output->in_timeline != NULL) {
		wlr_drm_syncobj_timeline_signal(scene_output->in_timeline, UINT64_MAX);
		wlr_drm_syncobj_timeline_unref(scene_output->in_timeline);
	}
	if (scene_output->out_timeline != NULL) {
		wlr_drm_syncobj_timeline_signal(scene_output->out_timeline, UINT64_MAX);
		wlr_drm_syncobj_timeline_unref(scene_output->out_timeline);
	}
	wlr_color_transform_unref(scene_output->gamma_lut_color_transform);
	wlr_color_transform_unref(scene_output->prev_gamma_lut_color_transform);
	wlr_color_transform_unref(scene_output->prev_supplied_color_transform);
	wlr_color_transform_unref(scene_output->combined_color_transform);
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

	enum wl_output_transform preferred_buffer_transform = WL_OUTPUT_TRANSFORM_NORMAL;
	if (options->scanout_primary_output != NULL) {
		preferred_buffer_transform = options->scanout_primary_output->transform;
	}

	// TODO: also send wl_surface.preferred_buffer_transform when running with
	// pure software rendering
	wlr_surface_set_preferred_buffer_transform(surface->surface, preferred_buffer_transform);
	wlr_linux_dmabuf_v1_set_surface_feedback(scene->linux_dmabuf_v1,
		surface->surface, &feedback);

	wlr_linux_dmabuf_feedback_v1_finish(&feedback);
}

static bool color_management_is_scanout_allowed(const struct wlr_output_image_description *img_desc,
		const struct wlr_scene_buffer *buffer) {
	// Disallow scanout if the output has colorimetry information but buffer
	// doesn't; allow it only if the output also lacks it.
	if (buffer->transfer_function == 0 && buffer->primaries == 0) {
		return img_desc == NULL;
	}

	// If the output has colorimetry information, the buffer must match it for
	// direct scanout to be allowed.
	if (img_desc != NULL) {
		return img_desc->transfer_function == buffer->transfer_function &&
				img_desc->primaries == buffer->primaries;
	}
	// If the output doesn't have colorimetry image description set, we can only
	// scan out buffers with default colorimetry (gamma2.2 transfer and sRGB
	// primaries) used in wlroots.
	return buffer->transfer_function == WLR_COLOR_TRANSFER_FUNCTION_GAMMA22 &&
			buffer->primaries == WLR_COLOR_NAMED_PRIMARIES_SRGB;
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
		struct wlr_render_list_entry *entry, struct wlr_output_state *state,
		const struct wlr_render_data *data) {
	struct wlr_scene_output *scene_output = data->output;
	struct wlr_scene_node *node = entry->node;

	if (!scene_output->scene->direct_scanout) {
		return SCANOUT_INELIGIBLE;
	}

	if (!wlr_scene_node_is_buffer(node)) {
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

	const struct wlr_output_image_description *img_desc = output_pending_image_description(scene_output->output, state);
	if (!color_management_is_scanout_allowed(img_desc, buffer)) {
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

	wlr_scene_node_get_size(node, &pending.buffer_dst_box.width, &pending.buffer_dst_box.height);
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

	if (scene_output->out_timeline) {
		scene_output->out_point++;
		wlr_output_state_set_signal_timeline(&pending, scene_output->out_timeline, scene_output->out_point);
	}

	if (buffer->color_encoding == WLR_COLOR_ENCODING_IDENTITY &&
			buffer->color_range == WLR_COLOR_RANGE_FULL) {
		// IDENTITY+FULL (used for RGB formats) is equivalent to no color
		// representation being set at all.
		wlr_output_state_set_color_encoding_and_range(&pending,
			WLR_COLOR_ENCODING_NONE, WLR_COLOR_RANGE_NONE);
	} else {
		wlr_output_state_set_color_encoding_and_range(&pending,
			buffer->color_encoding, buffer->color_range);
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
		.release_timeline = data->output->out_timeline,
		.release_point = data->output->out_point,
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

	wlr_output_state_set_color_transform(&gamma_pending, scene_output->gamma_lut_color_transform);
	scene_output->gamma_lut_changed = false;

	if (!wlr_output_test_state(scene_output->output, &gamma_pending)) {
		wlr_gamma_control_v1_send_failed_and_destroy(scene_output->gamma_lut);

		scene_output->gamma_lut = NULL;
		wlr_color_transform_unref(scene_output->gamma_lut_color_transform);
		scene_output->gamma_lut_color_transform = NULL;
		wlr_output_state_finish(&gamma_pending);
		return;
	}

	wlr_output_state_copy(state, &gamma_pending);
	wlr_output_state_finish(&gamma_pending);

}

static bool scene_output_combine_color_transforms(
		struct wlr_scene_output *scene_output, struct wlr_color_transform *supplied,
		const struct wlr_output_image_description *img_desc, bool render_gamma_lut) {
	bool result = false;
	struct wlr_color_transform *color_matrix = NULL;
	struct wlr_color_transform *inv_eotf = NULL;
	struct wlr_color_transform *user_gamma = NULL;

	if (img_desc != NULL) {
		assert(supplied == NULL);
		struct wlr_color_primaries primaries_srgb;
		wlr_color_primaries_from_named(&primaries_srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
		struct wlr_color_primaries primaries;
		wlr_color_primaries_from_named(&primaries, img_desc->primaries);
		float matrix[9];
		wlr_color_primaries_transform_absolute_colorimetric(&primaries_srgb, &primaries, matrix);

		struct wlr_color_luminances srgb_lum, dst_lum;
		wlr_color_transfer_function_get_default_luminance(
			WLR_COLOR_TRANSFER_FUNCTION_SRGB, &srgb_lum);
		wlr_color_transfer_function_get_default_luminance(img_desc->transfer_function, &dst_lum);
		float luminance_multiplier = get_luminance_multiplier(&srgb_lum, &dst_lum);
		for (int i = 0; i < 9; ++i) {
			matrix[i] *= luminance_multiplier;
		}

		color_matrix = wlr_color_transform_init_matrix(matrix);
		inv_eotf = wlr_color_transform_init_linear_to_inverse_eotf(img_desc->transfer_function);
		if (color_matrix == NULL || inv_eotf == NULL) {
			goto cleanup_transforms;
		}
	} else if (supplied != NULL) {
		inv_eotf = wlr_color_transform_ref(supplied);
	} else {
		inv_eotf = wlr_color_transform_init_linear_to_inverse_eotf(
			WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
		if (inv_eotf == NULL) {
			goto cleanup_transforms;
		}
	}

	struct wlr_color_transform *gamma_lut = scene_output->gamma_lut_color_transform;
	if (gamma_lut != NULL && render_gamma_lut) {
		user_gamma = wlr_color_transform_ref(gamma_lut);
	}

	struct wlr_color_transform *combined;
	struct wlr_color_transform *transforms[] = {
		color_matrix,
		inv_eotf,
		user_gamma,
	};
	const size_t transforms_len = sizeof(transforms) / sizeof(transforms[0]);
	if (!color_transform_compose(&combined, transforms, transforms_len)) {
		goto cleanup_transforms;
	}

	wlr_color_transform_unref(scene_output->prev_gamma_lut_color_transform);
	scene_output->prev_gamma_lut_color_transform = gamma_lut ? wlr_color_transform_ref(gamma_lut) : NULL;
	wlr_color_transform_unref(scene_output->prev_supplied_color_transform);
	scene_output->prev_supplied_color_transform = supplied ? wlr_color_transform_ref(supplied) : NULL;
	wlr_color_transform_unref(scene_output->combined_color_transform);
	scene_output->combined_color_transform = combined;

	result = true;

cleanup_transforms:
	wlr_color_transform_unref(color_matrix);
	wlr_color_transform_unref(inv_eotf);
	wlr_color_transform_unref(user_gamma);
	return result;
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

	bool render_gamma_lut = false;
	if (wlr_output_get_gamma_size(output) == 0 && output->renderer->features.output_color_transform) {
		if (scene_output->gamma_lut_color_transform != scene_output->prev_gamma_lut_color_transform) {
			scene_output_damage_whole(scene_output);
		}
		if (scene_output->gamma_lut_color_transform != NULL) {
			render_gamma_lut = true;
		}
	}

	struct wlr_render_data render_data = {
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

	struct wlr_render_list_constructor_data list_con = {
		.box = render_data.logical,
		.render_list = &scene_output->render_list,
		.calculate_visibility = scene_output->scene->calculate_visibility,
		.highlight_transparent_region = scene_output->scene->highlight_transparent_region,
		.fractional_scale = floor(render_data.scale) != render_data.scale,
	};

	list_con.render_list->size = 0;
	wlr_scene_node_nodes_in_box(&scene_output->scene->tree->node, &list_con.box,
		wlr_scene_node_construct_render_list_iterator, &list_con);
	array_realloc(list_con.render_list, list_con.render_list->size);

	struct wlr_render_list_entry *list_data = list_con.render_list->data;
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

	if ((render_gamma_lut
			&& scene_output->gamma_lut_color_transform != scene_output->prev_gamma_lut_color_transform)
			|| scene_output->prev_supplied_color_transform != options->color_transform
			|| (state->committed & WLR_OUTPUT_STATE_IMAGE_DESCRIPTION)) {
		const struct wlr_output_image_description *output_description =
			output_pending_image_description(output, state);
		if (!scene_output_combine_color_transforms(scene_output, options->color_transform,
				output_description, render_gamma_lut)) {
			wlr_buffer_unlock(buffer);
			return false;
		}
	}

	scene_output->in_point++;
	struct wlr_render_pass *render_pass = wlr_renderer_begin_buffer_pass(output->renderer, buffer,
			&(struct wlr_buffer_pass_options){
		.timer = timer ? timer->render_timer : NULL,
		.color_transform = scene_output->combined_color_transform,
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
			struct wlr_render_list_entry *entry = &list_data[i];

			// We must only cull opaque regions that are visible by the node.
			// The node's visibility will have the knowledge of a black rect
			// that may have been omitted from the render list via the black
			// rect optimization. In order to ensure we don't cull background
			// rendering in that black rect region, consider the node's visibility.
			pixman_region32_t opaque;
			pixman_region32_init(&opaque);
			wlr_scene_node_opaque_region(entry->node, entry->x, entry->y, &opaque);
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
		struct wlr_render_list_entry *entry = &list_data[i];
		wlr_scene_node_render(entry, &render_data);
		wlr_scene_node_dmabuf_feedback(entry, scene_output);
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
		scene_output->out_point++;
		wlr_output_state_set_signal_timeline(state, scene_output->out_timeline,
			scene_output->out_point);
	}

	if (!render_gamma_lut) {
		scene_output_state_attempt_gamma(scene_output, state);
	}

	return true;
}

void wlr_scene_timer_finish(struct wlr_scene_timer *timer) {
	if (timer->render_timer) {
		wlr_render_timer_destroy(timer->render_timer);
	}
}

void wlr_scene_output_send_frame_done(struct wlr_scene_output *scene_output,
		struct timespec *now) {
	wlr_scene_node_send_frame_done(&scene_output->scene->tree->node,
		scene_output, now);
}
