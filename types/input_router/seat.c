#include <stdlib.h>

#include <wlr/types/wlr_input_router.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

static void keyboard_send_enter(struct wlr_seat_input_router_layer *layer,
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
}

static void keyboard_handler_set_focus(struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_set_focus *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(handler, layer, keyboard_handler);
	keyboard_send_enter(layer, wlr_input_router_focus_get_surface(event->focus));
}

static void keyboard_handler_set_device(struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_set_device *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(handler, layer, keyboard_handler);
	struct wlr_keyboard *keyboard = event->device;

	// XXX: this doesn't allow to relay pressed keys from multiple keyboards at
	// the same time, which could be argued to contradict libinput's view on
	// seats. OTOH, libinput lives in a simple world without key repeat info and
	// keymaps. Oh well.

	if (wlr_seat_get_keyboard(layer->seat) == keyboard) {
		return;
	}

	// Avoid sending new modifiers with old keys
	struct wlr_surface *surface = layer->seat->keyboard_state.focused_surface;
	wlr_seat_keyboard_clear_focus(layer->seat);

	wlr_seat_set_keyboard(layer->seat, keyboard);

	keyboard_send_enter(layer, surface);
}

static void keyboard_handler_key(struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_key *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(handler, layer, keyboard_handler);
	if (event->intercepted) {
		// Update the client state without sending a wl_keyboard.key event
		// XXX: this is suboptimal, wl_keyboard.keys would be better
		// See https://gitlab.freedesktop.org/wayland/wayland/-/merge_requests/406
		struct wlr_surface *surface = layer->seat->keyboard_state.focused_surface;
		wlr_seat_keyboard_clear_focus(layer->seat);
		keyboard_send_enter(layer, surface);
	} else {
		wlr_seat_keyboard_send_key(layer->seat, event->time_msec, event->key, event->state);
	}
}

static void keyboard_handler_modifiers(struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_modifiers *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(handler, layer, keyboard_handler);
	wlr_seat_keyboard_send_modifiers(layer->seat, event->modifiers);
}

static const struct wlr_input_router_keyboard_handler_interface keyboard_handler_impl = {
	.base = {
		.name = "wlr_seat_input_router_layer",
	},
	.set_focus = keyboard_handler_set_focus,
	.set_device = keyboard_handler_set_device,
	.key = keyboard_handler_key,
	.modifiers = keyboard_handler_modifiers,
};

static void pointer_handler_position(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_position *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(handler, layer, pointer_handler);

	struct wlr_surface *surface = wlr_input_router_focus_get_surface(event->focus);
	double sx = 0, sy = 0;

	if (surface != NULL) {
		double surface_x, surface_y;
		if (wlr_input_router_get_surface_position(layer->router, surface,
				&surface_x, &surface_y)) {
			sx = event->x - surface_x;
			sy = event->y - surface_y;
		} else {
			surface = NULL;
		}
	}

	// One of these will short-circuit
	wlr_seat_pointer_enter(layer->seat, surface, sx, sy);
	wlr_seat_pointer_send_motion(layer->seat, event->time_msec, sx, sy);
}

static void pointer_handler_clear_focus(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_clear_focus *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(handler, layer, pointer_handler);
	wlr_seat_pointer_clear_focus(layer->seat);
}

static uint32_t pointer_handler_button(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_button *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(handler, layer, pointer_handler);
	return wlr_seat_pointer_send_button(layer->seat,
		event->time_msec, event->button, event->state);
}

static void pointer_handler_axis(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_axis *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(handler, layer, pointer_handler);
	wlr_seat_pointer_send_axis(layer->seat, event->time_msec, event->orientation,
		event->delta, event->delta_discrete, event->source, event->relative_direction);
}

static void pointer_handler_frame(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_frame *event) {
	struct wlr_seat_input_router_layer *layer = wl_container_of(handler, layer, pointer_handler);
	wlr_seat_pointer_send_frame(layer->seat);
}

static const struct wlr_input_router_pointer_handler_interface pointer_handler_impl = {
	.base = {
		.name = "wlr_seat_input_router_layer",
	},
	.position = pointer_handler_position,
	.clear_focus = pointer_handler_clear_focus,
	.button = pointer_handler_button,
	.axis = pointer_handler_axis,
	.frame = pointer_handler_frame,
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
	if (!wlr_input_router_register_keyboard_handler_interface(
			&keyboard_handler_impl, priority)) {
		return false;
	}
	if (!wlr_input_router_register_pointer_handler_interface(
			&pointer_handler_impl, priority)) {
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

	wlr_input_router_keyboard_handler_init(&layer->keyboard_handler, router, &keyboard_handler_impl);
	wlr_input_router_pointer_handler_init(&layer->pointer_handler, router, &pointer_handler_impl);

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

	wlr_input_router_keyboard_handler_finish(&layer->keyboard_handler);
	wlr_input_router_pointer_handler_finish(&layer->pointer_handler);

	wl_list_remove(&layer->router_destroy.link);
	wl_list_remove(&layer->seat_destroy.link);
	free(layer);
}
