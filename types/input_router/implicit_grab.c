#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/util/log.h>

static uint32_t pointer_position(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_position_event *event) {
	struct wlr_input_router_implicit_grab_layer *layer = wl_container_of(pointer, layer, pointer);

	if (event->explicit_focus) {
		// Invalidate implicit grab serial: the grab is no longer implicit
		layer->pointer_init_serial = 0;
	}

	struct wlr_input_router_pointer_position_event copy;
	if (!event->explicit_focus && layer->pointer_grabbed) {
		copy = *event;
		copy.focus = &layer->pointer_focus;
		event = &copy;
	} else {
		wlr_input_router_focus_copy(&layer->pointer_focus, event->focus);
	}
	return wlr_input_router_pointer_notify_position(pointer, event);
}

static uint32_t pointer_button(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_button_event *event) {
	struct wlr_input_router_implicit_grab_layer *layer = wl_container_of(pointer, layer, pointer);

	uint32_t serial = wlr_input_router_pointer_notify_button(pointer, event);

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (pointer->n_buttons == 1) {
			layer->pointer_grabbed = true;
			layer->pointer_init_button = event->button;
			layer->pointer_init_serial = serial;
		} else {
			// Ensure we didn't miss the first button press event
			assert(layer->pointer_grabbed);
		}
	} else {
		if (event->button == layer->pointer_init_button) {
			// Invalidate implicit grab serial: the furst button has been released
			layer->pointer_init_serial = 0;
		}

		if (pointer->n_buttons == 0) {
			// Ensure we didn't miss the first button release event
			assert(layer->pointer_init_serial == 0);
			layer->pointer_grabbed = false;
			wlr_input_router_pointer_refresh_position(pointer);
		}
	}

	return serial;
}

static const struct wlr_input_router_pointer_interface pointer_impl = {
	.base = {
		.name = "wlr_input_router_implicit_grab_layer-pointer",
	},
	.position = pointer_position,
	.button = pointer_button,
};

static void touch_position(struct wlr_input_router_touch *pointer,
		const struct wlr_input_router_touch_position_event *event) {
	struct wlr_input_router_implicit_grab_layer *layer =
		wl_container_of(pointer, layer, touch);

	struct wlr_input_router_touch_position_event relayed = *event;
	relayed.focus = &layer->touch_points[event->index].focus;
	wlr_input_router_touch_notify_position(pointer, &relayed);
}

static uint32_t touch_down(struct wlr_input_router_touch *pointer,
		const struct wlr_input_router_touch_down_event *event) {
	struct wlr_input_router_implicit_grab_layer *layer =
		wl_container_of(pointer, layer, touch);

	uint32_t serial = wlr_input_router_touch_notify_down(pointer, event);

	struct wlr_input_router_implicit_grab_layer_touch_point *point =
		&layer->touch_points[event->index];
	point->serial = serial;
	wlr_input_router_focus_copy(&point->focus, event->focus);

	return serial;
}

static const struct wlr_input_router_touch_interface touch_impl = {
	.base = {
		.name = "wlr_input_router_implicit_grab_layer-touch",
	},
	.position = touch_position,
	.down = touch_down,
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

	if (layer->pointer_init_serial != serial) {
		return false;
	}
	if (origin != NULL && origin != wlr_input_router_focus_get_surface(&layer->pointer_focus)) {
		return false;
	}

	if (button != NULL) {
		*button = layer->pointer_init_button;
	}
	return true;
}

bool wlr_input_router_implicit_grab_layer_validate_touch_serial(
		struct wlr_input_router_implicit_grab_layer *layer, struct wlr_surface *origin,
		uint32_t serial, int32_t *id) {
	if (serial == 0) {
		return false;
	}

	for (size_t i = 0; i < layer->touch.n_points; i++) {
		struct wlr_input_router_implicit_grab_layer_touch_point *point = &layer->touch_points[i];
		if (layer->touch_points[i].serial != serial) {
			continue;
		}
		if (origin != NULL && origin != wlr_input_router_focus_get_surface(&point->focus)) {
			continue;
		}

		if (id != NULL) {
			*id = layer->touch.points[i].id;
		}
		return true;
	}

	return false;
}

bool wlr_input_router_implicit_grab_layer_register(int32_t priority) {
	if (!wlr_input_router_pointer_register_interface(&pointer_impl, priority)) {
		return false;
	}
	if (!wlr_input_router_touch_register_interface(&touch_impl, priority)) {
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

	wlr_input_router_pointer_init(&layer->pointer, router, &pointer_impl);
	wlr_input_router_focus_init(&layer->pointer_focus);

	wlr_input_router_touch_init(&layer->touch, router, &touch_impl);
	for (size_t i = 0; i < WLR_INPUT_ROUTER_MAX_TOUCH_POINTS; i++) {
		wlr_input_router_focus_init(&layer->touch_points[i].focus);
	}

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

	assert(wl_list_empty(&layer->events.destroy.listener_list));

	wlr_input_router_pointer_finish(&layer->pointer);
	wlr_input_router_focus_finish(&layer->pointer_focus);

	wlr_input_router_touch_finish(&layer->touch);
	for (size_t i = 0; i < WLR_INPUT_ROUTER_MAX_TOUCH_POINTS; i++) {
		wlr_input_router_focus_finish(&layer->touch_points[i].focus);
	}

	wl_list_remove(&layer->router_destroy.link);

	free(layer);
}
