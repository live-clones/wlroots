#ifndef WLR_TYPES_WLR_SCENE_BUFFER_H
#define WLR_TYPES_WLR_SCENE_BUFFER_H

#include <wlr/types/wlr_scene_node.h>
#include <wayland-server-core.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/util/addon.h>
#include <wlr/util/box.h>

struct wlr_scene_buffer;

typedef bool (*wlr_scene_buffer_point_accepts_input_func_t)(
    struct wlr_scene_buffer *buffer, double *sx, double *sy);
typedef void (*wlr_scene_buffer_iterator_func_t)(
	struct wlr_scene_buffer *buffer, int sx, int sy, void *user_data);

/** A scene-graph node displaying a buffer */
struct wlr_scene_buffer {
    struct wlr_scene_node node;

	// May be NULL
	struct wlr_buffer *buffer;

	struct {
		struct wl_signal outputs_update; // struct wlr_scene_outputs_update_event
		struct wl_signal output_enter; // struct wlr_scene_output
		struct wl_signal output_leave; // struct wlr_scene_output
		struct wl_signal output_sample; // struct wlr_scene_output_sample_event
		struct wl_signal frame_done; // struct timespec
	} events;

	// May be NULL
	wlr_scene_buffer_point_accepts_input_func_t point_accepts_input;

	/**
	 * The output that the largest area of this buffer is displayed on.
	 * This may be NULL if the buffer is not currently displayed on any
	 * outputs. This is the output that should be used for frame callbacks,
	 * presentation feedback, etc.
	 */
	struct wlr_scene_output *primary_output;

	float opacity;
	enum wlr_scale_filter_mode filter_mode;
	struct wlr_fbox src_box;
	int dst_width, dst_height;
	enum wl_output_transform transform;
	pixman_region32_t opaque_region;
	enum wlr_color_transfer_function transfer_function;
	enum wlr_color_named_primaries primaries;
	enum wlr_color_encoding color_encoding;
	enum wlr_color_range color_range;


	struct {
		uint64_t active_outputs;
		struct wlr_texture *texture;
		struct wlr_linux_dmabuf_feedback_v1_init_options prev_feedback_options;

		bool own_buffer;
		int buffer_width, buffer_height;
		bool buffer_is_opaque;

		struct wlr_drm_syncobj_timeline *wait_timeline;
		uint64_t wait_point;

		struct wl_listener buffer_release;
		struct wl_listener renderer_destroy;

		// True if the underlying buffer is a wlr_single_pixel_buffer_v1
		bool is_single_pixel_buffer;
		// If is_single_pixel_buffer is set, contains the color of the buffer
		// as {R, G, B, A} where the max value of each component is UINT32_MAX
		uint32_t single_pixel_buffer_color[4];
	} WLR_PRIVATE;
};

/**
 * Add a node displaying a buffer to the scene-graph.
 *
 * If the buffer is NULL, this node will not be displayed.
 */
struct wlr_scene_buffer *wlr_scene_buffer_create(struct wlr_scene_tree *parent,
	struct wlr_buffer *buffer);

/**
 * Sets the buffer's backing buffer.
 *
 * If the buffer is NULL, the buffer node will not be displayed.
 */
void wlr_scene_buffer_set_buffer(struct wlr_scene_buffer *scene_buffer,
	struct wlr_buffer *buffer);

/**
 * Sets the buffer's backing buffer with a custom damage region.
 *
 * The damage region is in buffer-local coordinates. If the region is NULL,
 * the whole buffer node will be damaged.
 */
void wlr_scene_buffer_set_buffer_with_damage(struct wlr_scene_buffer *scene_buffer,
	struct wlr_buffer *buffer, const pixman_region32_t *region);

/**
 * Options for wlr_scene_buffer_set_buffer_with_options().
 */
struct wlr_scene_buffer_set_buffer_options {
	// The damage region is in buffer-local coordinates. If the region is NULL,
	// the whole buffer node will be damaged.
	const pixman_region32_t *damage;

	// Wait for a timeline synchronization point before reading from the buffer.
	struct wlr_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
};

/**
 * Sets the buffer's backing buffer.
 *
 * If the buffer is NULL, the buffer node will not be displayed. If options is
 * NULL, empty options are used.
 */
void wlr_scene_buffer_set_buffer_with_options(struct wlr_scene_buffer *scene_buffer,
	struct wlr_buffer *buffer, const struct wlr_scene_buffer_set_buffer_options *options);

/**
 * Sets the buffer's opaque region. This is an optimization hint used to
 * determine if buffers which reside under this one need to be rendered or not.
 */
void wlr_scene_buffer_set_opaque_region(struct wlr_scene_buffer *scene_buffer,
	const pixman_region32_t *region);

/**
 * Set the source rectangle describing the region of the buffer which will be
 * sampled to render this node. This allows cropping the buffer.
 *
 * If NULL, the whole buffer is sampled. By default, the source box is NULL.
 */
void wlr_scene_buffer_set_source_box(struct wlr_scene_buffer *scene_buffer,
	const struct wlr_fbox *box);

/**
 * Set the destination size describing the region of the scene-graph the buffer
 * will be painted onto. This allows scaling the buffer.
 *
 * If zero, the destination size will be the buffer size. By default, the
 * destination size is zero.
 */
void wlr_scene_buffer_set_dest_size(struct wlr_scene_buffer *scene_buffer,
	int width, int height);

/**
 * Set a transform which will be applied to the buffer.
 */
void wlr_scene_buffer_set_transform(struct wlr_scene_buffer *scene_buffer,
	enum wl_output_transform transform);

/**
* Sets the opacity of this buffer
*/
void wlr_scene_buffer_set_opacity(struct wlr_scene_buffer *scene_buffer,
	float opacity);

/**
* Sets the filter mode to use when scaling the buffer
*/
void wlr_scene_buffer_set_filter_mode(struct wlr_scene_buffer *scene_buffer,
	enum wlr_scale_filter_mode filter_mode);

struct wlr_scene_frame_done_event {
	struct wlr_scene_output *output;
	struct timespec when;
};

/**
 * Calls the buffer's frame_done signal.
 */
void wlr_scene_buffer_send_frame_done(struct wlr_scene_buffer *scene_buffer,
	struct wlr_scene_frame_done_event *event);

bool wlr_scene_node_is_buffer(const struct wlr_scene_node *node);

/**
 * If this node represents a wlr_scene_buffer, that buffer will be returned. It
 * is not legal to feed a node that does not represent a wlr_scene_buffer.
 */
struct wlr_scene_buffer *wlr_scene_buffer_from_node(struct wlr_scene_node *node);

void wlr_scene_buffer_set_transfer_function(struct wlr_scene_buffer *scene_buffer,
	enum wlr_color_transfer_function transfer_function);

void wlr_scene_buffer_set_primaries(struct wlr_scene_buffer *scene_buffer,
	enum wlr_color_named_primaries primaries);

void wlr_scene_buffer_set_color_encoding(struct wlr_scene_buffer *scene_buffer,
	enum wlr_color_encoding encoding);

void wlr_scene_buffer_set_color_range(struct wlr_scene_buffer *scene_buffer,
	enum wlr_color_range range);

/**
 * Call `iterator` on each buffer in the scene-graph, with the buffer's
 * position in layout coordinates. The function is called from root to leaves
 * (in rendering order).
 */
void wlr_scene_node_for_each_buffer(struct wlr_scene_node *node,
	wlr_scene_buffer_iterator_func_t iterator, void *user_data);
#endif // WLR_TYPES_WLR_SCENE_BUFFER_H
