/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_OUTPUT_MANAGER_H
#define WLR_TYPES_OUTPUT_MANAGER_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/render/drm_format_set.h>

struct wlr_renderer;
struct wlr_allocator;
struct wlr_backend;
struct wlr_output;

struct wlr_output_manager_backend {
	struct wlr_output_manager *manager;

	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_backend *backend;

	struct wl_list link; // wlr_output_manager.backends

	struct {
		struct wl_signal recovery;
	} events;

	// private state

	uint32_t locks;
	struct wl_listener backend_destroy;
	struct wl_listener renderer_lost;
};

struct wlr_output_manager {
	struct wl_list backends; // wlr_output_manager_backend.link

	struct wlr_output_manager_backend primary;
};

/**
 * Initializes the output given output manager. wlr_output_manager_finish
 * must be called to clean up this manager.
 */
bool wlr_output_manager_init(struct wlr_output_manager *manager,
	struct wlr_backend *backend);

/**
 * Finishes this output_manager and cleans up all its resources including any
 * output manager backends.
 */
void wlr_output_manager_finish(struct wlr_output_manager *manager);

/**
 * This will return a output_manager backend that will be reference counted.
 * wlr_output_manager_unlock_backend is required to be called after the usage
 * of this is finished.
 */
struct wlr_output_manager_backend *wlr_output_manager_lock_backend(
	struct wlr_output_manager *manager, struct wlr_backend *wlr_backend);

/**
 * wlr_output_manager_unlock_backend will unlock any backend returned by
 * wlr_output_manager_lock_rendener. The allocator and backend allocated
 * may be destroyed when the reference count reaches 0
 */
void wlr_output_manager_unlock_backend(struct wlr_output_manager_backend *backend);

/**
 * wlr_output_manager_init_output will automatically initialize the given output.
 * This is a helder function that will handle unlocking backends automatically
 * upon output destroy
 */
bool wlr_output_manager_init_output(struct wlr_output_manager *manager,
	struct wlr_output *output);

/**
 * Initializes shm for the given wl_display given the constraints all devices
 * on the manager have
 */
bool wlr_output_manager_init_wl_shm(struct wlr_output_manager *manager,
	struct wl_display *wl_display);

/**
 * Initializes the given wl_display given the constraints all devices
 * on the manager have
 */
bool wlr_output_manager_init_wl_display(struct wlr_output_manager *manager,
	struct wl_display *wl_display);

#endif
