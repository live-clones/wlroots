/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_BACKEND_MULTI_H
#define WLR_BACKEND_MULTI_H

#include <wlr/backend.h>

struct wlr_multi_backend;

/**
 * Creates a multi-backend. Multi-backends wrap an arbitrary number of backends
 * and aggregate their new_output/new_input signals.
 */
struct wlr_multi_backend *wlr_multi_backend_create(struct wl_display *display);

/**
 * Adds the given backend to the multi backend. This should be done before the
 * new backend is started.
 */
bool wlr_multi_backend_add(struct wlr_multi_backend *multi,
	struct wlr_backend *backend);

void wlr_multi_backend_remove(struct wlr_multi_backend *multi,
	struct wlr_backend *backend);

bool wlr_multi_backend_is_empty(struct wlr_multi_backend *backend);

void wlr_multi_backend_for_each(struct wlr_multi_backend *backend,
		void (*callback)(struct wlr_backend *backend, void *data), void *data);

struct wlr_multi_backend *wlr_multi_backend_try_from(struct wlr_backend *backend);

struct wlr_backend *wlr_multi_backend_base(struct wlr_multi_backend *backend);

#endif
