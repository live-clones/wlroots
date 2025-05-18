#ifndef TYPES_WLR_FOREIGN_TOPLEVEL_H
#define TYPES_WLR_FOREIGN_TOPLEVEL_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>

struct wl_resource *foreign_toplevel_create_resource_for_client(
	struct wlr_ext_foreign_toplevel_handle_v1 *toplevel, struct wl_client *client);
void foreign_toplevel_send_details_to_resource(
	struct wlr_ext_foreign_toplevel_handle_v1 *toplevel, struct wl_resource *resource);

#endif
