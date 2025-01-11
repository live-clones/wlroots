#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_input_router.h>
#include <wlr/util/log.h>

static uint32_t pointer_handler_button(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_button *event) {
	struct wlr_input_router_pointer_grab_layer *layer =
		wl_container_of(handler, layer, pointer_handler);

	struct wlr_input_router_pointer_button relayed = *event;

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		++layer->pointer_n_buttons;
		relayed.grab_state = layer->pointer_n_buttons == 1 ?
			WLR_INPUT_ROUTER_POINTER_GRAB_STATE_STARTED :
			WLR_INPUT_ROUTER_POINTER_GRAB_STATE_GRABBED;
	} else {
		assert(layer->pointer_n_buttons > 0);
		--layer->pointer_n_buttons;
		relayed.grab_state = layer->pointer_n_buttons == 0 ?
			WLR_INPUT_ROUTER_POINTER_GRAB_STATE_STOPPED :
			WLR_INPUT_ROUTER_POINTER_GRAB_STATE_GRABBED;
	}

	return wlr_input_router_pointer_handler_relay_button(handler, &relayed);
}

static const struct wlr_input_router_pointer_handler_interface pointer_handler_impl = {
	.base = {
		.name = "wlr_input_router_pointer_grab_layer",
	},
	.button = pointer_handler_button,
};

static void handle_router_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_router_pointer_grab_layer *layer =
		wl_container_of(listener, layer, router_destroy);
	wlr_input_router_pointer_grab_layer_destroy(layer);
}

bool wlr_input_router_pointer_grab_layer_register(int32_t priority) {
	if (!wlr_input_router_register_pointer_handler_interface(&pointer_handler_impl, priority)) {
		return false;
	}
	return true;
}

struct wlr_input_router_pointer_grab_layer *wlr_input_router_pointer_grab_layer_create(
		struct wlr_input_router *router) {
	struct wlr_input_router_pointer_grab_layer *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_input_router_pointer_handler_init(&layer->pointer_handler, router, &pointer_handler_impl);

	layer->router = router;
	layer->router_destroy.notify = handle_router_destroy;
	wl_signal_add(&router->events.destroy, &layer->router_destroy);

	wl_signal_init(&layer->events.destroy);

	return layer;
}

void wlr_input_router_pointer_grab_layer_destroy(
		struct wlr_input_router_pointer_grab_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	wlr_input_router_pointer_handler_finish(&layer->pointer_handler);

	wl_list_remove(&layer->router_destroy.link);
	free(layer);
}
