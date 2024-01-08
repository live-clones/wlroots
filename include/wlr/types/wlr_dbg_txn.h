/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_DBG_TXN_H
#define WLR_TYPES_WLR_DBG_TXN_H

#include <wayland-server-core.h>
#include "dbg-txn-protocol.h"

struct wlr_dbg_txn_manager {
	struct wl_global *global;

	struct wl_listener display_destroy;
};

struct wlr_dbg_txn_manager *wlr_dbg_txn_manager_create(struct wl_display *display);

#endif
