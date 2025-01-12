#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_input_router.h>
#include <wlr/util/log.h>

struct touch_point {
	int32_t id;
	uint32_t serial;
	struct wlr_input_router_focus focus;
	struct wl_list link; // wlr_input_router_implicit_grab_layer.touch_points
};

static void pointer_handler_position(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_position *event) {
	struct wlr_input_router_implicit_grab_layer *layer = wl_container_of(handler, layer, pointer_handler);

	struct wlr_input_router_pointer_position copy;
	if (layer->pointer_grabbed) {
		copy = *event;
		copy.focus = &layer->pointer_focus;
		event = &copy;
	} else {
		wlr_input_router_focus_copy(&layer->pointer_focus, event->focus);
	}
	wlr_input_router_pointer_handler_relay_position(handler, event);
}

static void pointer_handler_clear_focus(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_clear_focus *event) {
	struct wlr_input_router_implicit_grab_layer *layer = wl_container_of(handler, layer, pointer_handler);
	wlr_input_router_focus_clear(&layer->pointer_focus);
	wlr_input_router_pointer_handler_relay_clear_focus(handler, event);
}

static uint32_t pointer_handler_button(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_button *event) {
	struct wlr_input_router_implicit_grab_layer *layer = wl_container_of(handler, layer, pointer_handler);

	uint32_t serial = wlr_input_router_pointer_handler_relay_button(handler, event);

	switch (event->grab_state) {
	case WLR_INPUT_ROUTER_POINTER_GRAB_STATE_UNKNOWN:
		wlr_log(WLR_ERROR, "Received a button event with UNKNOWN grab state");
		abort();
	case WLR_INPUT_ROUTER_POINTER_GRAB_STATE_STARTED:
		layer->pointer_grabbed = true;
		layer->pointer_button = event->button;
		layer->pointer_serial = serial;
		break;
	case WLR_INPUT_ROUTER_POINTER_GRAB_STATE_GRABBED:
	 	if (event->button == layer->pointer_button) {
			// Ensure the button is pressed only once
			assert(event->state == WL_POINTER_BUTTON_STATE_RELEASED);
			layer->pointer_button = 0;
		}
		break;
	case WLR_INPUT_ROUTER_POINTER_GRAB_STATE_STOPPED:
		layer->pointer_grabbed = false;
		wlr_input_router_invalidate_pointer_position(layer->router);
		break;
	}

	return serial;
}

static const struct wlr_input_router_pointer_handler_interface pointer_handler_impl = {
	.base = {
		.name = "wlr_input_router_implicit_grab_layer",
	},
	.position = pointer_handler_position,
	.clear_focus = pointer_handler_clear_focus,
	.button = pointer_handler_button,
};

static void destroy_touch_point(struct touch_point *point) {
	wlr_input_router_focus_finish(&point->focus);
	wl_list_remove(&point->link);
	free(point);
}

static void clear_touch_points(struct wlr_input_router_implicit_grab_layer *layer) {
	struct touch_point *point, *tmp;
	wl_list_for_each_safe(point, tmp, &layer->touch_points, link) {
		destroy_touch_point(point);
	}
}

static void touch_handler_position(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_position *event) {
	struct wlr_input_router_implicit_grab_layer *layer =
		wl_container_of(handler, layer, touch_handler);

	struct touch_point *point;
	wl_list_for_each(point, &layer->touch_points, link) {
		if (point->id == event->touch_id) {
			struct wlr_input_router_touch_position relayed = *event;
			relayed.focus = &point->focus;
			wlr_input_router_touch_handler_relay_position(handler, &relayed);
			break;
		}
	}
}

static uint32_t touch_handler_down(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_down *event) {
	struct wlr_input_router_implicit_grab_layer *layer =
		wl_container_of(handler, layer, touch_handler);

	struct touch_point *point = calloc(1, sizeof(*point));
	if (point == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return 0;
	}

	uint32_t serial = wlr_input_router_touch_handler_relay_down(handler, event);

	point->id = event->id;
	point->serial = serial;
	wlr_input_router_focus_init(&point->focus);
	wlr_input_router_focus_copy(&point->focus, event->focus);

	wl_list_insert(&layer->touch_points, &point->link);

	return serial;
}

static uint32_t touch_handler_up(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_up *event) {
	struct wlr_input_router_implicit_grab_layer *layer =
		wl_container_of(handler, layer, touch_handler);

	struct touch_point *point;
	wl_list_for_each(point, &layer->touch_points, link) {
		if (point->id == event->id) {
			destroy_touch_point(point);
			return wlr_input_router_touch_handler_relay_up(handler, event);
		}
	}

	return 0;
}

static void touch_handler_cancel(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_cancel *event) {
	struct wlr_input_router_implicit_grab_layer *layer =
		wl_container_of(handler, layer, touch_handler);
	clear_touch_points(layer);
	wlr_input_router_touch_handler_relay_cancel(handler, event);
}

static const struct wlr_input_router_touch_handler_interface touch_handler_impl = {
	.base = {
		.name = "wlr_input_router_implicit_grab_layer",
	},
	.position = touch_handler_position,
	.down = touch_handler_down,
	.up = touch_handler_up,
	.cancel = touch_handler_cancel,
};

static void handle_router_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_router_implicit_grab_layer *layer =
		wl_container_of(listener, layer, router_destroy);
	wlr_input_router_implicit_grab_layer_destroy(layer);
}

bool wlr_input_router_implicit_grab_layer_validate_pointer_serial(
		struct wlr_input_router_implicit_grab_layer *layer, struct wlr_surface *origin,
		uint32_t serial, uint32_t *button) {
	if (serial == 0) {
		return false;
	}

	if (!layer->pointer_grabbed || layer->pointer_button == 0 || layer->pointer_serial != serial) {
		return false;
	}
	if (origin != NULL && origin != wlr_input_router_focus_get_surface(&layer->pointer_focus)) {
		return false;
	}

	if (button != NULL) {
		*button = layer->pointer_button;
	}
	return true;
}

bool wlr_input_router_implicit_grab_layer_validate_touch_serial(
		struct wlr_input_router_implicit_grab_layer *layer, struct wlr_surface *origin,
		uint32_t serial, int32_t *id) {
	if (serial == 0) {
		return false;
	}

	struct touch_point *point;
	wl_list_for_each(point, &layer->touch_points, link) {
		if (point->serial != serial) {
			continue;
		}
		if (origin != NULL && origin != wlr_input_router_focus_get_surface(&point->focus)) {
			continue;
		}

		if (id != NULL) {
			*id = point->id;
			return true;
		}
	}

	return false;
}

bool wlr_input_router_implicit_grab_layer_register(int32_t priority) {
	if (!wlr_input_router_register_pointer_handler_interface(&pointer_handler_impl, priority)) {
		return false;
	}
	if (!wlr_input_router_register_touch_handler_interface(&touch_handler_impl, priority)) {
		return false;
	}
	return true;
}

struct wlr_input_router_implicit_grab_layer *wlr_input_router_implicit_grab_layer_create(
		struct wlr_input_router *router) {
	struct wlr_input_router_implicit_grab_layer *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_input_router_pointer_handler_init(&layer->pointer_handler, router, &pointer_handler_impl);
	wlr_input_router_focus_init(&layer->pointer_focus);

	wlr_input_router_touch_handler_init(&layer->touch_handler, router, &touch_handler_impl);
	wl_list_init(&layer->touch_points);

	layer->router = router;
	layer->router_destroy.notify = handle_router_destroy;
	wl_signal_add(&router->events.destroy, &layer->router_destroy);

	wl_signal_init(&layer->events.destroy);

	return layer;
}

void wlr_input_router_implicit_grab_layer_destroy(
		struct wlr_input_router_implicit_grab_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	wlr_input_router_pointer_handler_finish(&layer->pointer_handler);
	wlr_input_router_focus_finish(&layer->pointer_focus);

	wlr_input_router_touch_handler_finish(&layer->touch_handler);
	clear_touch_points(layer);

	wl_list_remove(&layer->router_destroy.link);

	free(layer);

}
