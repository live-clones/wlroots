#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/util/log.h>

static void update_focus(struct wlr_input_router_focus_layer *layer, double x, double y) {
	wlr_input_router_at(layer->router, x, y, &layer->focus, NULL, NULL);
}

static uint32_t pointer_position(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_position_event *event) {
	struct wlr_input_router_focus_layer *layer = wl_container_of(pointer, layer, pointer);

	struct wlr_input_router_pointer_position_event copy;
	if (!event->explicit_focus) {
		update_focus(layer, event->x, event->y);
		copy = *event;
		copy.focus = &layer->focus;
		event = &copy;
	}
	return wlr_input_router_pointer_notify_position(pointer, event);
}

static const struct wlr_input_router_pointer_interface pointer_impl = {
	.base = {
		.name = "wlr_input_router_focus_layer-pointer",
	},
	.position = pointer_position,
};

static void touch_position(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_position_event *event) {
	struct wlr_input_router_focus_layer *layer = wl_container_of(touch, layer, touch);
	update_focus(layer, event->x, event->y);

	struct wlr_input_router_touch_position_event relayed = *event;
	relayed.focus = &layer->focus;
	wlr_input_router_touch_notify_position(touch, &relayed);
}

static uint32_t touch_down(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_down_event *event) {
	struct wlr_input_router_focus_layer *layer = wl_container_of(touch, layer, touch);
	update_focus(layer, event->x, event->y);

	struct wlr_input_router_touch_down_event relayed = *event;
	relayed.focus = &layer->focus;
	return wlr_input_router_touch_notify_down(touch, &relayed);
}

static const struct wlr_input_router_touch_interface touch_impl = {
	.base = {
		.name = "wlr_input_router_focus_layer-touch",
	},
	.position = touch_position,
	.down = touch_down,
};

static void handle_router_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_router_focus_layer *layer = wl_container_of(listener, layer, router_destroy);
	wlr_input_router_focus_layer_destroy(layer);
}

bool wlr_input_router_focus_layer_register(int32_t priority) {
	if (!wlr_input_router_pointer_register_interface(&pointer_impl, priority)) {
		return false;
	}
	if (!wlr_input_router_touch_register_interface(&touch_impl, priority)) {
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

	wlr_input_router_pointer_init(&layer->pointer, router, &pointer_impl);
	wlr_input_router_touch_init(&layer->touch, router, &touch_impl);

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

	assert(wl_list_empty(&layer->events.destroy.listener_list));

	wlr_input_router_pointer_finish(&layer->pointer);
	wlr_input_router_touch_finish(&layer->touch);

	wlr_input_router_focus_finish(&layer->focus);

	wl_list_remove(&layer->router_destroy.link);

	free(layer);

}
