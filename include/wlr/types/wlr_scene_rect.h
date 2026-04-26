#ifndef WLR_TYPES_WLR_SCENE_RECT_H
#define WLR_TYPES_WLR_SCENE_RECT_H

#include <wlr/types/wlr_scene_node.h>

struct wlr_scene_rect {
	struct wlr_scene_node node;
	int width, height;
	float color[4];
};

/**
 * Add a node displaying a solid-colored rectangle to the scene-graph.
 *
 * The color argument must be a premultiplied color value.
 */
struct wlr_scene_rect *wlr_scene_rect_create(struct wlr_scene_tree *parent,
	int width, int height, const float color[static 4]);

/**
 * Change the width and height of an existing rectangle node.
 */
void wlr_scene_rect_set_size(struct wlr_scene_rect *rect, int width, int height);

/**
 * Change the color of an existing rectangle node.
 *
 * The color argument must be a premultiplied color value.
 */
void wlr_scene_rect_set_color(struct wlr_scene_rect *rect, const float color[static 4]);

bool wlr_scene_node_is_rect(const struct wlr_scene_node *node);

/**
 * If this node represents a wlr_scene_rect, that rect will be returned. It
 * is not legal to feed a node that does not represent a wlr_scene_rect.
 */
struct wlr_scene_rect *wlr_scene_rect_from_node(struct wlr_scene_node *node);

#endif
