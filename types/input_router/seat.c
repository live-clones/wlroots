#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

// TODO: wlr_seat_touch_send_*() functions are currently useless
// https://gitlab.freedesktop.org/wlroots/wlroots/-/issues/3478

// For touch points, seat_client is NULL if the client is not aware of the point

static void clear_touch_point_seat_client(struct wlr_seat_input_router_layer_touch_point *point) {
	point->seat_client = NULL;
	wl_list_remove(&point->seat_client_destroy.link);
	wl_list_init(&point->seat_client_destroy.link);
}

static void touch_point_handle_seat_client_destroy(struct wl_listener *listener, void *data) {
	struct wlr_seat_input_router_layer_touch_point *point =
		wl_container_of(listener, point, seat_client_destroy);
	clear_touch_point_seat_client(point);
}

static void init_touch_point(struct wlr_seat_input_router_layer_touch_point *point) {
	*point = (struct wlr_seat_input_router_layer_touch_point){0};
	point->seat_client_destroy.notify = touch_point_handle_seat_client_destroy;
	wl_list_init(&point->seat_client_destroy.link);
}

static void finish_touch_point(struct wlr_seat_input_router_layer_touch_point *point) {
	wl_list_remove(&point->seat_client_destroy.link);
}

static uint32_t keyboard_send_enter(struct wlr_seat_input_router_layer *layer,
		struct wlr_surface *surface) {
	uint32_t *keycodes = NULL;
	size_t num_keycodes = 0;
	const struct wlr_keyboard_modifiers *modifiers = NULL;

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(layer->seat);
	if (keyboard != NULL) {
		keycodes = keyboard->keycodes;
		num_keycodes = keyboard->num_keycodes;
		modifiers = &keyboard->modifiers;
	}

	wlr_seat_keyboard_enter(layer->seat, surface, keycodes, num_keycodes, modifiers);
	// TODO: get a serial from enter
	return 0;
}

static uint32_t keyboard_focus(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_focus_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(keyboard, layer, keyboard);
	return keyboard_send_enter(layer, wlr_input_router_focus_get_surface(event->focus));
}

static void keyboard_device(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_device_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(keyboard, layer, keyboard);
	struct wlr_keyboard *device = event->device;

	if (wlr_seat_get_keyboard(layer->seat) == device) {
		return;
	}

	// Avoid sending new modifiers with old keys
	struct wlr_surface *surface = layer->seat->keyboard_state.focused_surface;
	wlr_seat_keyboard_clear_focus(layer->seat);

	wlr_seat_set_keyboard(layer->seat, device);

	keyboard_send_enter(layer, surface);
}

static uint32_t keyboard_key(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_key_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(keyboard, layer, keyboard);
	if (event->intercepted) {
		// Update the client state without sending a wl_keyboard.key event
		// XXX: this is suboptimal, wl_keyboard.keys would be better
		// See https://gitlab.freedesktop.org/wayland/wayland/-/merge_requests/406
		struct wlr_surface *surface = layer->seat->keyboard_state.focused_surface;
		wlr_seat_keyboard_clear_focus(layer->seat);
		keyboard_send_enter(layer, surface);
		return 0;
	} else {
		wlr_seat_keyboard_send_key(layer->seat, event->time_msec, event->key, event->state);
		// TODO: get a serial from key
		return 0;
	}
}

static void keyboard_modifiers(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_modifiers_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(keyboard, layer, keyboard);
	wlr_seat_keyboard_send_modifiers(layer->seat, &keyboard->device->modifiers);
}

static const struct wlr_input_router_keyboard_interface keyboard_impl = {
	.base = {
		.name = "wlr_seat_input_router_layer-keyboard",
	},
	.focus = keyboard_focus,
	.device = keyboard_device,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
};

static uint32_t pointer_position(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_position_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(pointer, layer, pointer);

	struct wlr_surface *surface = wlr_input_router_focus_get_surface(event->focus);
	double sx = 0, sy = 0;

	if (surface != NULL) {
		double surface_x, surface_y;
		if (!wlr_input_router_get_surface_position(layer->router, surface,
				&surface_x, &surface_y)) {
			return 0;
		}
		sx = event->x - surface_x;
		sy = event->y - surface_y;
	}

	// One of these will short-circuit
	wlr_seat_pointer_enter(layer->seat, surface, sx, sy);
	wlr_seat_pointer_send_motion(layer->seat, event->time_msec, sx, sy);

	// TODO: get a serial from enter
	return 0;
}

static uint32_t pointer_button(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_button_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(pointer, layer, pointer);
	return wlr_seat_pointer_send_button(layer->seat,
		event->time_msec, event->button, event->state);
}

static void pointer_axis(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_axis_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(pointer, layer, pointer);
	wlr_seat_pointer_send_axis(layer->seat, event->time_msec, event->orientation,
		event->delta, event->delta_discrete, event->source, event->relative_direction);
}

static void pointer_frame(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_frame_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(pointer, layer, pointer);
	wlr_seat_pointer_send_frame(layer->seat);
}

static const struct wlr_input_router_pointer_interface pointer_impl = {
	.base = {
		.name = "wlr_seat_input_router_layer-pointer",
	},
	.position = pointer_position,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
};

static void touch_position(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_position_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(touch, layer, touch);

	struct wlr_seat_input_router_layer_touch_point *point = &layer->touch_points[event->index];
	struct wlr_surface *surface = wlr_input_router_focus_get_surface(event->focus);
	if (surface == NULL) {
		return;
	}

	double surface_x, surface_y;
	if (!wlr_input_router_get_surface_position(layer->router, surface, &surface_x, &surface_y)) {
		return;
	}
	double sx = event->x - surface_x, sy = event->y - surface_y;

	wl_fixed_t sx_fixed = wl_fixed_from_double(sx), sy_fixed = wl_fixed_from_double(sy);
	if (point->sx == sx_fixed && point->sy == sy_fixed) {
		// Avoid sending same values
		return;
	}
	point->sx = sx_fixed;
	point->sy = sy_fixed;

	struct wl_resource *resource;
	wl_resource_for_each(resource, &point->seat_client->touches) {
		wl_touch_send_motion(resource, event->time_msec, event->id, sx_fixed, sy_fixed);
	}
	point->seat_client->needs_touch_frame = true;
}

static uint32_t touch_down(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_down_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(touch, layer, touch);

	struct wlr_seat_input_router_layer_touch_point *point = &layer->touch_points[event->index];
	init_touch_point(point);

	struct wlr_surface *surface = wlr_input_router_focus_get_surface(event->focus);
	if (surface == NULL) {
		return 0;
	}

	double surface_x, surface_y;
	if (!wlr_input_router_get_surface_position(layer->router, surface, &surface_x, &surface_y)) {
		return 0;
	}
	double sx = event->x - surface_x, sy = event->y - surface_y;

	struct wlr_seat_client *seat_client = wlr_seat_client_for_wl_client(layer->seat,
		wl_resource_get_client(surface->resource));
	if (seat_client == NULL) {
		return 0;
	}

	point->seat_client = seat_client;
	wl_signal_add(&seat_client->events.destroy, &point->seat_client_destroy);

	wl_fixed_t sx_fixed = wl_fixed_from_double(sx), sy_fixed = wl_fixed_from_double(sy);
	point->sx = sx_fixed;
	point->sy = sy_fixed;

	uint32_t serial = wlr_seat_client_next_serial(seat_client);

	struct wl_resource *resource;
	wl_resource_for_each(resource, &seat_client->touches) {
		wl_touch_send_down(resource, serial, event->time_msec, surface->resource,
			event->id, sx_fixed, sy_fixed);
	}
	seat_client->needs_touch_frame = true;

	return serial;
}

static uint32_t touch_up(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_up_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(touch, layer, touch);

	struct wlr_seat_input_router_layer_touch_point *point = &layer->touch_points[event->index];
	struct wlr_seat_client *seat_client = point->seat_client;
	finish_touch_point(point);

	if (seat_client == NULL) {
		return 0;
	}

	uint32_t serial = wlr_seat_client_next_serial(seat_client);

	struct wl_resource *resource;
	wl_resource_for_each(resource, &seat_client->touches) {
		wl_touch_send_up(resource, serial, event->time_msec, event->id);
	}
	seat_client->needs_touch_frame = true;

	return serial;
}

static void touch_cancel(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_cancel_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(touch, layer, touch);

	struct wlr_seat_input_router_layer_touch_point *point = &layer->touch_points[event->index];
	struct wlr_seat_client *seat_client = point->seat_client;
	if (seat_client == NULL) {
		return;
	}

	// Cancels are client-wide
	for (size_t i = 0; i < touch->n_points; i++) {
		point = &layer->touch_points[i];
		if (point->seat_client == seat_client) {
			clear_touch_point_seat_client(point);
		}
	}

	struct wl_resource *resource;
	wl_resource_for_each(resource, &seat_client->touches) {
		wl_touch_send_cancel(resource);
	}
}

static void touch_frame(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_frame_event *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(touch, layer, touch);

	for (size_t i = 0; i < touch->n_points; i++) {
		struct wlr_seat_input_router_layer_touch_point *point = &layer->touch_points[i];
		struct wlr_seat_client *seat_client = point->seat_client;
		if (seat_client != NULL && seat_client->needs_touch_frame) {
			struct wl_resource *resource;
			wl_resource_for_each(resource, &seat_client->touches) {
				wl_touch_send_frame(resource);
			}
			seat_client->needs_touch_frame = false;
		}
	}
}

static const struct wlr_input_router_touch_interface touch_impl = {
	.base = {
		.name = "wlr_seat_input_router_layer-touch",
	},
	.position = touch_position,
	.down = touch_down,
	.up = touch_up,
	.cancel = touch_cancel,
	.frame = touch_frame,
};

static void handle_router_destroy(struct wl_listener *listener, void *data) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(listener, layer, router_destroy);
	wlr_seat_input_router_layer_destroy(layer);
}

static void handle_seat_destroy(struct wl_listener *listener, void *data) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(listener, layer, seat_destroy);
	wlr_seat_input_router_layer_destroy(layer);
}

bool wlr_seat_input_router_layer_register(int32_t priority) {
	if (!wlr_input_router_keyboard_register_interface(&keyboard_impl, priority)) {
		return false;
	}
	if (!wlr_input_router_pointer_register_interface(&pointer_impl, priority)) {
		return false;
	}
	if (!wlr_input_router_touch_register_interface(&touch_impl, priority)) {
		return false;
	}
	return true;
}

struct wlr_seat_input_router_layer *wlr_seat_input_router_layer_create(
		struct wlr_input_router *router, struct wlr_seat *seat) {
	struct wlr_seat_input_router_layer *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_input_router_keyboard_init(&layer->keyboard, router, &keyboard_impl);
	wlr_input_router_pointer_init(&layer->pointer, router, &pointer_impl);

	wlr_input_router_touch_init(&layer->touch, router, &touch_impl);
	for (size_t i = 0; i < layer->touch.n_points; i++) {
		init_touch_point(&layer->touch_points[i]);
	}

	layer->router = router;
	layer->router_destroy.notify = handle_router_destroy;
	wl_signal_add(&router->events.destroy, &layer->router_destroy);

	layer->seat = seat;
	layer->seat_destroy.notify = handle_seat_destroy;
	wl_signal_add(&seat->events.destroy, &layer->seat_destroy);

	wl_signal_init(&layer->events.destroy);

	return layer;
}

void wlr_seat_input_router_layer_destroy(struct wlr_seat_input_router_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	assert(wl_list_empty(&layer->events.destroy.listener_list));

	wlr_input_router_keyboard_finish(&layer->keyboard);
	wlr_input_router_pointer_finish(&layer->pointer);

	for (size_t i = 0; i < layer->touch.n_points; i++) {
		finish_touch_point(&layer->touch_points[i]);
	}
	wlr_input_router_touch_finish(&layer->touch);

	wl_list_remove(&layer->router_destroy.link);
	wl_list_remove(&layer->seat_destroy.link);
	free(layer);
}
