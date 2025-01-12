#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/util/log.h>

static void layer_position(struct wlr_drag_input_router_layer *layer, uint32_t time_msec,
		double x, double y) {
	struct wlr_input_router_focus focus;
	wlr_input_router_focus_init(&focus);

	double sx, sy;
	wlr_input_router_at(layer->router, x, y, &focus, &sx, &sy);

	// One of these will short-circuit
	wlr_drag_enter(layer->drag, wlr_input_router_focus_get_surface(&focus), sx, sy);
	wlr_drag_send_motion(layer->drag, time_msec, sx, sy);

	wlr_input_router_focus_finish(&focus);
}

static void pointer_handler_position(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_position *event) {
	struct wlr_drag_input_router_layer *layer = wl_container_of(handler, layer, pointer.handler);
	layer_position(layer, event->time_msec, event->x, event->y);
}

static void pointer_handler_clear_focus(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_clear_focus *event) {
	struct wlr_drag_input_router_layer *layer = wl_container_of(handler, layer, pointer.handler);
	wlr_drag_clear_focus(layer->drag);
}

static uint32_t pointer_handler_button(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_button *event) {
	struct wlr_drag_input_router_layer *layer = wl_container_of(handler, layer, pointer.handler);
	uint32_t serial = wlr_input_router_pointer_handler_relay_button(handler, event);

	if (event->button == layer->pointer.button &&
			event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		wlr_drag_drop_and_destroy(layer->drag, event->time_msec);
	}

	return serial;
}

static void pointer_handler_axis(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_axis *event) {
	// Consumed
}

static void pointer_handler_frame(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_frame *event) {
	// Consumed
}

static const struct wlr_input_router_pointer_handler_interface pointer_handler_impl = {
	.base = {
		.name = "wlr_drag_input_router_layer",
	},
	.position = pointer_handler_position,
	.clear_focus = pointer_handler_clear_focus,
	.button = pointer_handler_button,
	.axis = pointer_handler_axis,
	.frame = pointer_handler_frame,
};

static void touch_handler_position(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_position *event) {
	struct wlr_drag_input_router_layer *layer = wl_container_of(handler, layer, touch.handler);
	if (event->touch_id == layer->touch.id) {
		layer_position(layer, event->time_msec, event->x, event->y);
	} else {
		wlr_input_router_touch_handler_relay_position(handler, event);
	}
}

static uint32_t touch_handler_down(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_down *event) {
	struct wlr_drag_input_router_layer *layer = wl_container_of(handler, layer, touch.handler);
	if (event->id == layer->touch.id) {
		wlr_log(WLR_ERROR, "Got double down event for id %" PRIi32, event->id);
		return 0;
	} else {
		return wlr_input_router_touch_handler_relay_down(handler, event);
	}
}

static uint32_t touch_handler_up(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_up *event) {
	struct wlr_drag_input_router_layer *layer = wl_container_of(handler, layer, touch.handler);
	if (event->id == layer->touch.id) {
		wlr_drag_drop_and_destroy(layer->drag, event->time_msec);
		return 0;
	} else {
		return wlr_input_router_touch_handler_relay_up(handler, event);
	}
}

static void touch_handler_cancel(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_cancel *event) {
	struct wlr_drag_input_router_layer *layer = wl_container_of(handler, layer, touch.handler);
	wlr_drag_destroy(layer->drag);
}

static void touch_handler_frame(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_frame *event) {
	// Consumed
}

static const struct wlr_input_router_touch_handler_interface touch_handler_impl = {
	.base = {
		.name = "wlr_drag_input_router_layer",
	},
	.position = touch_handler_position,
	.down = touch_handler_down,
	.up = touch_handler_up,
	.cancel = touch_handler_cancel,
	.frame = touch_handler_frame,
};

static void handle_router_destroy(struct wl_listener *listener, void *data) {
	struct wlr_drag_input_router_layer *layer = wl_container_of(listener, layer, router_destroy);
	wlr_drag_input_router_layer_destroy(layer);
}

static void handle_drag_destroy(struct wl_listener *listener, void *data) {
	struct wlr_drag_input_router_layer *layer = wl_container_of(listener, layer, drag_destroy);
	layer->drag = NULL;
	wlr_drag_input_router_layer_destroy(layer);
}

static struct wlr_drag_input_router_layer *layer_create(struct wlr_input_router *router,
		struct wlr_drag *drag, enum wlr_drag_grab_type type) {
	struct wlr_drag_input_router_layer *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	layer->grab_type = type;
	switch (type) {
	case WLR_DRAG_GRAB_KEYBOARD:
		break;
	case WLR_DRAG_GRAB_KEYBOARD_POINTER:
		wlr_input_router_pointer_handler_init(&layer->pointer.handler,
			router, &pointer_handler_impl);

		wlr_input_router_pointer_handler_relay_clear_focus(&layer->pointer.handler,
			&(struct wlr_input_router_pointer_clear_focus){0});
		break;
	case WLR_DRAG_GRAB_KEYBOARD_TOUCH:
		wlr_input_router_touch_handler_init(&layer->touch.handler,
			router, &touch_handler_impl);

		wlr_input_router_touch_handler_relay_up(&layer->touch.handler,
			&(struct wlr_input_router_touch_up){
				.id = layer->touch.id,
			});
		break;
	}

	wlr_drag_start(drag);

	layer->router = router;
	layer->router_destroy.notify = handle_router_destroy;
	wl_signal_add(&router->events.destroy, &layer->router_destroy);

	layer->drag = drag;
	layer->drag_destroy.notify = handle_drag_destroy;
	wl_signal_add(&drag->events.destroy, &layer->drag_destroy);

	wl_signal_init(&layer->events.destroy);

	return layer;
}

bool wlr_drag_input_router_layer_register(int32_t priority) {
	if (!wlr_input_router_register_pointer_handler_interface(&pointer_handler_impl, priority)) {
		return false;
	}
	if (!wlr_input_router_register_touch_handler_interface(&touch_handler_impl, priority)) {
		return false;
	}
	return true;
}

struct wlr_drag_input_router_layer *wlr_drag_input_router_layer_create_pointer(
		struct wlr_input_router *router, struct wlr_drag *drag, uint32_t button) {
	struct wlr_drag_input_router_layer *layer =
		layer_create(router, drag, WLR_DRAG_GRAB_KEYBOARD_POINTER);
	if (layer == NULL) {
		return NULL;
	}
	layer->pointer.button = button;

	return layer;
}

struct wlr_drag_input_router_layer *wlr_drag_input_router_layer_create_touch(
		struct wlr_input_router *router, struct wlr_drag *drag, int32_t id) {
	struct wlr_drag_input_router_layer *layer =
		layer_create(router, drag, WLR_DRAG_GRAB_KEYBOARD_TOUCH);
	if (layer == NULL) {
		return NULL;
	}
	layer->touch.id = id;

	return layer;
}

void wlr_drag_input_router_layer_destroy(struct wlr_drag_input_router_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wl_list_remove(&layer->router_destroy.link);
	wl_list_remove(&layer->drag_destroy.link);

	if (layer->drag != NULL) {
		wlr_drag_destroy(layer->drag);
	}

	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	switch (layer->grab_type) {
	case WLR_DRAG_GRAB_KEYBOARD:
		break;
	case WLR_DRAG_GRAB_KEYBOARD_POINTER:
		wlr_input_router_pointer_handler_finish(&layer->pointer.handler);
		wlr_input_router_invalidate_pointer_position(layer->router);
		break;
	case WLR_DRAG_GRAB_KEYBOARD_TOUCH:
		wlr_input_router_touch_handler_finish(&layer->touch.handler);
		break;
	}

	free(layer);
}
