/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_FOREIGN_TOPLEVEL_REQUEST_V1_H
#define WLR_TYPES_WLR_FOREIGN_TOPLEVEL_REQUEST_V1_H

#include <wayland-server-core.h>

struct wlr_ext_foreign_toplevel_request_manager_v1 {
	struct wl_global *global;
	struct wl_list resources; // wl_resource_get_link()

	struct {
		struct wl_signal request;
		struct wl_signal destroy;
	} events;

	void *data;

	struct {
		struct wl_listener display_destroy;
	} WLR_PRIVATE;
};

struct wlr_ext_foreign_toplevel_request_v1 {
	struct wl_resource *manager;
	struct wl_resource *resource;

	struct {
		struct wl_signal destroy;
	} events;
};

struct wlr_ext_foreign_toplevel_request_source_v1 {
	struct wl_global *global;
	struct wl_list resources; // wl_resource_get_link()

	struct {
		struct wl_signal toplevel;
		struct wl_signal cancel;
		struct wl_signal destroy;
	} events;

	struct {
		struct wl_listener display_destroy;
	} WLR_PRIVATE;
};

struct wlr_ext_foreign_toplevel_request_pending_v1 {
	struct wlr_ext_foreign_toplevel_request_source_v1 *source;
	struct wlr_ext_foreign_toplevel_request_v1 *request;
	struct wlr_ext_foreign_toplevel_handle_v1 *handle;
	struct wl_resource *resource;
};

struct wlr_ext_foreign_toplevel_request_manager_v1 *
	wlr_ext_foreign_toplevel_request_manager_v1_create(struct wl_display *display, uint32_t version);

void wlr_ext_foreign_toplevel_request_v1_send_toplevel(
	struct wlr_ext_foreign_toplevel_request_v1 *request, struct wlr_ext_foreign_toplevel_handle_v1 *toplevel);
void wlr_ext_foreign_toplevel_request_v1_cancel(struct wlr_ext_foreign_toplevel_request_v1 *request);

struct wlr_ext_foreign_toplevel_request_source_v1 *
	wlr_ext_foreign_toplevel_request_source_v1_create(struct wl_display *display, uint32_t version);

void wlr_ext_foreign_toplevel_request_source_v1_request(
	struct wlr_ext_foreign_toplevel_request_source_v1 *source, struct wlr_ext_foreign_toplevel_request_v1 *request);
#endif
