/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_POINTER_CONSTRAINTS_V1_H
#define WLR_TYPES_WLR_POINTER_CONSTRAINTS_V1_H

#include <stdint.h>
#include <wayland-server-core.h>
#include <pixman.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/types/wlr_seat.h>
#include "pointer-constraints-unstable-v1-protocol.h"

struct wlr_seat;

enum wlr_pointer_constraint_v1_type {
	WLR_POINTER_CONSTRAINT_V1_LOCKED,
	WLR_POINTER_CONSTRAINT_V1_CONFINED,
};

enum wlr_pointer_constraint_v1_state_field {
	WLR_POINTER_CONSTRAINT_V1_STATE_REGION = 1 << 0,
	WLR_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT = 1 << 1,
};

struct wlr_pointer_constraint_v1_state {
	uint32_t committed; // enum wlr_pointer_constraint_v1_state_field
	pixman_region32_t region;

	// only valid for locked_pointer
	struct {
		bool enabled;
		double x, y;
	} cursor_hint;
};

struct wlr_pointer_constraint_v1 {
	struct wlr_pointer_constraints_v1 *pointer_constraints;

	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wlr_seat *seat;
	enum zwp_pointer_constraints_v1_lifetime lifetime;
	enum wlr_pointer_constraint_v1_type type;
	pixman_region32_t region;

	struct wlr_pointer_constraint_v1_state current, pending;

	struct wl_list link; // wlr_pointer_constraints_v1.constraints

	struct {
		/**
		 * Emitted when a pointer constraint's region is updated.
		 */
		struct wl_signal set_region;
		struct wl_signal destroy;
	} events;

	void *data;

	struct {
		struct wl_listener surface_destroy;
		struct wl_listener seat_destroy;

		struct wlr_surface_synced synced;

		bool destroying;
	} WLR_PRIVATE;
};

struct wlr_pointer_constraints_v1 {
	struct wl_global *global;
	struct wl_list constraints; // wlr_pointer_constraint_v1.link

	struct {
		struct wl_signal destroy;
		struct wl_signal new_constraint; // struct wlr_pointer_constraint_v1
	} events;

	void *data;

	struct {
		struct wl_listener display_destroy;
	} WLR_PRIVATE;
};

/**
 * A zwp_pointer_constraints_v1 input router layer which modifiers pointer
 * position based on the constraint of an active surface.
 */
struct wlr_pointer_constraints_v1_input_router_layer {
	struct wlr_input_router *router;
	struct wlr_pointer_constraints_v1 *constraints;
	struct wlr_seat *seat;

	struct {
		struct wl_signal destroy;

		/**
		 * Emitted when a pointer lock with a cursor hint is unlocked. The
		 * compositor should then warp the pointer to the specified position.
		 */
		struct wl_signal cursor_hint;
	} events;

	struct {
		struct wlr_input_router_pointer pointer;

		struct wlr_surface *active_surface;
		struct wlr_pointer_constraint_v1 *active;

		double last_x, last_y;
		double lock_sx, lock_sy;
		bool lock_applied;

		struct wl_listener active_surface_destroy;

		struct wl_listener active_destroy;
		struct wl_listener active_set_region;

		struct wl_listener constraints_destroy;
		struct wl_listener constraints_new_constraint;

		struct wl_listener router_destroy;
		struct wl_listener seat_destroy;
	} WLR_PRIVATE;
};

struct wlr_pointer_constraints_v1_input_router_layer_cursor_hint_event {
	// Global position to warp the pointer to
	double x, y;
};

struct wlr_pointer_constraints_v1 *wlr_pointer_constraints_v1_create(
	struct wl_display *display);

struct wlr_pointer_constraint_v1 *
	wlr_pointer_constraints_v1_constraint_for_surface(
	struct wlr_pointer_constraints_v1 *pointer_constraints,
	struct wlr_surface *surface, struct wlr_seat *seat);

void wlr_pointer_constraint_v1_send_activated(
	struct wlr_pointer_constraint_v1 *constraint);
/**
 * Deactivate the constraint. May destroy the constraint.
 */
void wlr_pointer_constraint_v1_send_deactivated(
	struct wlr_pointer_constraint_v1 *constraint);

bool wlr_pointer_constraints_v1_input_router_layer_register(int32_t priority);

struct wlr_pointer_constraints_v1_input_router_layer *
wlr_pointer_constraints_v1_input_router_layer_create(
		struct wlr_input_router *router, struct wlr_pointer_constraints_v1 *constraints,
		struct wlr_seat *seat);
void wlr_pointer_constraints_v1_input_router_layer_destroy(
		struct wlr_pointer_constraints_v1_input_router_layer *layer);

void wlr_pointer_constraints_v1_input_router_layer_set_active_surface(
		struct wlr_pointer_constraints_v1_input_router_layer *layer,
		struct wlr_surface *surface);

#endif
