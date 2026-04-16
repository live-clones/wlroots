#ifndef WLR_TYPES_WLR_XDG_CUTOUTS_V1
#define WLR_TYPES_WLR_XDG_CUTOUTS_V1

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>

enum wlr_cutouts_type {
  WLR_CUTOUTS_TYPE_CUTOUT = 0,
  WLR_CUTOUTS_TYPE_NOTCH = 1 << 0,
  WLR_CUTOUTS_TYPE_WATERFALL = 1 << 1,
};

struct wlr_xdg_cutouts_manager_v1 {
	struct wl_global *global;
	struct wl_list cutouts; // wlr_xdg_cutouts_v1.link

	struct {
		struct wl_signal new_cutouts; // struct wlr_xdg_cutouts
		struct wl_signal destroy;
	} events;

	void *data;

	struct {
		uint32_t next_id;
		struct wl_listener display_destroy;
	} WLR_PRIVATE;
};

struct wlr_xdg_cutouts_v1_configure {
	struct wl_list link; // wlr_xdg_cutouts.configure_list
	struct wlr_surface_configure *surface_configure;
	// Ids valid in this configure sequence
	struct wl_array valid_ids;
};

struct wlr_xdg_cutouts_v1_state {
	struct wl_array unhandled;
};

/**
 * Cutouts interface.
 *
 * Emits `send_coutouts` when cutout information should be sent for the associated toplevel.
 * Emits `unhandled_updated` when the list of unhandled ids got updated.
 */
struct wlr_xdg_cutouts_v1 {
	struct wl_resource *resource;
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_xdg_cutouts_manager_v1 *manager;
	struct wl_list link; // wlr_xdg_cutouts_manager_v1.link

	struct wlr_xdg_cutouts_v1_state current, pending;

	struct {
		struct wl_signal destroy;
		struct wl_signal send_cutouts;
		struct wl_signal unhandled_updated;
	} events;

	void *data;

	struct {
		// ids sent in the current configure sequence
		struct wl_list configure_list; // wlr_xdg_cutouts_v1_configure.link
		struct wl_array sent_ids; // uint32_t
		struct wl_listener toplevel_destroy;
		struct wl_listener surface_configure;
		struct wl_listener surface_ack_configure;
	} WLR_PRIVATE;
};

struct wlr_xdg_cutouts_manager_v1 *wlr_xdg_cutouts_manager_v1_create(struct wl_display *display);
void wlr_xdg_cutouts_v1_send_cutout(struct wlr_xdg_cutouts_v1 *cutouts, const struct wlr_box *box,
	enum wlr_cutouts_type type, uint32_t id);
void wlr_xdg_cutouts_v1_send_corner(struct wlr_xdg_cutouts_v1 *cutouts,	enum wlr_edges position,
	uint32_t radius, uint32_t id);
void wlr_xdg_cutouts_v1_send_cutouts_done(struct wlr_xdg_cutouts_v1 *cutouts);

#endif
