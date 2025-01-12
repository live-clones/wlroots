#include <stdlib.h>

#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

static void closest_point(
		pixman_region32_t *region, double x, double y, double *dst_x, double *dst_y) {
	*dst_x = x;
	*dst_y = y;
	double d2_best = INFINITY;

	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(region, &nrects);
	for (int i = 0; i < nrects; i++) {
		pixman_box32_t *rect = &rects[i];
		double box_x = x < rect->x1 ? rect->x1 : x >= rect->x2 ? rect->x2 - 1 : x;
		double box_y = y < rect->y1 ? rect->y1 : y >= rect->y2 ? rect->y2 - 1 : y;
		double dx = box_x - x, dy = box_y - y;
		double d2 = dx * dx + dy * dy;
		if (d2 <= d2_best) {
			d2_best = d2;
			*dst_x = box_x;
			*dst_y = box_y;
		}
		if (d2_best == 0) {
			break;
		}
	}
}

static void pointer_handler_position(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_position *event) {
	struct wlr_pointer_constraints_v1_input_router_layer *layer =
		wl_container_of(handler, layer, pointer_handler);

	struct wlr_pointer_constraint_v1 *constraint = layer->active;

	struct wlr_input_router_pointer_position copy;
	if (constraint != NULL) {
		copy = *event;

		// NB: be careful to not lose precision here
		double surface_x, surface_y;
		if (wlr_input_router_get_surface_position(
				layer->router, constraint->surface, &surface_x, &surface_y)) {
			double sx = event->x - surface_x, sy = event->y - surface_y;

			switch (constraint->type) {
			case WLR_POINTER_CONSTRAINT_V1_LOCKED:
				if (!layer->lock_applied) {
					closest_point(
						&constraint->region, sx, sy, &layer->lock_sx, &layer->lock_sy);
					layer->lock_applied = true;
				}
				copy.x = surface_x + layer->lock_sx;
				copy.y = surface_y + layer->lock_sy;
				break;
			case WLR_POINTER_CONSTRAINT_V1_CONFINED:
				if (!wlr_region_confine(&constraint->region, layer->last_x - surface_x,
						layer->last_y - surface_y, sx, sy, &sx, &sy)) {
					closest_point(&constraint->region, sx, sy, &sx, &sy);
				}
				copy.x = surface_x + sx;
				copy.y = surface_y + sy;
				break;
			}
		}

		event = &copy;
	}

	layer->last_x = event->x;
	layer->last_y = event->y;
	wlr_input_router_pointer_handler_relay_position(handler, event);
}

static const struct wlr_input_router_pointer_handler_interface pointer_handler_impl = {
	.base = {
		.name = "wlr_pointer_constraints_v1_input_router_layer",
	},
	.position = pointer_handler_position,
};

static void set_active(struct wlr_pointer_constraints_v1_input_router_layer *layer,
		struct wlr_pointer_constraint_v1 *constraint) {
	struct wlr_pointer_constraint_v1 *prev = layer->active;
	if (constraint == prev) {
		return;
	}

	layer->lock_applied = false;
	layer->active = constraint;

	wl_list_remove(&layer->active_destroy.link);
	wl_list_remove(&layer->active_set_region.link);

	if (prev != NULL) {
		if (prev->type == WLR_POINTER_CONSTRAINT_V1_LOCKED && prev->current.cursor_hint.enabled) {
			double surface_x, surface_y;
			if (wlr_input_router_get_surface_position(layer->router, prev->surface,
					&surface_x, &surface_y)) {
				struct wlr_pointer_constraints_v1_input_router_layer_cursor_hint_event event = {
					.x = surface_x + prev->current.cursor_hint.x,
					.y = surface_y + prev->current.cursor_hint.y,
				};
				wl_signal_emit_mutable(&layer->events.cursor_hint, &event);
			}
		}

		wlr_pointer_constraint_v1_send_deactivated(prev);
	}

	if (constraint != NULL) {
		wl_signal_add(&constraint->events.destroy, &layer->active_destroy);
		wl_signal_add(&constraint->events.set_region, &layer->active_set_region);

		wlr_pointer_constraint_v1_send_activated(constraint);
	} else {
		wl_list_init(&layer->active_destroy.link);
		wl_list_init(&layer->active_set_region.link);
	}

	wlr_input_router_invalidate_pointer_position(layer->router);
}

static void handle_active_surface_destroy(struct wl_listener *listener, void *data) {
	struct wlr_pointer_constraints_v1_input_router_layer *layer =
		wl_container_of(listener, layer, active_surface_destroy);
	wlr_pointer_constraints_v1_input_router_layer_set_active_surface(layer, NULL);
}

static void handle_active_destroy(struct wl_listener *listener, void *data) {
	struct wlr_pointer_constraints_v1_input_router_layer *layer =
		wl_container_of(listener, layer, active_destroy);
	set_active(layer, NULL);
}

static void handle_active_set_region(struct wl_listener *listener, void *data) {
	struct wlr_pointer_constraints_v1_input_router_layer *layer =
		wl_container_of(listener, layer, active_set_region);
	layer->lock_applied = false;
	wlr_input_router_invalidate_pointer_position(layer->router);
}

static void handle_constraints_destroy(struct wl_listener *listener, void *data) {
	struct wlr_pointer_constraints_v1_input_router_layer *layer =
		wl_container_of(listener, layer, constraints_destroy);
	wlr_pointer_constraints_v1_input_router_layer_destroy(layer);
}

static void handle_constraints_new_constraint(struct wl_listener *listener, void *data) {
	struct wlr_pointer_constraints_v1_input_router_layer *layer =
		wl_container_of(listener, layer, constraints_new_constraint);
	struct wlr_pointer_constraint_v1 *constraint = data;
	if (layer->active_surface == constraint->surface) {
		set_active(layer, constraint);
	}
}

static void handle_router_destroy(struct wl_listener *listener, void *data) {
	struct wlr_pointer_constraints_v1_input_router_layer *layer =
		wl_container_of(listener, layer, router_destroy);
	wlr_pointer_constraints_v1_input_router_layer_destroy(layer);
}

static void handle_seat_destroy(struct wl_listener *listener, void *data) {
	struct wlr_pointer_constraints_v1_input_router_layer *layer =
		wl_container_of(listener, layer, seat_destroy);
	wlr_pointer_constraints_v1_input_router_layer_destroy(layer);
}

void wlr_pointer_constraints_v1_input_router_layer_set_active_surface(
		struct wlr_pointer_constraints_v1_input_router_layer *layer,
		struct wlr_surface *surface) {
	if (layer->active_surface == surface) {
		return;
	}

	layer->active_surface = surface;

	wl_list_remove(&layer->active_surface_destroy.link);
	if (surface != NULL) {
		wl_signal_add(&surface->events.destroy, &layer->active_surface_destroy);
	} else {
		wl_list_init(&layer->active_surface_destroy.link);
	}

	struct wlr_pointer_constraint_v1 *constraint =
		wlr_pointer_constraints_v1_constraint_for_surface(layer->constraints,
			surface, layer->seat);
	set_active(layer, constraint);
}

bool wlr_pointer_constraints_v1_input_router_layer_register(int32_t priority) {
	if (!wlr_input_router_register_pointer_handler_interface(&pointer_handler_impl, priority)) {
		return false;
	}
	return true;
}

struct wlr_pointer_constraints_v1_input_router_layer *
wlr_pointer_constraints_v1_input_router_layer_create(
		struct wlr_input_router *router, struct wlr_pointer_constraints_v1 *constraints,
		struct wlr_seat *seat) {
	struct wlr_pointer_constraints_v1_input_router_layer *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_input_router_pointer_handler_init(&layer->pointer_handler, router, &pointer_handler_impl);

	layer->active_surface_destroy.notify = handle_active_surface_destroy;
	wl_list_init(&layer->active_surface_destroy.link);

	layer->active_destroy.notify = handle_active_destroy;
	wl_list_init(&layer->active_destroy.link);

	layer->active_set_region.notify = handle_active_set_region;
	wl_list_init(&layer->active_set_region.link);

	layer->constraints = constraints;

	layer->constraints_destroy.notify = handle_constraints_destroy;
	wl_signal_add(&constraints->events.destroy, &layer->constraints_destroy);
	layer->constraints_new_constraint.notify = handle_constraints_new_constraint;
	wl_signal_add(&constraints->events.new_constraint, &layer->constraints_new_constraint);

	layer->router = router;
	layer->router_destroy.notify = handle_router_destroy;
	wl_signal_add(&router->events.destroy, &layer->router_destroy);

	layer->seat = seat;
	layer->seat_destroy.notify = handle_seat_destroy;
	wl_signal_add(&seat->events.destroy, &layer->seat_destroy);

	wl_signal_init(&layer->events.destroy);
	wl_signal_init(&layer->events.cursor_hint);

	return layer;
}

void wlr_pointer_constraints_v1_input_router_layer_destroy(
		struct wlr_pointer_constraints_v1_input_router_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	wlr_input_router_pointer_handler_finish(&layer->pointer_handler);

	wl_list_remove(&layer->active_surface_destroy.link);
	wl_list_remove(&layer->active_destroy.link);
	wl_list_remove(&layer->active_set_region.link);
	wl_list_remove(&layer->constraints_new_constraint.link);
	wl_list_remove(&layer->constraints_destroy.link);
	wl_list_remove(&layer->router_destroy.link);
	wl_list_remove(&layer->seat_destroy.link);

	free(layer);
}
