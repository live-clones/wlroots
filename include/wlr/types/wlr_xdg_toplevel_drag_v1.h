/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_XDG_TOPLEVEL_DRAG_V1_H
#define WLR_TYPES_WLR_XDG_TOPLEVEL_DRAG_V1_H

#include <wayland-server-core.h>

struct wlr_xdg_toplevel;
struct wlr_data_source;

struct wlr_xdg_toplevel_drag_v1 {
	struct wl_resource *resource;
	struct wlr_xdg_toplevel_drag_manager_v1 *manager;
	struct wlr_data_source *data_source;

	struct wlr_xdg_toplevel *toplevel; // NULL if none attached
	int32_t x_offset, y_offset;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct wl_list link; // wlr_xdg_toplevel_drag_manager_v1.drags
		bool started, ended;

		struct wl_listener data_source_destroy;
		struct wl_listener toplevel_unmap;
		struct wl_listener toplevel_surface_destroy;
	} WLR_PRIVATE;
};

struct wlr_xdg_toplevel_drag_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_signal destroy;
		struct wl_signal new_toplevel_drag; // struct wlr_xdg_toplevel_drag_v1
	} events;

	struct {
		struct wl_list drags; // wlr_xdg_toplevel_drag_v1.link
		struct wl_listener display_destroy;
	} WLR_PRIVATE;
};

struct wlr_xdg_toplevel_drag_manager_v1 *wlr_xdg_toplevel_drag_manager_v1_create(
		struct wl_display *display, uint32_t version);

/**
 * Get a struct wlr_xdg_toplevel_drag_v1 from a struct wlr_data_source.
 *
 * Returns NULL if there's no xdg_toplevel_drag_v1 associated with the data source.
 */
struct wlr_xdg_toplevel_drag_v1 *wlr_xdg_toplevel_drag_v1_from_wlr_data_source(
		struct wlr_xdg_toplevel_drag_manager_v1 *manager,
		struct wlr_data_source *source);

/**
 * Get a struct wlr_xdg_toplevel_drag_v1 from a struct wlr_xdg_toplevel.
 *
 * Returns NULL if there's no xdg_toplevel_drag_v1 with the given toplevel attached.
 */
struct wlr_xdg_toplevel_drag_v1 *wlr_xdg_toplevel_drag_v1_from_wlr_xdg_toplevel(
		struct wlr_xdg_toplevel_drag_manager_v1 *manager,
		struct wlr_xdg_toplevel *toplevel);

/**
 * Notify the xdg_toplevel_drag that the underlying drag has started.
 *
 * This should be called by the compositor when wlr_seat starts a drag
 * that has an associated xdg_toplevel_drag.
 */
void wlr_xdg_toplevel_drag_v1_start(struct wlr_xdg_toplevel_drag_v1 *drag);

/**
 * Notify the xdg_toplevel_drag that the underlying drag has ended.
 *
 * This should be called by the compositor when the wlr_drag is destroyed
 * (i.e., after drop or cancel). After this call, the client may safely
 * destroy the xdg_toplevel_drag resource.
 */
void wlr_xdg_toplevel_drag_v1_finish(struct wlr_xdg_toplevel_drag_v1 *drag);

#endif
