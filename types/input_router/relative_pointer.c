#include <stdlib.h>

#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

static void pointer_handler_position(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_position *event) {
	struct wlr_relative_pointer_v1_input_router_layer *layer =
		wl_container_of(handler, layer, pointer_handler);
	wlr_relative_pointer_manager_v1_send_relative_motion(layer->manager,
		layer->seat, (uint64_t)event->time_msec * 1000, event->dx, event->dy,
		event->unaccel_dx, event->unaccel_dy);
	wlr_input_router_pointer_handler_relay_position(handler, event);
}

static const struct wlr_input_router_pointer_handler_interface pointer_handler_impl = {
	.base = {
		.name = "wlr_relative_pointer_v1_input_router_layer",
	},
	.position = pointer_handler_position,
};

static void handle_router_destroy(struct wl_listener *listener, void *data) {
	struct wlr_relative_pointer_v1_input_router_layer *layer =
		wl_container_of(listener, layer, router_destroy);
	wlr_relative_pointer_v1_input_router_layer_destroy(layer);
}

static void handle_manager_destroy(struct wl_listener *listener, void *data) {
	struct wlr_relative_pointer_v1_input_router_layer *layer =
		wl_container_of(listener, layer, manager_destroy);
	wlr_relative_pointer_v1_input_router_layer_destroy(layer);
}

static void handle_seat_destroy(struct wl_listener *listener, void *data) {
	struct wlr_relative_pointer_v1_input_router_layer *layer =
		wl_container_of(listener, layer, seat_destroy);
	wlr_relative_pointer_v1_input_router_layer_destroy(layer);
}

bool wlr_relative_pointer_v1_input_router_layer_register(int32_t priority) {
	if (!wlr_input_router_register_pointer_handler_interface(&pointer_handler_impl, priority)) {
		return false;
	}
	return true;
}

struct wlr_relative_pointer_v1_input_router_layer *
wlr_relative_pointer_v1_input_router_layer_create(
		struct wlr_input_router *router, struct wlr_relative_pointer_manager_v1 *manager,
		struct wlr_seat *seat) {
	struct wlr_relative_pointer_v1_input_router_layer *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_input_router_pointer_handler_init(&layer->pointer_handler, router, &pointer_handler_impl);

	layer->router = router;
	layer->router_destroy.notify = handle_router_destroy;
	wl_signal_add(&router->events.destroy, &layer->router_destroy);

	layer->manager = manager;
	layer->manager_destroy.notify = handle_manager_destroy;
	wl_signal_add(&manager->events.destroy, &layer->manager_destroy);

	layer->seat = seat;
	layer->seat_destroy.notify = handle_seat_destroy;
	wl_signal_add(&seat->events.destroy, &layer->seat_destroy);

	wl_signal_init(&layer->events.destroy);

	return layer;
}

void wlr_relative_pointer_v1_input_router_layer_destroy(
		struct wlr_relative_pointer_v1_input_router_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	wlr_input_router_pointer_handler_finish(&layer->pointer_handler);

	wl_list_remove(&layer->router_destroy.link);
	wl_list_remove(&layer->manager_destroy.link);
	wl_list_remove(&layer->seat_destroy.link);

	free(layer);
}
