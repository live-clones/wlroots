/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_COLOR_REPRESENTATION_V1_H
#define WLR_TYPES_WLR_COLOR_REPRESENTATION_V1_H

#include <wayland-server-core.h>

struct wlr_surface;

enum wlr_color_repr_encoding {
	WLR_COLOR_REPR_ENCODING_UNSET,
	WLR_COLOR_REPR_ENCODING_BT601,
	WLR_COLOR_REPR_ENCODING_BT709,
	WLR_COLOR_REPR_ENCODING_BT2020,
	WLR_COLOR_REPR_ENCODING_UNKNOWN,
};
enum wlr_color_repr_range {
	WLR_COLOR_REPR_RANGE_UNSET,
	WLR_COLOR_REPR_RANGE_LIMITED,
	WLR_COLOR_REPR_RANGE_FULL,
	WLR_COLOR_REPR_RANGE_UNKNOWN,
};
enum wlr_color_repr_chroma_loc {
	WLR_COLOR_REPR_CHROMA_LOC_UNSET,
	WLR_COLOR_REPR_CHROMA_LOC_TYPE0,
	WLR_COLOR_REPR_CHROMA_LOC_TYPE1,
	WLR_COLOR_REPR_CHROMA_LOC_TYPE2,
	WLR_COLOR_REPR_CHROMA_LOC_TYPE3,
	WLR_COLOR_REPR_CHROMA_LOC_UNKNOWN,
};

enum wlr_color_repr_encoding wlr_color_repr_encoding_from_wp(int wp_val);
enum wlr_color_repr_range wlr_color_repr_range_from_wp(int wp_val);
enum wlr_color_repr_chroma_loc wlr_color_repr_chroma_loc_from_wp(int wp_val);

// State stored per-surface, which is sync'ed to surface commit
struct wlr_color_representation_v1_state {
	enum wlr_color_repr_encoding coefficients;
	enum wlr_color_repr_range range;
	enum wlr_color_repr_chroma_loc chroma_location;
};

struct wlr_color_representation_manager_v1 {
	struct wl_global *global;

	struct {
		// Create a new wlr_color_representation_v1 for a surface
		struct wl_signal create;

		// Destroy the manager
		struct wl_signal destroy;
	} events;

	struct wl_listener display_destroy;
};

struct wlr_color_representation_manager_v1 *wlr_color_representation_manager_v1_create(
		struct wl_display *display, uint32_t version);

const struct wlr_color_representation_v1_state *wlr_color_representation_v1_get_surface_state(
	struct wlr_surface *surface);

#endif // WLR_TYPES_WLR_COLOR_REPRESENTATION_V1_H
