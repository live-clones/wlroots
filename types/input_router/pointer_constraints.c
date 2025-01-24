#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

static void closest_point(pixman_region32_t *region, double x, double y,
		double *out_x, double *out_y) {
	*out_x = x;
	*out_y = y;
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
			*out_x = box_x;
			*out_y = box_y;
		}
		if (d2_best == 0) {
			break;
		}
	}
}

static uint32_t update_position(struct wlr_pointer_constraints_v1_input_router_layer *layer,
		const struct wlr_input_router_pointer_position_event *event) {
	struct wlr_pointer_constraint_v1 *constraint = layer->active;

	struct wlr_input_router_pointer_position_event copy;
	if (constraint != NULL) {
		double surface_x, surface_y;
		if (!wlr_input_router_get_surface_position(layer->router, constraint->surface,
				&surface_x, &surface_y)) {
			goto out;
		}
		double sx = event->x - surface_x, sy = event->y - surface_y;

		copy = *event;
		switch (constraint->type) {
		case WLR_POINTER_CONSTRAINT_V1_LOCKED:
			if (!layer->lock_applied) {
				closest_point(&constraint->region, sx, sy, &layer->lock_sx, &layer->lock_sy);
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

		event = &copy;
	}

out:
	layer->last_x = event->x;
	layer->last_y = event->y;
	return wlr_input_router_pointer_notify_position(&layer->pointer, event);
}

static uint32_t pointer_position(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_position_event *event) {
	struct wlr_pointer_constraints_v1_input_router_layer *layer =
		wl_container_of(pointer, layer, pointer);
	return update_position(layer, event);
}

static const struct wlr_input_router_pointer_interface pointer_impl = {
	.base = {
		.name = "wlr_pointer_constraints_v1_input_router_layer-pointer",
	},
	.position = pointer_position,
};

static void refresh_position(struct wlr_pointer_constraints_v1_input_router_layer *layer) {
	update_position(layer,
		&(struct wlr_input_router_pointer_position_event){
			.x = layer->pointer.x,
			.y = layer->pointer.y,
			.focus = &layer->pointer.focus,
			.synthetic = true,
		});
}

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

	refresh_position(layer);
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
	refresh_position(layer);
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
	if (!wlr_input_router_pointer_register_interface(&pointer_impl, priority)) {
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

	wlr_input_router_pointer_init(&layer->pointer, router, &pointer_impl);

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

	assert(wl_list_empty(&layer->events.destroy.listener_list));
	assert(wl_list_empty(&layer->events.cursor_hint.listener_list));

	wlr_input_router_pointer_finish(&layer->pointer);

	wl_list_remove(&layer->active_surface_destroy.link);
	wl_list_remove(&layer->active_destroy.link);
	wl_list_remove(&layer->active_set_region.link);
	wl_list_remove(&layer->constraints_new_constraint.link);
	wl_list_remove(&layer->constraints_destroy.link);
	wl_list_remove(&layer->router_destroy.link);
	wl_list_remove(&layer->seat_destroy.link);

	free(layer);
}
