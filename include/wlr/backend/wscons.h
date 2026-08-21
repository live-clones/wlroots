/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_BACKEND_WSCONS_H
#define WLR_BACKEND_WSCONS_H

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>

struct wlr_input_device;

struct wlr_backend *wlr_wscons_backend_create(struct wlr_session *session);

bool wlr_backend_is_wscons(const struct wlr_backend *backend);
bool wlr_input_device_is_wscons(struct wlr_input_device *device);

#endif
