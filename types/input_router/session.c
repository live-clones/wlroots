#include <assert.h>
#include <stdlib.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

static uint32_t keyboard_key(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_key_event *event) {
	struct wlr_session_input_router_layer *layer = wl_container_of(keyboard, layer, keyboard);

	struct xkb_state *xkb_state = layer->keyboard.device->xkb_state;

	struct wlr_input_router_keyboard_key_event copy;
	if (xkb_state != NULL) {
		bool intercepted = false;

		const xkb_keysym_t *syms;
		int n_syms = xkb_state_key_get_syms(xkb_state, event->key + 8, &syms);
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

	return wlr_input_router_keyboard_notify_key(keyboard, event);
}

static const struct wlr_input_router_keyboard_interface keyboard_impl = {
	.base = {
		.name = "wlr_session_input_router_layer-keyboard",
	},
	.key = keyboard_key,
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

bool wlr_session_input_router_layer_register(int32_t priority) {
	if (!wlr_input_router_keyboard_register_interface(&keyboard_impl, priority)) {
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

	wlr_input_router_keyboard_init(&layer->keyboard,
		router, &keyboard_impl);

	layer->router = router;
	layer->router_destroy.notify = handle_router_destroy;
	wl_signal_add(&router->events.destroy, &layer->router_destroy);

	layer->session = session;
	layer->session_destroy.notify = handle_session_destroy;
	wl_signal_add(&session->events.destroy, &layer->session_destroy);

	wl_signal_init(&layer->events.destroy);

	return layer;
}

void wlr_session_input_router_layer_destroy(struct wlr_session_input_router_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	assert(wl_list_empty(&layer->events.destroy.listener_list));

	wlr_input_router_keyboard_finish(&layer->keyboard);

	wl_list_remove(&layer->router_destroy.link);
	wl_list_remove(&layer->session_destroy.link);

	free(layer);
}
