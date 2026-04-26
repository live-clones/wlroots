#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_SCENE_NODE_H
#define WLR_TYPES_WLR_SCENE_NODE_H

#include <pixman.h>
#include <wayland-server-core.h>
#include <wlr/util/addon.h>
#include <wlr/util/box.h>
#include <wlr/config.h>

#include <time.h>

#if WLR_HAS_XWAYLAND
#include <wlr/xwayland/xwayland.h>
#endif

struct wlr_scene_node;
struct wlr_scene_output;
struct wlr_render_list_entry;
struct wlr_render_data;
struct wlr_scene;
struct wlr_scene_buffer;
struct wlr_linux_dmabuf_feedback_v1_init_options;

typedef bool (*scene_node_box_iterator_func_t)(struct wlr_scene_node *node,
	int sx, int sy, void *data);

struct wlr_scene_update_data {
	pixman_region32_t *visible;
	const pixman_region32_t *update_region;
	struct wlr_box update_box;
	struct wl_list *outputs;
	bool calculate_visibility;
	bool restack_xwayland_surfaces;

#if WLR_HAS_XWAYLAND
	struct wlr_xwayland_surface *restack_above;
#endif
};

struct wlr_render_data {
	enum wl_output_transform transform;
	float scale;
	struct wlr_box logical;
	int trans_width, trans_height;

	struct wlr_scene_output *output;

	struct wlr_render_pass *render_pass;
	pixman_region32_t damage;
};

struct wlr_render_list_constructor_data {
	struct wlr_box box;
	struct wl_array *render_list;
	bool calculate_visibility;
	bool highlight_transparent_region;
	bool fractional_scale;
};

struct wlr_render_list_entry {
	struct wlr_scene_node *node;
	bool highlight_transparent_region;
	int x, y;
};

struct wlr_scene_node_impl {
	void (*destroy)(struct wlr_scene_node *node);
	void (*set_enabled)(struct wlr_scene_node *node, bool enabled);
	void (*set_position)(struct wlr_scene_node *node, int x, int y);
	void (*bounds)(struct wlr_scene_node *node,
		int x, int y, pixman_region32_t *visible);
	void (*get_size)(struct wlr_scene_node *node,
		int *width, int *height);
	bool (*coords)(struct wlr_scene_node *node, int *lx_ptr, int *ly_ptr);
	struct wlr_scene_node *(*at)(struct wlr_scene_node *node,
		double lx, double ly, double *nx, double *ny);
	bool (*in_box)(struct wlr_scene_node *node, struct wlr_box *box,
		scene_node_box_iterator_func_t iterator, void *user_data);
	void (*opaque_region)(struct wlr_scene_node *node, int x, int y,
		pixman_region32_t *opaque);
	void (*update_outputs)(struct wlr_scene_node *node,
		struct wl_list *outputs, struct wlr_scene_output *ignore,
		struct wlr_scene_output *force);
	void (*update)(struct wlr_scene_node *node,
		pixman_region32_t *damage);
	void (*visibility)(struct wlr_scene_node *node,
		pixman_region32_t *visible);
	void (*frame_done)(struct wlr_scene_node *node,
		struct wlr_scene_output *scene_output, struct timespec *now);
	bool (*invisible)(struct wlr_scene_node *node);
	bool (*construct_render_list_iterator)(struct wlr_scene_node *node,
		int lx, int ly, void *_data);
	void (*render)(struct wlr_render_list_entry *entry, const struct wlr_render_data *data);
	void (*dmabuf_feedback)(struct wlr_render_list_entry *entry,
		struct wlr_scene_output *scene_output);
	void (*get_extents)(struct wlr_scene_node *node, int lx, int ly,
		int *x_min, int *y_min, int *x_max, int *y_max);
	struct wl_list *(*get_children)(struct wlr_scene_node *node);
	void (*restack_xwayland_surface)(struct wlr_scene_node *node,
		struct wlr_box *box, struct wlr_scene_update_data *data);
	void (*cleanup_when_disabled)(struct wlr_scene_node *node,
		bool xwayland_restack, struct wl_list *outputs);
};

struct wlr_scene_node {
	const struct wlr_scene_node_impl *impl;

	struct wlr_scene_tree *parent;
	struct wlr_scene *scene;

	struct wl_list link; // wlr_scene_tree.children

	bool enabled;
	int x, y; // relative to parent

	struct {
		struct wl_signal destroy;
	} events;

	void *data;

	struct wlr_addon_set addons;

	struct {
		pixman_region32_t visible;
	} WLR_PRIVATE;

	struct wl_list children; // wlr_scene_node.link
};

void wlr_scene_node_init(struct wlr_scene_node *node,
	const struct wlr_scene_node_impl *impl, struct wlr_scene_tree *parent);
void wlr_scene_node_destroy(struct wlr_scene_node *node);

void wlr_scene_node_set_enabled(struct wlr_scene_node *node, bool enabled);
void wlr_scene_node_set_position(struct wlr_scene_node *node, int x, int y);
void wlr_scene_node_place_above(struct wlr_scene_node *node,
	struct wlr_scene_node *sibling);
void wlr_scene_node_place_below(struct wlr_scene_node *node,
	struct wlr_scene_node *sibling);
void wlr_scene_node_raise_to_top(struct wlr_scene_node *node);
void wlr_scene_node_lower_to_bottom(struct wlr_scene_node *node);
void wlr_scene_node_reparent(struct wlr_scene_node *node,
	struct wlr_scene_tree *new_parent);
void wlr_scene_node_bounds(struct wlr_scene_node *node,
	int x, int y, pixman_region32_t *visible);
void wlr_scene_node_get_size(struct wlr_scene_node *node,
	int *width, int *height);
bool wlr_scene_node_coords(struct wlr_scene_node *node, int *lx_ptr, int *ly_ptr);
struct wlr_scene_node *wlr_scene_node_at(struct wlr_scene_node *node,
	double lx, double ly, double *nx, double *ny);
bool wlr_scene_node_nodes_in_box(struct wlr_scene_node *node, struct wlr_box *box,
	scene_node_box_iterator_func_t iterator, void *user_data);
void wlr_scene_node_opaque_region(struct wlr_scene_node *node, int x, int y,
	pixman_region32_t *opaque);
void wlr_scene_node_update_outputs(struct wlr_scene_node *node,
	struct wl_list *outputs, struct wlr_scene_output *ignore,
	struct wlr_scene_output *force);
void wlr_scene_node_update(struct wlr_scene_node *node,
	pixman_region32_t *damage);
void wlr_scene_node_visibility(struct wlr_scene_node *node,
	pixman_region32_t *visible);
void wlr_scene_node_send_frame_done(struct wlr_scene_node *node,
	struct wlr_scene_output *scene_output, struct timespec *now);
bool wlr_scene_node_invisible(struct wlr_scene_node *node);
bool wlr_scene_node_construct_render_list_iterator(struct wlr_scene_node *node,
	int lx, int ly, void *_data);
void wlr_scene_node_render(struct wlr_render_list_entry *entry, const struct wlr_render_data *data);
void wlr_scene_node_dmabuf_feedback(struct wlr_render_list_entry *entry,
	struct wlr_scene_output *scene_output);
void wlr_scene_node_restack_xwayland_surface_below(struct wlr_scene_node *node);
void wlr_scene_node_get_extents(struct wlr_scene_node *node, int lx, int ly,
	int *x_min, int *y_min, int *x_max, int *y_max);
struct wl_list *wlr_scene_node_get_children(struct wlr_scene_node *node);
void wlr_scene_node_restack_xwayland_surface(struct wlr_scene_node *node,
	struct wlr_box *box, struct wlr_scene_update_data *data);
void wlr_scene_node_cleanup_when_disabled(struct wlr_scene_node *node,
	bool xwayland_restack, struct wl_list *outputs);

#endif
