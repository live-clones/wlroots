#include <stdlib.h>

#include <wlr/backend/session.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

static void set_device(struct wlr_session_input_router_layer *layer,
		struct wlr_keyboard *device) {
	if (layer->device == device) {
		return;
	}
	layer->device = device;
	wl_list_remove(&layer->device_destroy.link);
	if (device != NULL) {
		wl_signal_add(&device->base.events.destroy, &layer->device_destroy);
	} else {
		wl_list_init(&layer->device_destroy.link);
	}
}

static void keyboard_handler_set_device(struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_set_device *event) {
	struct wlr_session_input_router_layer *layer =
		wl_container_of(handler, layer, keyboard_handler);
	set_device(layer, event->device);
	wlr_input_router_keyboard_handler_relay_set_device(handler, event);
}

static void keyboard_handler_key(struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_key *event) {
	struct wlr_session_input_router_layer *layer =
		wl_container_of(handler, layer, keyboard_handler);
	struct wlr_input_router_keyboard_key copy;

	if (layer->device != NULL && layer->device->xkb_state != NULL) {
		bool intercepted = false;

		const xkb_keysym_t *syms;
		int n_syms = xkb_state_key_get_syms(layer->device->xkb_state, event->key + 8, &syms);
		for (int i = 0; i < n_syms; i++) {
			xkb_keysym_t sym = syms[i];
			if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
				copy.intercepted = true;

				unsigned int vt = sym + 1 - XKB_KEY_XF86Switch_VT_1;
				wlr_session_change_vt(layer->session, vt);
			}
		}

		if (intercepted) {
			copy = *event;
			copy.intercepted = true;
			event = &copy;
		}
	}

	wlr_input_router_keyboard_handler_relay_key(handler, event);
}

static const struct wlr_input_router_keyboard_handler_interface keyboard_handler_impl = {
	.base = {
		.name = "wlr_session_input_router_layer",
	},
	.set_device = keyboard_handler_set_device,
	.key = keyboard_handler_key,
};

static void handle_router_destroy(struct wl_listener *listener, void *data) {
	struct wlr_session_input_router_layer *layer =
		wl_container_of(listener, layer, router_destroy);
	wlr_session_input_router_layer_destroy(layer);
}

static void handle_session_destroy(struct wl_listener *listener, void *data) {
	struct wlr_session_input_router_layer *layer =
		wl_container_of(listener, layer, session_destroy);
	wlr_session_input_router_layer_destroy(layer);
}

static void handle_device_destroy(struct wl_listener *listener, void *data) {
	struct wlr_session_input_router_layer *layer =
		wl_container_of(listener, layer, device_destroy);
	set_device(layer, NULL);
}

bool wlr_session_input_router_layer_register(int32_t priority) {
	if (!wlr_input_router_register_keyboard_handler_interface(&keyboard_handler_impl, priority)) {
		return false;
	}
	return true;
}

struct wlr_session_input_router_layer *wlr_session_input_router_layer_create(
		struct wlr_input_router *router, struct wlr_session *session) {
	struct wlr_session_input_router_layer *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_input_router_keyboard_handler_init(&layer->keyboard_handler,
		router, &keyboard_handler_impl);

	layer->router = router;
	layer->router_destroy.notify = handle_router_destroy;
	wl_signal_add(&router->events.destroy, &layer->router_destroy);

	layer->session = session;
	layer->session_destroy.notify = handle_session_destroy;
	wl_signal_add(&session->events.destroy, &layer->session_destroy);

	layer->device_destroy.notify = handle_device_destroy;
	wl_list_init(&layer->device_destroy.link);

	wl_signal_init(&layer->events.destroy);

	wlr_input_router_invalidate_keyboard_device(router);

	return layer;
}

void wlr_session_input_router_layer_destroy(struct wlr_session_input_router_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	wlr_input_router_keyboard_handler_finish(&layer->keyboard_handler);

	wl_list_remove(&layer->router_destroy.link);
	wl_list_remove(&layer->session_destroy.link);
	wl_list_remove(&layer->device_destroy.link);

	free(layer);
}
