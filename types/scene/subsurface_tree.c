#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/addon.h>
#include "types/wlr_scene.h"

/**
 * A tree for a surface and all of its child sub-surfaces.
 *
 * `tree` contains `scene_surface` and one node per sub-surface.
 */
struct wlr_scene_subsurface_tree_surface {
	struct wlr_scene_tree *tree;
	struct wlr_surface *surface;
	struct wlr_scene_surface *scene_surface;
	struct wlr_scene_subsurface_tree *subsurface_tree;

	struct wl_listener surface_destroy;
	struct wl_listener surface_commit;
	struct wl_listener surface_map;
	struct wl_listener surface_unmap;
	struct wl_listener surface_new_subsurface;
	struct wl_listener scene_destroy;

	struct wlr_scene_subsurface_tree_surface *parent; // NULL for the top-level surface

	struct wlr_box clip;
	struct wl_list link;

	// Only valid if the surface is a sub-surface

	struct wlr_addon surface_addon;

	struct wl_listener subsurface_destroy;
};

static void subsurface_tree_handle_scene_destroy(struct wl_listener *listener, void *data) {
	struct wlr_scene_subsurface_tree_surface *subsurface_tree =
		wl_container_of(listener, subsurface_tree, scene_destroy);
	// tree and scene_surface will be cleaned up by scene_node_finish
	if (subsurface_tree->parent) {
		wlr_addon_finish(&subsurface_tree->surface_addon);
		wl_list_remove(&subsurface_tree->subsurface_destroy.link);
	} else {
		free(subsurface_tree->subsurface_tree);
	}
	wl_list_remove(&subsurface_tree->surface_destroy.link);
	wl_list_remove(&subsurface_tree->surface_commit.link);
	wl_list_remove(&subsurface_tree->surface_map.link);
	wl_list_remove(&subsurface_tree->surface_unmap.link);
	wl_list_remove(&subsurface_tree->surface_new_subsurface.link);
	wl_list_remove(&subsurface_tree->scene_destroy.link);
	wl_list_remove(&subsurface_tree->link);
	free(subsurface_tree);
}

static const struct wlr_addon_interface subsurface_tree_surface_addon_impl;

static struct wlr_scene_subsurface_tree_surface *subsurface_tree_from_subsurface(
		struct wlr_scene_subsurface_tree_surface *parent,
		struct wlr_subsurface *subsurface) {
	struct wlr_addon *addon = wlr_addon_find(&subsurface->surface->addons,
		parent, &subsurface_tree_surface_addon_impl);
	assert(addon != NULL);
	struct wlr_scene_subsurface_tree_surface *subsurface_tree =
		wl_container_of(addon, subsurface_tree, surface_addon);
	return subsurface_tree;
}

static bool subsurface_tree_reconfigure_clip(
		struct wlr_scene_subsurface_tree_surface *subsurface_tree) {
	if (subsurface_tree->parent) {
		subsurface_tree->clip = (struct wlr_box){
			.x = subsurface_tree->parent->clip.x - subsurface_tree->tree->node.x,
			.y = subsurface_tree->parent->clip.y - subsurface_tree->tree->node.y,
			.width = subsurface_tree->parent->clip.width,
			.height = subsurface_tree->parent->clip.height,
		};
	}

	if (wlr_box_empty(&subsurface_tree->clip)) {
		scene_surface_set_clip(subsurface_tree->scene_surface, NULL);
		wlr_scene_node_set_enabled(&subsurface_tree->scene_surface->buffer->node, true);
		wlr_scene_node_set_position(&subsurface_tree->scene_surface->buffer->node, 0, 0);

		return false;
	} else {
		struct wlr_box clip = subsurface_tree->clip;
		struct wlr_box surface_box = {
			.width = subsurface_tree->surface->current.width,
			.height = subsurface_tree->surface->current.height,
		};

		bool intersects = wlr_box_intersection(&clip, &clip, &surface_box);
		wlr_scene_node_set_enabled(&subsurface_tree->scene_surface->buffer->node, intersects);

		if (intersects) {
			wlr_scene_node_set_position(&subsurface_tree->scene_surface->buffer->node, clip.x, clip.y);
			scene_surface_set_clip(subsurface_tree->scene_surface, &clip);
		}

		return true;
	}
}

static void subsurface_tree_reconfigure(
		struct wlr_scene_subsurface_tree_surface *subsurface_tree) {
	bool has_clip = subsurface_tree_reconfigure_clip(subsurface_tree);

	struct wlr_surface *surface = subsurface_tree->surface;

	struct wlr_scene_node *prev = NULL;
	struct wlr_subsurface *subsurface;
	wl_list_for_each(subsurface, &surface->current.subsurfaces_below,
			current.link) {
		struct wlr_scene_subsurface_tree_surface *child =
			subsurface_tree_from_subsurface(subsurface_tree, subsurface);
		if (prev != NULL) {
			wlr_scene_node_place_above(&child->tree->node, prev);
		}
		prev = &child->tree->node;

		wlr_scene_node_set_position(&child->tree->node,
			subsurface->current.x, subsurface->current.y);

		if (has_clip) {
			subsurface_tree_reconfigure_clip(child);
		}
	}

	if (prev != NULL) {
		wlr_scene_node_place_above(&subsurface_tree->scene_surface->buffer->node, prev);
	}
	prev = &subsurface_tree->scene_surface->buffer->node;

	wl_list_for_each(subsurface, &surface->current.subsurfaces_above,
			current.link) {
		struct wlr_scene_subsurface_tree_surface *child =
			subsurface_tree_from_subsurface(subsurface_tree, subsurface);
		wlr_scene_node_place_above(&child->tree->node, prev);
		prev = &child->tree->node;

		wlr_scene_node_set_position(&child->tree->node,
			subsurface->current.x, subsurface->current.y);

		if (has_clip) {
			subsurface_tree_reconfigure_clip(child);
		}
	}
}

static void subsurface_tree_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_scene_subsurface_tree_surface *subsurface_tree =
		wl_container_of(listener, subsurface_tree, surface_destroy);
	wlr_scene_node_destroy(&subsurface_tree->tree->node);
}

static void subsurface_tree_handle_surface_commit(struct wl_listener *listener,
		void *data) {
	struct wlr_scene_subsurface_tree_surface *subsurface_tree =
		wl_container_of(listener, subsurface_tree, surface_commit);

	// TODO: only do this on subsurface order or position change
	subsurface_tree_reconfigure(subsurface_tree);
}

static void subsurface_tree_handle_subsurface_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_scene_subsurface_tree_surface *subsurface_tree =
		wl_container_of(listener, subsurface_tree, subsurface_destroy);
	wlr_scene_node_destroy(&subsurface_tree->tree->node);
}

static void subsurface_tree_handle_surface_map(struct wl_listener *listener,
		void *data) {
	struct wlr_scene_subsurface_tree_surface *subsurface_tree =
		wl_container_of(listener, subsurface_tree, surface_map);

	wlr_scene_node_set_enabled(&subsurface_tree->tree->node, true);
}

static void subsurface_tree_handle_surface_unmap(struct wl_listener *listener,
		void *data) {
	struct wlr_scene_subsurface_tree_surface *subsurface_tree =
		wl_container_of(listener, subsurface_tree, surface_unmap);

	wlr_scene_node_set_enabled(&subsurface_tree->tree->node, false);
}

static void subsurface_tree_surface_addon_destroy(struct wlr_addon *addon) {
	struct wlr_scene_subsurface_tree_surface *subsurface_tree =
		wl_container_of(addon, subsurface_tree, surface_addon);
	wlr_scene_node_destroy(&subsurface_tree->tree->node);
}

static const struct wlr_addon_interface subsurface_tree_surface_addon_impl = {
	.name = "wlr_scene_subsurface_tree_surface",
	.destroy = subsurface_tree_surface_addon_destroy,
};

static struct wlr_scene_subsurface_tree_surface *scene_surface_tree_create_surface(
	struct wlr_scene_subsurface_tree *tree,
	struct wlr_scene_tree *parent, struct wlr_surface *surface);

static bool subsurface_tree_create_subsurface(
		struct wlr_scene_subsurface_tree_surface *parent,
		struct wlr_subsurface *subsurface) {
	struct wlr_scene_subsurface_tree_surface *child = scene_surface_tree_create_surface(
		parent->subsurface_tree, parent->tree, subsurface->surface);
	if (child == NULL) {
		return false;
	}

	child->parent = parent;

	wlr_addon_init(&child->surface_addon, &subsurface->surface->addons,
		parent, &subsurface_tree_surface_addon_impl);

	child->subsurface_destroy.notify = subsurface_tree_handle_subsurface_destroy;
	wl_signal_add(&subsurface->events.destroy, &child->subsurface_destroy);

	return true;
}

static void subsurface_tree_handle_surface_new_subsurface(
		struct wl_listener *listener, void *data) {
	struct wlr_scene_subsurface_tree_surface *subsurface_tree =
		wl_container_of(listener, subsurface_tree, surface_new_subsurface);
	struct wlr_subsurface *subsurface = data;
	if (!subsurface_tree_create_subsurface(subsurface_tree, subsurface)) {
		wl_resource_post_no_memory(subsurface->resource);
	}
}

static struct wlr_scene_subsurface_tree_surface *scene_surface_tree_create_surface(
		struct wlr_scene_subsurface_tree *tree,
		struct wlr_scene_tree *parent, struct wlr_surface *surface) {
	struct wlr_scene_subsurface_tree_surface *subsurface_tree =
		calloc(1, sizeof(*subsurface_tree));
	if (subsurface_tree == NULL) {
		return NULL;
	}

	subsurface_tree->subsurface_tree = tree;
	subsurface_tree->tree = wlr_scene_tree_create(parent);
	if (subsurface_tree->tree == NULL) {
		goto error_surface_tree;
	}

	subsurface_tree->scene_surface =
		wlr_scene_surface_create(subsurface_tree->tree, surface);
	if (subsurface_tree->scene_surface == NULL) {
		goto error_scene_surface;
	}

	subsurface_tree->surface = surface;

	struct wlr_subsurface *subsurface;
	wl_list_for_each(subsurface, &surface->current.subsurfaces_below,
			current.link) {
		if (!subsurface_tree_create_subsurface(subsurface_tree, subsurface)) {
			goto error_scene_surface;
		}
	}
	wl_list_for_each(subsurface, &surface->current.subsurfaces_above,
			current.link) {
		if (!subsurface_tree_create_subsurface(subsurface_tree, subsurface)) {
			goto error_scene_surface;
		}
	}

	wl_list_insert(&tree->surfaces, &subsurface_tree->link);
	subsurface_tree_reconfigure(subsurface_tree);

	subsurface_tree->scene_destroy.notify = subsurface_tree_handle_scene_destroy;
	wl_signal_add(&subsurface_tree->tree->node.events.destroy, &subsurface_tree->scene_destroy);

	subsurface_tree->surface_destroy.notify = subsurface_tree_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &subsurface_tree->surface_destroy);

	subsurface_tree->surface_commit.notify = subsurface_tree_handle_surface_commit;
	wl_signal_add(&surface->events.commit, &subsurface_tree->surface_commit);

	subsurface_tree->surface_map.notify = subsurface_tree_handle_surface_map;
	wl_signal_add(&surface->events.map, &subsurface_tree->surface_map);

	subsurface_tree->surface_unmap.notify = subsurface_tree_handle_surface_unmap;
	wl_signal_add(&surface->events.unmap, &subsurface_tree->surface_unmap);

	subsurface_tree->surface_new_subsurface.notify =
		subsurface_tree_handle_surface_new_subsurface;
	wl_signal_add(&surface->events.new_subsurface,
		&subsurface_tree->surface_new_subsurface);

	wlr_scene_node_set_enabled(&subsurface_tree->tree->node, surface->mapped);

	return subsurface_tree;

error_scene_surface:
	wlr_scene_node_destroy(&subsurface_tree->tree->node);
error_surface_tree:
	free(subsurface_tree);
	return NULL;
}

struct wlr_scene_subsurface_tree *wlr_scene_subsurface_tree_create(
		struct wlr_scene_tree *parent, struct wlr_surface *surface) {
	struct wlr_scene_subsurface_tree *subsurface_tree = calloc(1, sizeof(*subsurface_tree));
	if (!subsurface_tree) {
		return NULL;
	}

	wl_list_init(&subsurface_tree->surfaces);

	struct wlr_scene_subsurface_tree_surface *subsurface_tree_surface =
		scene_surface_tree_create_surface(subsurface_tree, parent, surface);
	if (subsurface_tree_surface == NULL) {
		free(subsurface_tree);
		return NULL;
	}

	subsurface_tree->root = subsurface_tree_surface;
	subsurface_tree->tree = subsurface_tree_surface->tree;
	return subsurface_tree;
}

void wlr_scene_subsurface_tree_set_clip(struct wlr_scene_subsurface_tree *tree,
		const struct wlr_box *clip) {
	if (wlr_box_equal(&tree->root->clip, clip)) {
		return;
	}

	if (clip) {
		tree->root->clip = *clip;
	} else {
		tree->root->clip = (struct wlr_box){0};
	}

	// iterate in reverse because children of parent subsurface will appear
	// earlier in the list.
	struct wlr_scene_subsurface_tree_surface *surface;
	wl_list_for_each_reverse(surface, &tree->surfaces, link) {
		subsurface_tree_reconfigure_clip(surface);
	}
}
