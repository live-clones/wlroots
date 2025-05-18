#ifndef TYPES_WLR_FOREIGN_TOPLEVEL_H
#define TYPES_WLR_FOREIGN_TOPLEVEL_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>

void foreign_toplevel_send_details_to_resource(
	struct wlr_ext_foreign_toplevel_handle_v1 *toplevel, struct wl_resource *resource);

#endif
