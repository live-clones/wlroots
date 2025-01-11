#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

static uint32_t pointer_position(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_position_event *event) {
	struct wlr_relative_pointer_v1_input_router_layer *layer =
		wl_container_of(pointer, layer, pointer);
	wlr_relative_pointer_manager_v1_send_relative_motion(layer->manager,
		layer->seat, (uint64_t)event->time_msec * 1000, event->dx, event->dy,
		event->unaccel_dx, event->unaccel_dy);
	return wlr_input_router_pointer_notify_position(pointer, event);
}

static const struct wlr_input_router_pointer_interface pointer_impl = {
	.base = {
		.name = "wlr_relative_pointer_v1_input_router_layer-pointer",
	},
	.position = pointer_position,
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
	if (!wlr_input_router_pointer_register_interface(&pointer_impl, priority)) {
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

	wlr_input_router_pointer_init(&layer->pointer, router, &pointer_impl);

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

	assert(wl_list_empty(&layer->events.destroy.listener_list));

	wlr_input_router_pointer_finish(&layer->pointer);

	wl_list_remove(&layer->router_destroy.link);
	wl_list_remove(&layer->manager_destroy.link);
	wl_list_remove(&layer->seat_destroy.link);

	free(layer);
}
