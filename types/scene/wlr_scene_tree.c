#include <wlr/types/wlr_scene_tree.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>
#include "types/wlr_output.h"
#include "types/wlr_scene.h"

#include <wayland-server-core.h>
#include <wayland-util.h>

#include <assert.h>
#include <stdlib.h>

#include <wlr/config.h>

static void scene_node_visibility(struct wlr_scene_node *node,
	pixman_region32_t *visible);

static void scene_node_destroy(struct wlr_scene_node *node) {
	struct wlr_scene *scene = node->scene;
	struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);

	if (scene_tree == scene->tree) {
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

	free(scene_tree);
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

	at_data->rx = rx;
	at_data->ry = ry;
	at_data->node = node;
	return true;
}

static struct wlr_scene_node *scene_node_at(struct wlr_scene_node *node,
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

	if (wlr_scene_node_nodes_in_box(node, &box, scene_node_at_iterator, &data)) {
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

static void scene_node_bounds(struct wlr_scene_node *node,
		int x, int y, pixman_region32_t *visible) {
	if (!node->enabled) {
		return;
	}

	struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &scene_tree->children, link) {
		wlr_scene_node_bounds(child, x + child->x, y + child->y, visible);
	}
}

static bool _scene_nodes_in_box(struct wlr_scene_node *node, struct wlr_box *box,
		scene_node_box_iterator_func_t iterator, void *user_data, int lx, int ly) {
	if (!node->enabled) {
		return false;
	}

	struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child;
	wl_list_for_each_reverse(child, &scene_tree->children, link) {
		if (wlr_scene_node_nodes_in_box(child, box, iterator, user_data)) {
			return true;
		}
	}

	return false;
}

static bool scene_nodes_in_box(struct wlr_scene_node *node, struct wlr_box *box,
		scene_node_box_iterator_func_t iterator, void *user_data) {
	int x, y;
	wlr_scene_node_coords(node, &x, &y);

	return _scene_nodes_in_box(node, box, iterator, user_data, x, y);
}

static void scale_region(pixman_region32_t *region, float scale, bool round_up) {
	wlr_region_scale(region, region, scale);

	if (round_up && floor(scale) != scale) {
		wlr_region_expand(region, region, 1);
	}
}

static void scene_node_visibility(struct wlr_scene_node *node,
		pixman_region32_t *visible) {
	if (!node->enabled) {
		return;
	}

	struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &scene_tree->children, link) {
		wlr_scene_node_visibility(child, visible);
	}
}

static void scene_node_send_frame_done(struct wlr_scene_node *node,
		struct wlr_scene_output *scene_output, struct timespec *now) {
	if (!node->enabled) {
		return;
	}

	struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &scene_tree->children, link) {
		wlr_scene_node_send_frame_done(child, scene_output, now);
	}
}


static bool scene_node_invisible(struct wlr_scene_node *node) {
	return true;
}

static bool construct_render_list_iterator(struct wlr_scene_node *node,
		int lx, int ly, void *_data) {
	struct wlr_render_list_constructor_data *data = _data;

	if (wlr_scene_node_invisible(node)) {
		return false;
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

	struct wlr_render_list_entry *entry = wl_array_add(data->render_list, sizeof(*entry));
	if (entry == NULL) {
		return false;
	}

	*entry = (struct wlr_render_list_entry){
		.node = node,
		.x = lx,
		.y = ly,
		.highlight_transparent_region = data->highlight_transparent_region,
	};

	return false;
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

static void scene_node_render(struct wlr_render_list_entry *entry, const struct wlr_render_data *data) {
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
	wlr_scene_node_get_size(node, &dst_box.width, &dst_box.height);
	transform_output_box(&dst_box, data);

	pixman_region32_t opaque;
	pixman_region32_init(&opaque);
	wlr_scene_node_opaque_region(node, x, y, &opaque);
	logical_to_buffer_coords(&opaque, data, false);
	pixman_region32_subtract(&opaque, &render_region, &opaque);

	assert(false);

	pixman_region32_fini(&opaque);
	pixman_region32_fini(&render_region);
}

static void get_scene_node_extents(struct wlr_scene_node *node, int lx, int ly,
		int *x_min, int *y_min, int *x_max, int *y_max) {
	struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &scene_tree->children, link) {
		wlr_scene_node_get_extents(child, lx + child->x, ly + child->y, x_min, y_min, x_max, y_max);
	}
}

static struct wl_list *scene_node_get_children(struct wlr_scene_node *node) {
	struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
	return &scene_tree->children;
}

static void scene_node_cleanup_when_disabled(struct wlr_scene_node *node,
		bool xwayland_restack, struct wl_list *outputs) {
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			if (!child->enabled) {
				continue;
			}

			wlr_scene_node_cleanup_when_disabled(child, xwayland_restack, outputs);
		}
		return;
}

static const struct wlr_scene_node_impl scene_node_impl = {
	.destroy = scene_node_destroy,
	.set_enabled = NULL,
	.set_position = NULL,
	.bounds = scene_node_bounds,
	.get_size = NULL,
	.coords = NULL,
	.at = scene_node_at,
	.in_box = scene_nodes_in_box,
	.opaque_region = NULL,
	.update_outputs = NULL,
	.update = NULL,
	.visibility = scene_node_visibility,
	.frame_done = scene_node_send_frame_done,
	.invisible = scene_node_invisible,
	.construct_render_list_iterator = construct_render_list_iterator,
	.render = scene_node_render,
	.get_extents = get_scene_node_extents,
	.get_children = scene_node_get_children,
	.restack_xwayland_surface = NULL,
	.cleanup_when_disabled = scene_node_cleanup_when_disabled,
};

static struct wlr_scene_tree *scene_tree_create(struct wlr_scene_tree *parent) {
	struct wlr_scene_tree *tree = calloc(1, sizeof(*tree));
	if (tree == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_scene_node_init(&tree->node, &scene_node_impl, parent);
	wl_list_init(&tree->children);
	return tree;
}

struct wlr_scene_tree *wlr_scene_tree_create(struct wlr_scene_tree *parent) {
	assert(parent);

	return scene_tree_create(parent);
}

struct wlr_scene_tree *wlr_root_scene_tree_create(struct wlr_scene *scene) {
	struct wlr_scene_tree *tree = scene_tree_create(NULL);
	tree->node.scene = scene;

	return tree;
}

bool wlr_scene_node_is_tree(const struct wlr_scene_node *node) {
	return node->impl == &scene_node_impl;
}

struct wlr_scene_tree *wlr_scene_tree_from_node(struct wlr_scene_node *node) {
	assert(node->impl == &scene_node_impl);

	struct wlr_scene_tree *scene_tree =
		wl_container_of(node, scene_tree, node);

	return scene_tree;
}
