#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_input_router.h>
#include <wlr/util/log.h>

static void update_focus(struct wlr_input_router_focus_layer *layer, double x, double y) {
	wlr_input_router_at(layer->router, x, y, &layer->focus, NULL, NULL);
}

static void pointer_handler_position(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_position *event) {
	struct wlr_input_router_focus_layer *layer = wl_container_of(handler, layer, pointer_handler);
	update_focus(layer, event->x, event->y);

	struct wlr_input_router_pointer_position relayed = *event;
	relayed.focus = &layer->focus;
	wlr_input_router_pointer_handler_relay_position(handler, &relayed);
}

static const struct wlr_input_router_pointer_handler_interface pointer_handler_impl = {
	.base = {
		.name = "wlr_input_router_focus_layer",
	},
	.position = pointer_handler_position,
};

static void touch_handler_position(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_position *event) {
	struct wlr_input_router_focus_layer *layer = wl_container_of(handler, layer, touch_handler);
	update_focus(layer, event->x, event->y);

	struct wlr_input_router_touch_position relayed = *event;
	relayed.focus = &layer->focus;
	wlr_input_router_touch_handler_relay_position(handler, &relayed);
}

static uint32_t touch_handler_down(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_down *event) {
	struct wlr_input_router_focus_layer *layer = wl_container_of(handler, layer, touch_handler);
	update_focus(layer, event->x, event->y);

	struct wlr_input_router_touch_down relayed = *event;
	relayed.focus = &layer->focus;
	return wlr_input_router_touch_handler_relay_down(handler, &relayed);
}

static const struct wlr_input_router_touch_handler_interface touch_handler_impl = {
	.base = {
		.name = "wlr_input_router_focus_layer",
	},
	.position = touch_handler_position,
	.down = touch_handler_down,
};

static void handle_router_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_router_focus_layer *layer = wl_container_of(listener, layer, router_destroy);
	wlr_input_router_focus_layer_destroy(layer);
}

bool wlr_input_router_focus_layer_register(int32_t priority) {
	if (!wlr_input_router_register_pointer_handler_interface(&pointer_handler_impl, priority)) {
		return false;
	}
	if (!wlr_input_router_register_touch_handler_interface(&touch_handler_impl, priority)) {
		return false;
	}
	return true;
}

struct wlr_input_router_focus_layer *wlr_input_router_focus_layer_create(
		struct wlr_input_router *router) {
	struct wlr_input_router_focus_layer *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_input_router_pointer_handler_init(&layer->pointer_handler, router, &pointer_handler_impl);
	wlr_input_router_touch_handler_init(&layer->touch_handler, router, &touch_handler_impl);

	wlr_input_router_focus_init(&layer->focus);

	layer->router = router;
	layer->router_destroy.notify = handle_router_destroy;
	wl_signal_add(&router->events.destroy, &layer->router_destroy);

	wl_signal_init(&layer->events.destroy);

	return layer;
}

void wlr_input_router_focus_layer_destroy(struct wlr_input_router_focus_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	wlr_input_router_pointer_handler_finish(&layer->pointer_handler);
	wlr_input_router_touch_handler_finish(&layer->touch_handler);

	wlr_input_router_focus_finish(&layer->focus);

	wl_list_remove(&layer->router_destroy.link);

	free(layer);

}
