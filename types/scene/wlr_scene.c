#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>
#include "types/wlr_scene.h"
#include "util/env.h"

#include <wlr/config.h>

#if WLR_HAS_XWAYLAND
#include <wlr/xwayland/xwayland.h>
#endif

struct wlr_scene_tree *wlr_scene_tree_from_node(struct wlr_scene_node *node) {
	assert(node->type == WLR_SCENE_NODE_TREE);
	struct wlr_scene_tree *tree = wl_container_of(node, tree, node);
	return tree;
}

struct wlr_scene_rect *wlr_scene_rect_from_node(struct wlr_scene_node *node) {
	assert(node->type == WLR_SCENE_NODE_RECT);
	struct wlr_scene_rect *rect = wl_container_of(node, rect, node);
	return rect;
}

struct wlr_scene_buffer *wlr_scene_buffer_from_node(
		struct wlr_scene_node *node) {
	assert(node->type == WLR_SCENE_NODE_BUFFER);
	struct wlr_scene_buffer *buffer = wl_container_of(node, buffer, node);
	return buffer;
}

struct wlr_scene *scene_node_get_root(struct wlr_scene_node *node) {
	struct wlr_scene_tree *tree;
	if (node->type == WLR_SCENE_NODE_TREE) {
		tree = wlr_scene_tree_from_node(node);
	} else {
		tree = node->parent;
	}

	while (tree->node.parent != NULL) {
		tree = tree->node.parent;
	}
	struct wlr_scene *scene = wl_container_of(tree, scene, tree);
	return scene;
}

static void scene_node_init(struct wlr_scene_node *node,
		enum wlr_scene_node_type type, struct wlr_scene_tree *parent) {
	*node = (struct wlr_scene_node){
		.type = type,
		.parent = parent,
		.enabled = true,
	};

	wl_list_init(&node->link);

	wl_signal_init(&node->events.destroy);
	pixman_region32_init(&node->visible);

	if (parent != NULL) {
		wl_list_insert(parent->children.prev, &node->link);
	}

	wlr_addon_set_init(&node->addons);
}

static void scene_buffer_set_buffer(struct wlr_scene_buffer *scene_buffer,
	struct wlr_buffer *buffer);

void wlr_scene_node_destroy(struct wlr_scene_node *node) {
	if (node == NULL) {
		return;
	}

	// We want to call the destroy listeners before we do anything else
	// in case the destroy signal would like to remove children before they
	// are recursively destroyed.
	wl_signal_emit_mutable(&node->events.destroy, NULL);
	wlr_addon_set_finish(&node->addons);

	wlr_scene_node_set_enabled(node, false);

	struct wlr_scene *scene = scene_node_get_root(node);
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);

		uint64_t active = scene_buffer->active_outputs;
		if (active) {
			struct wlr_scene_output *scene_output;
			wl_list_for_each(scene_output, &scene->outputs, link) {
				if (active & (1ull << scene_output->index)) {
					wl_signal_emit_mutable(&scene_buffer->events.output_leave,
						scene_output);
				}
			}
		}

		scene_buffer_set_buffer(scene_buffer, NULL);
		scene_buffer_set_texture(scene_buffer, NULL);
		pixman_region32_fini(&scene_buffer->opaque_region);
		wlr_drm_syncobj_timeline_unref(scene_buffer->wait_timeline);

		assert(wl_list_empty(&scene_buffer->events.output_leave.listener_list));
		assert(wl_list_empty(&scene_buffer->events.output_enter.listener_list));
		assert(wl_list_empty(&scene_buffer->events.outputs_update.listener_list));
		assert(wl_list_empty(&scene_buffer->events.output_sample.listener_list));
		assert(wl_list_empty(&scene_buffer->events.frame_done.listener_list));
	} else if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);

		if (scene_tree == &scene->tree) {
			assert(!node->parent);
			struct wlr_scene_output *scene_output, *scene_output_tmp;
			wl_list_for_each_safe(scene_output, scene_output_tmp, &scene->outputs, link) {
				wlr_scene_output_destroy(scene_output);
			}

			wl_list_remove(&scene->linux_dmabuf_v1_destroy.link);
			wl_list_remove(&scene->gamma_control_manager_v1_destroy.link);
			wl_list_remove(&scene->gamma_control_manager_v1_set_gamma.link);
		} else {
			assert(node->parent);
		}

		struct wlr_scene_node *child, *child_tmp;
		wl_list_for_each_safe(child, child_tmp,
				&scene_tree->children, link) {
			wlr_scene_node_destroy(child);
		}
	}

	assert(wl_list_empty(&node->events.destroy.listener_list));

	wl_list_remove(&node->link);
	pixman_region32_fini(&node->visible);
	free(node);
}

static void scene_tree_init(struct wlr_scene_tree *tree,
		struct wlr_scene_tree *parent) {
	*tree = (struct wlr_scene_tree){0};
	scene_node_init(&tree->node, WLR_SCENE_NODE_TREE, parent);
	wl_list_init(&tree->children);
}

struct wlr_scene *wlr_scene_create(void) {
	struct wlr_scene *scene = calloc(1, sizeof(*scene));
	if (scene == NULL) {
		return NULL;
	}

	scene_tree_init(&scene->tree, NULL);

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

struct wlr_scene_tree *wlr_scene_tree_create(struct wlr_scene_tree *parent) {
	assert(parent);

	struct wlr_scene_tree *tree = calloc(1, sizeof(*tree));
	if (tree == NULL) {
		return NULL;
	}

	scene_tree_init(tree, parent);
	return tree;
}

static bool _scene_nodes_in_box(struct wlr_scene_node *node, struct wlr_box *box,
		scene_node_box_iterator_func_t iterator, void *user_data, int lx, int ly) {
	if (!node->enabled) {
		return false;
	}

	switch (node->type) {
	case WLR_SCENE_NODE_TREE:;
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each_reverse(child, &scene_tree->children, link) {
			if (_scene_nodes_in_box(child, box, iterator, user_data, lx + child->x, ly + child->y)) {
				return true;
			}
		}
		break;
	case WLR_SCENE_NODE_RECT:
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_box node_box = { .x = lx, .y = ly };
		scene_node_get_size(node, &node_box.width, &node_box.height);

		if (wlr_box_intersection(&node_box, &node_box, box) &&
				iterator(node, lx, ly, user_data)) {
			return true;
		}
		break;
	}

	return false;
}

bool scene_nodes_in_box(struct wlr_scene_node *node, struct wlr_box *box,
		scene_node_box_iterator_func_t iterator, void *user_data) {
	int x, y;
	wlr_scene_node_coords(node, &x, &y);

	return _scene_nodes_in_box(node, box, iterator, user_data, x, y);
}

void scene_node_opaque_region(struct wlr_scene_node *node, int x, int y, pixman_region32_t *opaque) {
	int width, height;
	scene_node_get_size(node, &width, &height);

	if (node->type == WLR_SCENE_NODE_RECT) {
		struct wlr_scene_rect *scene_rect = wlr_scene_rect_from_node(node);
		if (scene_rect->color[3] != 1) {
			return;
		}
	} else if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);

		if (!scene_buffer->buffer) {
			return;
		}

		if (scene_buffer->opacity != 1) {
			return;
		}

		if (!scene_buffer->buffer_is_opaque) {
			pixman_region32_copy(opaque, &scene_buffer->opaque_region);
			pixman_region32_intersect_rect(opaque, opaque, 0, 0, width, height);
			pixman_region32_translate(opaque, x, y);
			return;
		}
	}

	pixman_region32_fini(opaque);
	pixman_region32_init_rect(opaque, x, y, width, height);
}

struct scene_update_data {
	pixman_region32_t *visible;
	pixman_region32_t *update_region;
	struct wlr_box update_box;
	struct wl_list *outputs;
	bool calculate_visibility;

#if WLR_HAS_XWAYLAND
	struct wlr_xwayland_surface *restack_above;
#endif
};

static uint32_t region_area(pixman_region32_t *region) {
	uint32_t area = 0;

	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(region, &nrects);
	for (int i = 0; i < nrects; ++i) {
		area += (rects[i].x2 - rects[i].x1) * (rects[i].y2 - rects[i].y1);
	}

	return area;
}

void scale_region(pixman_region32_t *region, float scale, bool round_up) {
	wlr_region_scale(region, region, scale);

	if (round_up && floor(scale) != scale) {
		wlr_region_expand(region, region, 1);
	}
}

static void scene_damage_outputs(struct wlr_scene *scene, pixman_region32_t *damage) {
	if (pixman_region32_empty(damage)) {
		return;
	}

	struct wlr_scene_output *scene_output;
	wl_list_for_each(scene_output, &scene->outputs, link) {
		pixman_region32_t output_damage;
		pixman_region32_init(&output_damage);
		pixman_region32_copy(&output_damage, damage);
		pixman_region32_translate(&output_damage,
			-scene_output->x, -scene_output->y);
		scale_region(&output_damage, scene_output->output->scale, true);
		output_to_buffer_coords(&output_damage, scene_output->output);
		scene_output_damage(scene_output, &output_damage);
		pixman_region32_fini(&output_damage);
	}
}

void update_node_update_outputs(struct wlr_scene_node *node,
		struct wl_list *outputs, struct wlr_scene_output *ignore,
		struct wlr_scene_output *force) {
	if (node->type != WLR_SCENE_NODE_BUFFER) {
		return;
	}

	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);

	uint32_t largest_overlap = 0;
	struct wlr_scene_output *old_primary_output = scene_buffer->primary_output;
	scene_buffer->primary_output = NULL;

	size_t count = 0;
	uint64_t active_outputs = 0;

	// let's update the outputs in two steps:
	//  - the primary outputs
	//  - the enter/leave signals
	// This ensures that the enter/leave signals can rely on the primary output
	// to have a reasonable value. Otherwise, they may get a value that's in
	// the middle of a calculation.
	struct wlr_scene_output *scene_output;
	wl_list_for_each(scene_output, outputs, link) {
		if (scene_output == ignore) {
			continue;
		}

		if (!scene_output->output->enabled) {
			continue;
		}

		struct wlr_box output_box = {
			.x = scene_output->x,
			.y = scene_output->y,
		};
		wlr_output_effective_resolution(scene_output->output,
			&output_box.width, &output_box.height);

		pixman_region32_t intersection;
		pixman_region32_init(&intersection);
		pixman_region32_intersect_rect(&intersection, &node->visible,
			output_box.x, output_box.y, output_box.width, output_box.height);

		if (!pixman_region32_empty(&intersection)) {
			uint32_t overlap = region_area(&intersection);
			if (overlap >= largest_overlap) {
				largest_overlap = overlap;
				scene_buffer->primary_output = scene_output;
			}

			active_outputs |= 1ull << scene_output->index;
			count++;
		}

		pixman_region32_fini(&intersection);
	}

	if (old_primary_output != scene_buffer->primary_output) {
		scene_buffer->prev_feedback_options =
			(struct wlr_linux_dmabuf_feedback_v1_init_options){0};
	}

	uint64_t old_active = scene_buffer->active_outputs;
	scene_buffer->active_outputs = active_outputs;

	wl_list_for_each(scene_output, outputs, link) {
		uint64_t mask = 1ull << scene_output->index;
		bool intersects = active_outputs & mask;
		bool intersects_before = old_active & mask;

		if (intersects && !intersects_before) {
			wl_signal_emit_mutable(&scene_buffer->events.output_enter, scene_output);
		} else if (!intersects && intersects_before) {
			wl_signal_emit_mutable(&scene_buffer->events.output_leave, scene_output);
		}
	}

	// if there are active outputs on this node, we should always have a primary
	// output
	assert(!scene_buffer->active_outputs || scene_buffer->primary_output);

	// Skip output update event if nothing was updated
	if (old_active == active_outputs &&
			(!force || ((1ull << force->index) & ~active_outputs)) &&
			old_primary_output == scene_buffer->primary_output) {
		return;
	}

	struct wlr_scene_output *outputs_array[64];
	struct wlr_scene_outputs_update_event event = {
		.active = outputs_array,
		.size = count,
	};

	size_t i = 0;
	wl_list_for_each(scene_output, outputs, link) {
		if (~active_outputs & (1ull << scene_output->index)) {
			continue;
		}

		assert(i < count);
		outputs_array[i++] = scene_output;
	}

	wl_signal_emit_mutable(&scene_buffer->events.outputs_update, &event);
}

#if WLR_HAS_XWAYLAND
static struct wlr_xwayland_surface *scene_node_try_get_managed_xwayland_surface(
		struct wlr_scene_node *node) {
	if (node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}

	struct wlr_scene_buffer *buffer_node = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *surface_node = wlr_scene_surface_try_from_buffer(buffer_node);
	if (!surface_node) {
		return NULL;
	}

	struct wlr_xwayland_surface *xwayland_surface =
		wlr_xwayland_surface_try_from_wlr_surface(surface_node->surface);
	if (!xwayland_surface || xwayland_surface->override_redirect) {
		return NULL;
	}

	return xwayland_surface;
}

static void restack_xwayland_surface(struct wlr_scene_node *node,
		struct wlr_box *box, struct scene_update_data *data) {
	struct wlr_xwayland_surface *xwayland_surface =
		scene_node_try_get_managed_xwayland_surface(node);
	if (!xwayland_surface) {
		return;
	}

	// ensure this node is entirely inside the update region. If not, we can't
	// restack this node since we're not considering the whole thing.
	if (wlr_box_contains_box(&data->update_box, box)) {
		if (data->restack_above) {
			wlr_xwayland_surface_restack(xwayland_surface, data->restack_above, XCB_STACK_MODE_BELOW);
		} else {
			wlr_xwayland_surface_restack(xwayland_surface, NULL, XCB_STACK_MODE_ABOVE);
		}
	}

	data->restack_above = xwayland_surface;
}

static void restack_xwayland_surface_below(struct wlr_scene_node *node) {
	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			restack_xwayland_surface_below(child);
		}
		return;
	}

	struct wlr_xwayland_surface *xwayland_surface =
		scene_node_try_get_managed_xwayland_surface(node);
	if (!xwayland_surface) {
		return;
	}

	wlr_xwayland_surface_restack(xwayland_surface, NULL, XCB_STACK_MODE_BELOW);
}
#endif

static bool scene_node_update_iterator(struct wlr_scene_node *node,
		int lx, int ly, void *_data) {
	struct scene_update_data *data = _data;

	struct wlr_box box = { .x = lx, .y = ly };
	scene_node_get_size(node, &box.width, &box.height);

	pixman_region32_subtract(&node->visible, &node->visible, data->update_region);
	pixman_region32_union(&node->visible, &node->visible, data->visible);
	pixman_region32_intersect_rect(&node->visible, &node->visible,
		lx, ly, box.width, box.height);

	if (data->calculate_visibility) {
		pixman_region32_t opaque;
		pixman_region32_init(&opaque);
		scene_node_opaque_region(node, lx, ly, &opaque);
		pixman_region32_subtract(data->visible, data->visible, &opaque);
		pixman_region32_fini(&opaque);
	}

	update_node_update_outputs(node, data->outputs, NULL, NULL);
#if WLR_HAS_XWAYLAND
	restack_xwayland_surface(node, &box, data);
#endif

	return false;
}

static void scene_node_visibility(struct wlr_scene_node *node,
		pixman_region32_t *visible) {
	if (!node->enabled) {
		return;
	}

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_node_visibility(child, visible);
		}
		return;
	}

	pixman_region32_union(visible, visible, &node->visible);
}

static void scene_node_bounds(struct wlr_scene_node *node,
		int x, int y, pixman_region32_t *visible) {
	if (!node->enabled) {
		return;
	}

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_node_bounds(child, x + child->x, y + child->y, visible);
		}
		return;
	}

	int width, height;
	scene_node_get_size(node, &width, &height);
	pixman_region32_union_rect(visible, visible, x, y, width, height);
}

static void scene_update_region(struct wlr_scene *scene,
		pixman_region32_t *update_region) {
	pixman_region32_t visible;
	pixman_region32_init(&visible);
	pixman_region32_copy(&visible, update_region);

	struct pixman_box32 *region_box = pixman_region32_extents(update_region);
	struct scene_update_data data = {
		.visible = &visible,
		.update_region = update_region,
		.update_box = {
			.x = region_box->x1,
			.y = region_box->y1,
			.width = region_box->x2 - region_box->x1,
			.height = region_box->y2 - region_box->y1,
		},
		.outputs = &scene->outputs,
		.calculate_visibility = scene->calculate_visibility,
	};

	// update node visibility and output enter/leave events
	scene_nodes_in_box(&scene->tree.node, &data.update_box, scene_node_update_iterator, &data);

	pixman_region32_fini(&visible);
}

static void scene_node_update(struct wlr_scene_node *node,
		pixman_region32_t *damage) {
	struct wlr_scene *scene = scene_node_get_root(node);

	int x, y;
	if (!wlr_scene_node_coords(node, &x, &y)) {
#if WLR_HAS_XWAYLAND
		restack_xwayland_surface_below(node);
#endif
		if (damage) {
			scene_update_region(scene, damage);
			scene_damage_outputs(scene, damage);
			pixman_region32_fini(damage);
		}

		return;
	}

	pixman_region32_t visible;
	if (!damage) {
		pixman_region32_init(&visible);
		scene_node_visibility(node, &visible);
		damage = &visible;
	}

	pixman_region32_t update_region;
	pixman_region32_init(&update_region);
	pixman_region32_copy(&update_region, damage);
	scene_node_bounds(node, x, y, &update_region);

	scene_update_region(scene, &update_region);
	pixman_region32_fini(&update_region);

	scene_node_visibility(node, damage);
	scene_damage_outputs(scene, damage);
	pixman_region32_fini(damage);
}

struct wlr_scene_rect *wlr_scene_rect_create(struct wlr_scene_tree *parent,
		int width, int height, const float color[static 4]) {
	assert(parent);
	assert(width >= 0 && height >= 0);

	struct wlr_scene_rect *scene_rect = calloc(1, sizeof(*scene_rect));
	if (scene_rect == NULL) {
		return NULL;
	}
	scene_node_init(&scene_rect->node, WLR_SCENE_NODE_RECT, parent);

	scene_rect->width = width;
	scene_rect->height = height;
	memcpy(scene_rect->color, color, sizeof(scene_rect->color));

	scene_node_update(&scene_rect->node, NULL);

	return scene_rect;
}

void wlr_scene_rect_set_size(struct wlr_scene_rect *rect, int width, int height) {
	if (rect->width == width && rect->height == height) {
		return;
	}

	assert(width >= 0 && height >= 0);

	rect->width = width;
	rect->height = height;
	scene_node_update(&rect->node, NULL);
}

void wlr_scene_rect_set_color(struct wlr_scene_rect *rect, const float color[static 4]) {
	if (memcmp(rect->color, color, sizeof(rect->color)) == 0) {
		return;
	}

	memcpy(rect->color, color, sizeof(rect->color));
	scene_node_update(&rect->node, NULL);
}

static void scene_buffer_handle_buffer_release(struct wl_listener *listener,
		void *data) {
	struct wlr_scene_buffer *scene_buffer =
		wl_container_of(listener, scene_buffer, buffer_release);

	scene_buffer->buffer = NULL;
	wl_list_remove(&scene_buffer->buffer_release.link);
	wl_list_init(&scene_buffer->buffer_release.link);
}

static void scene_buffer_set_buffer(struct wlr_scene_buffer *scene_buffer,
		struct wlr_buffer *buffer) {
	wl_list_remove(&scene_buffer->buffer_release.link);
	wl_list_init(&scene_buffer->buffer_release.link);
	if (scene_buffer->own_buffer) {
		wlr_buffer_unlock(scene_buffer->buffer);
	}
	scene_buffer->buffer = NULL;
	scene_buffer->own_buffer = false;
	scene_buffer->buffer_width = scene_buffer->buffer_height = 0;
	scene_buffer->buffer_is_opaque = false;

	if (!buffer) {
		return;
	}

	scene_buffer->own_buffer = true;
	scene_buffer->buffer = wlr_buffer_lock(buffer);
	scene_buffer->buffer_width = buffer->width;
	scene_buffer->buffer_height = buffer->height;
	scene_buffer->buffer_is_opaque = wlr_buffer_is_opaque(buffer);

	scene_buffer->buffer_release.notify = scene_buffer_handle_buffer_release;
	wl_signal_add(&buffer->events.release, &scene_buffer->buffer_release);
}

static void scene_buffer_handle_renderer_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_scene_buffer *scene_buffer = wl_container_of(listener, scene_buffer, renderer_destroy);
	scene_buffer_set_texture(scene_buffer, NULL);
}

void scene_buffer_set_texture(struct wlr_scene_buffer *scene_buffer, struct wlr_texture *texture) {
	wl_list_remove(&scene_buffer->renderer_destroy.link);
	wlr_texture_destroy(scene_buffer->texture);
	scene_buffer->texture = texture;

	if (texture != NULL) {
		scene_buffer->renderer_destroy.notify = scene_buffer_handle_renderer_destroy;
		wl_signal_add(&texture->renderer->events.destroy, &scene_buffer->renderer_destroy);
	} else {
		wl_list_init(&scene_buffer->renderer_destroy.link);
	}
}

static void scene_buffer_set_wait_timeline(struct wlr_scene_buffer *scene_buffer,
		struct wlr_drm_syncobj_timeline *timeline, uint64_t point) {
	wlr_drm_syncobj_timeline_unref(scene_buffer->wait_timeline);
	if (timeline != NULL) {
		scene_buffer->wait_timeline = wlr_drm_syncobj_timeline_ref(timeline);
		scene_buffer->wait_point = point;
	} else {
		scene_buffer->wait_timeline = NULL;
		scene_buffer->wait_point = 0;
	}
}

struct wlr_scene_buffer *wlr_scene_buffer_create(struct wlr_scene_tree *parent,
		struct wlr_buffer *buffer) {
	struct wlr_scene_buffer *scene_buffer = calloc(1, sizeof(*scene_buffer));
	if (scene_buffer == NULL) {
		return NULL;
	}
	assert(parent);
	scene_node_init(&scene_buffer->node, WLR_SCENE_NODE_BUFFER, parent);

	wl_signal_init(&scene_buffer->events.outputs_update);
	wl_signal_init(&scene_buffer->events.output_enter);
	wl_signal_init(&scene_buffer->events.output_leave);
	wl_signal_init(&scene_buffer->events.output_sample);
	wl_signal_init(&scene_buffer->events.frame_done);

	pixman_region32_init(&scene_buffer->opaque_region);
	wl_list_init(&scene_buffer->buffer_release.link);
	wl_list_init(&scene_buffer->renderer_destroy.link);
	scene_buffer->opacity = 1;

	scene_buffer_set_buffer(scene_buffer, buffer);
	scene_node_update(&scene_buffer->node, NULL);

	return scene_buffer;
}

void wlr_scene_buffer_set_buffer_with_options(struct wlr_scene_buffer *scene_buffer,
		struct wlr_buffer *buffer, const struct wlr_scene_buffer_set_buffer_options *options) {
	const struct wlr_scene_buffer_set_buffer_options default_options = {0};
	if (options == NULL) {
		options = &default_options;
	}

	// specifying a region for a NULL buffer doesn't make sense. We need to know
	// about the buffer to scale the buffer local coordinates down to scene
	// coordinates.
	assert(buffer || !options->damage);

	bool mapped = buffer != NULL;
	bool prev_mapped = scene_buffer->buffer != NULL || scene_buffer->texture != NULL;

	if (!mapped && !prev_mapped) {
		// unmapping already unmapped buffer - noop
		return;
	}

	// if this node used to not be mapped or its previous displayed
	// buffer region will be different from what the new buffer would
	// produce we need to update the node.
	bool update = mapped != prev_mapped;
	if (buffer != NULL && scene_buffer->dst_width == 0 && scene_buffer->dst_height == 0) {
		update = update || scene_buffer->buffer_width != buffer->width ||
			scene_buffer->buffer_height != buffer->height;
	}

	// If this is a buffer change, check if it's a single pixel buffer.
	// Cache that so we can still apply rendering optimisations even when
	// the original buffer has been freed after texture upload.
	if (buffer != scene_buffer->buffer) {
		scene_buffer->is_single_pixel_buffer = false;
		struct wlr_client_buffer *client_buffer = NULL;
		if (buffer != NULL) {
			client_buffer = wlr_client_buffer_get(buffer);
		}
		if (client_buffer != NULL && client_buffer->source != NULL) {
			struct wlr_single_pixel_buffer_v1 *single_pixel_buffer =
				wlr_single_pixel_buffer_v1_try_from_buffer(client_buffer->source);
			if (single_pixel_buffer != NULL) {
				scene_buffer->is_single_pixel_buffer = true;
				scene_buffer->single_pixel_buffer_color[0] = single_pixel_buffer->r;
				scene_buffer->single_pixel_buffer_color[1] = single_pixel_buffer->g;
				scene_buffer->single_pixel_buffer_color[2] = single_pixel_buffer->b;
				scene_buffer->single_pixel_buffer_color[3] = single_pixel_buffer->a;
			}
		}
	}

	scene_buffer_set_buffer(scene_buffer, buffer);
	scene_buffer_set_texture(scene_buffer, NULL);
	scene_buffer_set_wait_timeline(scene_buffer,
		options->wait_timeline, options->wait_point);

	if (update) {
		scene_node_update(&scene_buffer->node, NULL);
		// updating the node will already damage the whole node for us. Return
		// early to not damage again
		return;
	}

	int lx, ly;
	if (!wlr_scene_node_coords(&scene_buffer->node, &lx, &ly)) {
		return;
	}

	pixman_region32_t fallback_damage;
	pixman_region32_init_rect(&fallback_damage, 0, 0, buffer->width, buffer->height);
	const pixman_region32_t *damage = options->damage;
	if (!damage) {
		damage = &fallback_damage;
	}

	struct wlr_fbox box = scene_buffer->src_box;
	if (wlr_fbox_empty(&box)) {
		box.x = 0;
		box.y = 0;
		box.width = buffer->width;
		box.height = buffer->height;
	}

	wlr_fbox_transform(&box, &box, scene_buffer->transform,
		buffer->width, buffer->height);

	float scale_x, scale_y;
	if (scene_buffer->dst_width || scene_buffer->dst_height) {
		scale_x = scene_buffer->dst_width / box.width;
		scale_y = scene_buffer->dst_height / box.height;
	} else {
		scale_x = buffer->width / box.width;
		scale_y = buffer->height / box.height;
	}

	pixman_region32_t trans_damage;
	pixman_region32_init(&trans_damage);
	wlr_region_transform(&trans_damage, damage,
		scene_buffer->transform, buffer->width, buffer->height);
	pixman_region32_intersect_rect(&trans_damage, &trans_damage,
		box.x, box.y, box.width, box.height);
	pixman_region32_translate(&trans_damage, -box.x, -box.y);

	struct wlr_scene *scene = scene_node_get_root(&scene_buffer->node);
	struct wlr_scene_output *scene_output;
	wl_list_for_each(scene_output, &scene->outputs, link) {
		float output_scale = scene_output->output->scale;
		float output_scale_x = output_scale * scale_x;
		float output_scale_y = output_scale * scale_y;
		pixman_region32_t output_damage;
		pixman_region32_init(&output_damage);
		wlr_region_scale_xy(&output_damage, &trans_damage,
			output_scale_x, output_scale_y);

		// One output pixel will match (buffer_scale_x)x(buffer_scale_y) buffer pixels.
		// If the buffer is upscaled on the given axis (output_scale_* > 1.0,
		// buffer_scale_* < 1.0), its contents will bleed into adjacent
		// (ceil(output_scale_* / 2)) output pixels because of linear filtering.
		// Additionally, if the buffer is downscaled (output_scale_* < 1.0,
		// buffer_scale_* > 1.0), and one output pixel matches a non-integer number of
		// buffer pixels, its contents will bleed into neighboring output pixels.
		// Handle both cases by computing buffer_scale_{x,y} and checking if they are
		// integer numbers; ceilf() is used to ensure that the distance is at least 1.
		float buffer_scale_x = 1.0f / output_scale_x;
		float buffer_scale_y = 1.0f / output_scale_y;
		int dist_x = floor(buffer_scale_x) != buffer_scale_x ?
			(int)ceilf(output_scale_x / 2.0f) : 0;
		int dist_y = floor(buffer_scale_y) != buffer_scale_y ?
			(int)ceilf(output_scale_y / 2.0f) : 0;
		// TODO: expand with per-axis distances
		wlr_region_expand(&output_damage, &output_damage,
			dist_x >= dist_y ? dist_x : dist_y);

		pixman_region32_t cull_region;
		pixman_region32_init(&cull_region);
		pixman_region32_copy(&cull_region, &scene_buffer->node.visible);
		scale_region(&cull_region, output_scale, true);
		pixman_region32_translate(&cull_region, -lx * output_scale, -ly * output_scale);
		pixman_region32_intersect(&output_damage, &output_damage, &cull_region);
		pixman_region32_fini(&cull_region);

		pixman_region32_translate(&output_damage,
			(int)round((lx - scene_output->x) * output_scale),
			(int)round((ly - scene_output->y) * output_scale));
		output_to_buffer_coords(&output_damage, scene_output->output);
		scene_output_damage(scene_output, &output_damage);
		pixman_region32_fini(&output_damage);
	}

	pixman_region32_fini(&trans_damage);
	pixman_region32_fini(&fallback_damage);
}

void wlr_scene_buffer_set_buffer_with_damage(struct wlr_scene_buffer *scene_buffer,
		struct wlr_buffer *buffer, const pixman_region32_t *damage) {
	const struct wlr_scene_buffer_set_buffer_options options = {
		.damage = damage,
	};
	wlr_scene_buffer_set_buffer_with_options(scene_buffer, buffer, &options);
}

void wlr_scene_buffer_set_buffer(struct wlr_scene_buffer *scene_buffer,
		struct wlr_buffer *buffer)  {
	wlr_scene_buffer_set_buffer_with_options(scene_buffer, buffer, NULL);
}

void wlr_scene_buffer_set_opaque_region(struct wlr_scene_buffer *scene_buffer,
		const pixman_region32_t *region) {
	if (pixman_region32_equal(&scene_buffer->opaque_region, region)) {
		return;
	}

	pixman_region32_copy(&scene_buffer->opaque_region, region);

	int x, y;
	if (!wlr_scene_node_coords(&scene_buffer->node, &x, &y)) {
		return;
	}

	pixman_region32_t update_region;
	pixman_region32_init(&update_region);
	scene_node_bounds(&scene_buffer->node, x, y, &update_region);
	scene_update_region(scene_node_get_root(&scene_buffer->node), &update_region);
	pixman_region32_fini(&update_region);
}

void wlr_scene_buffer_set_source_box(struct wlr_scene_buffer *scene_buffer,
		const struct wlr_fbox *box) {
	if (wlr_fbox_equal(&scene_buffer->src_box, box)) {
		return;
	}

	if (box != NULL) {
		assert(box->x >= 0 && box->y >= 0 && box->width >= 0 && box->height >= 0);
		scene_buffer->src_box = *box;
	} else {
		scene_buffer->src_box = (struct wlr_fbox){0};
	}

	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_dest_size(struct wlr_scene_buffer *scene_buffer,
		int width, int height) {
	if (scene_buffer->dst_width == width && scene_buffer->dst_height == height) {
		return;
	}

	assert(width >= 0 && height >= 0);
	scene_buffer->dst_width = width;
	scene_buffer->dst_height = height;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_transform(struct wlr_scene_buffer *scene_buffer,
		enum wl_output_transform transform) {
	if (scene_buffer->transform == transform) {
		return;
	}

	scene_buffer->transform = transform;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_send_frame_done(struct wlr_scene_buffer *scene_buffer,
		struct timespec *now) {
	if (!pixman_region32_empty(&scene_buffer->node.visible)) {
		wl_signal_emit_mutable(&scene_buffer->events.frame_done, now);
	}
}

void wlr_scene_buffer_set_opacity(struct wlr_scene_buffer *scene_buffer,
		float opacity) {
	if (scene_buffer->opacity == opacity) {
		return;
	}

	assert(opacity >= 0 && opacity <= 1);
	scene_buffer->opacity = opacity;
	scene_node_update(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_filter_mode(struct wlr_scene_buffer *scene_buffer,
		enum wlr_scale_filter_mode filter_mode) {
	if (scene_buffer->filter_mode == filter_mode) {
		return;
	}

	scene_buffer->filter_mode = filter_mode;
	scene_node_update(&scene_buffer->node, NULL);
}

void scene_node_get_size(struct wlr_scene_node *node, int *width, int *height) {
	*width = 0;
	*height = 0;

	switch (node->type) {
	case WLR_SCENE_NODE_TREE:
		return;
	case WLR_SCENE_NODE_RECT:;
		struct wlr_scene_rect *scene_rect = wlr_scene_rect_from_node(node);
		*width = scene_rect->width;
		*height = scene_rect->height;
		break;
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
		if (scene_buffer->dst_width > 0 && scene_buffer->dst_height > 0) {
			*width = scene_buffer->dst_width;
			*height = scene_buffer->dst_height;
		} else {
			*width = scene_buffer->buffer_width;
			*height = scene_buffer->buffer_height;
			wlr_output_transform_coords(scene_buffer->transform, width, height);
		}
		break;
	}
}

void wlr_scene_node_set_enabled(struct wlr_scene_node *node, bool enabled) {
	if (node->enabled == enabled) {
		return;
	}

	int x, y;
	pixman_region32_t visible;
	pixman_region32_init(&visible);
	if (wlr_scene_node_coords(node, &x, &y)) {
		scene_node_visibility(node, &visible);
	}

	node->enabled = enabled;

	scene_node_update(node, &visible);
}

void wlr_scene_node_set_position(struct wlr_scene_node *node, int x, int y) {
	if (node->x == x && node->y == y) {
		return;
	}

	node->x = x;
	node->y = y;
	scene_node_update(node, NULL);
}

void wlr_scene_node_place_above(struct wlr_scene_node *node,
		struct wlr_scene_node *sibling) {
	assert(node != sibling);
	assert(node->parent == sibling->parent);

	if (node->link.prev == &sibling->link) {
		return;
	}

	wl_list_remove(&node->link);
	wl_list_insert(&sibling->link, &node->link);
	scene_node_update(node, NULL);
}

void wlr_scene_node_place_below(struct wlr_scene_node *node,
		struct wlr_scene_node *sibling) {
	assert(node != sibling);
	assert(node->parent == sibling->parent);

	if (node->link.next == &sibling->link) {
		return;
	}

	wl_list_remove(&node->link);
	wl_list_insert(sibling->link.prev, &node->link);
	scene_node_update(node, NULL);
}

void wlr_scene_node_raise_to_top(struct wlr_scene_node *node) {
	struct wlr_scene_node *current_top = wl_container_of(
		node->parent->children.prev, current_top, link);
	if (node == current_top) {
		return;
	}
	wlr_scene_node_place_above(node, current_top);
}

void wlr_scene_node_lower_to_bottom(struct wlr_scene_node *node) {
	struct wlr_scene_node *current_bottom = wl_container_of(
		node->parent->children.next, current_bottom, link);
	if (node == current_bottom) {
		return;
	}
	wlr_scene_node_place_below(node, current_bottom);
}

void wlr_scene_node_reparent(struct wlr_scene_node *node,
		struct wlr_scene_tree *new_parent) {
	assert(new_parent != NULL);

	if (node->parent == new_parent) {
		return;
	}

	/* Ensure that a node cannot become its own ancestor */
	for (struct wlr_scene_tree *ancestor = new_parent; ancestor != NULL;
			ancestor = ancestor->node.parent) {
		assert(&ancestor->node != node);
	}

	int x, y;
	pixman_region32_t visible;
	pixman_region32_init(&visible);
	if (wlr_scene_node_coords(node, &x, &y)) {
		scene_node_visibility(node, &visible);
	}

	wl_list_remove(&node->link);
	node->parent = new_parent;
	wl_list_insert(new_parent->children.prev, &node->link);
	scene_node_update(node, &visible);
}

bool wlr_scene_node_coords(struct wlr_scene_node *node,
		int *lx_ptr, int *ly_ptr) {
	assert(node);

	int lx = 0, ly = 0;
	bool enabled = true;
	while (true) {
		lx += node->x;
		ly += node->y;
		enabled = enabled && node->enabled;
		if (node->parent == NULL) {
			break;
		}

		node = &node->parent->node;
	}

	*lx_ptr = lx;
	*ly_ptr = ly;
	return enabled;
}

static void scene_node_for_each_scene_buffer(struct wlr_scene_node *node,
		int lx, int ly, wlr_scene_buffer_iterator_func_t user_iterator,
		void *user_data) {
	if (!node->enabled) {
		return;
	}

	lx += node->x;
	ly += node->y;

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
		user_iterator(scene_buffer, lx, ly, user_data);
	} else if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_node_for_each_scene_buffer(child, lx, ly, user_iterator, user_data);
		}
	}
}

void wlr_scene_node_for_each_buffer(struct wlr_scene_node *node,
		wlr_scene_buffer_iterator_func_t user_iterator, void *user_data) {
	scene_node_for_each_scene_buffer(node, 0, 0, user_iterator, user_data);
}

struct node_at_data {
	double lx, ly;
	double rx, ry;
	struct wlr_scene_node *node;
};

static bool scene_node_at_iterator(struct wlr_scene_node *node,
		int lx, int ly, void *data) {
	struct node_at_data *at_data = data;

	double rx = at_data->lx - lx;
	double ry = at_data->ly - ly;

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);

		if (scene_buffer->point_accepts_input &&
				!scene_buffer->point_accepts_input(scene_buffer, &rx, &ry)) {
			return false;
		}
	}

	at_data->rx = rx;
	at_data->ry = ry;
	at_data->node = node;
	return true;
}

struct wlr_scene_node *wlr_scene_node_at(struct wlr_scene_node *node,
		double lx, double ly, double *nx, double *ny) {
	struct wlr_box box = {
		.x = floor(lx),
		.y = floor(ly),
		.width = 1,
		.height = 1
	};

	struct node_at_data data = {
		.lx = lx,
		.ly = ly
	};

	if (scene_nodes_in_box(node, &box, scene_node_at_iterator, &data)) {
		if (nx) {
			*nx = data.rx;
		}
		if (ny) {
			*ny = data.ry;
		}
		return data.node;
	}

	return NULL;
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

int64_t wlr_scene_timer_get_duration_ns(struct wlr_scene_timer *timer) {
	int64_t pre_render = timer->pre_render_duration;
	if (!timer->render_timer) {
		return pre_render;
	}
	int64_t render = wlr_render_timer_get_duration_ns(timer->render_timer);
	return render != -1 ? pre_render + render : -1;
}

void wlr_scene_timer_finish(struct wlr_scene_timer *timer) {
	if (timer->render_timer) {
		wlr_render_timer_destroy(timer->render_timer);
	}
}
