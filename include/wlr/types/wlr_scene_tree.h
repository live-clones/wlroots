#ifndef WLR_TYPES_WLR_SCENE_TREE_H
#define WLR_TYPES_WLR_SCENE_TREE_H

#include <stdbool.h>

#include <wlr/types/wlr_scene_node.h>

#include <wayland-server-core.h>

struct wlr_scene;

struct wlr_scene_tree {
	struct wlr_scene_node node;

	struct wl_list children; // wlr_scene_node.link
};

/**
 * Add a node displaying nothing but its children.
 */
struct wlr_scene_tree *wlr_scene_tree_create(struct wlr_scene_tree *parent);
struct wlr_scene_tree *wlr_root_scene_tree_create(struct wlr_scene *scene);

bool wlr_scene_node_is_tree(const struct wlr_scene_node *node);

/**
 * If this node represents a wlr_scene_tree, that tree will be returned. It
 * is not legal to feed a node that does not represent a wlr_scene_tree.
 */
struct wlr_scene_tree *wlr_scene_tree_from_node(struct wlr_scene_node *node);

#endif // WLR_TYPES_WLR_SCENE_TREE_H
