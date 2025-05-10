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

#include <wlr/render/color.h>

struct wlr_surface;

// State stored per-surface, which is sync'ed to surface commit
struct wlr_color_representation_v1_state {
	enum wlr_color_encoding coefficients;
	enum wlr_color_range range;
	enum wlr_color_chroma_location chroma_location;
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
