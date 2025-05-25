#ifndef TYPES_WLR_SCENE_H
#define TYPES_WLR_SCENE_H

#include <wlr/types/wlr_scene.h>

struct wlr_scene *scene_node_get_root(struct wlr_scene_node *node);

void scene_node_get_size(struct wlr_scene_node *node, int *width, int *height);

void scene_node_opaque_region(struct wlr_scene_node *node, int x, int y, pixman_region32_t *opaque);

void scene_surface_set_clip(struct wlr_scene_surface *surface, struct wlr_box *clip);

void scale_region(pixman_region32_t *region, float scale, bool round_up);

void output_to_buffer_coords(pixman_region32_t *damage, struct wlr_output *output);

void scene_output_damage(struct wlr_scene_output *scene_output, const pixman_region32_t *damage);

void scene_buffer_set_texture(struct wlr_scene_buffer *scene_buffer, struct wlr_texture *texture);

void update_node_update_outputs(struct wlr_scene_node *node,
	struct wl_list *outputs, struct wlr_scene_output *ignore,
	struct wlr_scene_output *force);

typedef bool (*scene_node_box_iterator_func_t)(struct wlr_scene_node *node,
	int sx, int sy, void *data);

bool scene_nodes_in_box(struct wlr_scene_node *node, struct wlr_box *box,
	scene_node_box_iterator_func_t iterator, void *user_data);

#endif
