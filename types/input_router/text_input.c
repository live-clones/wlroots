#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_text_input_v3.h>
#include "wlr/util/log.h"

struct text_input {
	struct wlr_text_input_v3_input_router_layer *layer;
	struct wlr_text_input_v3 *wlr_text_input;

	struct wl_list link;

	struct wl_listener destroy;
	struct wl_listener enable;
	struct wl_listener disable;
};

// TODO: improve wlr_text_input_v3 focus API
static void text_input_safe_enter(struct text_input *text_input, struct wlr_surface *surface) {
	struct wlr_text_input_v3 *wlr_text_input = text_input->wlr_text_input;

	if (surface != NULL && wl_resource_get_client(wlr_text_input->resource) !=
			wl_resource_get_client(surface->resource)) {
		surface = NULL;
	}

	if (wlr_text_input->focused_surface == surface) {
		return;
	}

	if (wlr_text_input->focused_surface != NULL) {
		wlr_text_input_v3_send_leave(wlr_text_input);
	}
	if (surface != NULL) {
		wlr_text_input_v3_send_enter(wlr_text_input, surface);
	}
}

static void update_active_text_input(struct wlr_text_input_v3_input_router_layer *layer) {
	struct wlr_text_input_v3 *active_text_input = NULL;
	struct wlr_surface *surface = wlr_input_router_focus_get_surface(&layer->keyboard.focus);
	if (surface != NULL) {
		struct text_input *text_input;
		wl_list_for_each(text_input, &layer->text_inputs, link) {
			struct wlr_text_input_v3 *wlr_text_input = text_input->wlr_text_input;
			if (wlr_text_input->current_enabled && wlr_text_input->focused_surface == surface) {
				active_text_input = wlr_text_input;
				break;
			}
		}
	}

	if (layer->active_text_input == active_text_input) {
		return;
	}
	layer->active_text_input = active_text_input;

	struct wlr_text_input_v3_input_router_layer_set_active_event event = {
		.active_text_input = active_text_input,
	};
	wl_signal_emit_mutable(&layer->events.set_active_text_input, &event);
}

static void destroy_text_input(struct text_input *text_input) {
	wl_list_remove(&text_input->link);

	struct wlr_text_input_v3_input_router_layer *layer = text_input->layer;
	if (layer->active_text_input == text_input->wlr_text_input) {
		update_active_text_input(layer);
	}

	wl_list_remove(&text_input->destroy.link);
	wl_list_remove(&text_input->enable.link);
	wl_list_remove(&text_input->disable.link);
	free(text_input);
}

static void text_input_handle_destroy(struct wl_listener *listener, void *data) {
	struct text_input *text_input = wl_container_of(listener, text_input, destroy);
	destroy_text_input(text_input);
}

static void text_input_handle_enable(struct wl_listener *listener, void *data) {
	struct text_input *text_input = wl_container_of(listener, text_input, enable);
	struct wlr_text_input_v3_input_router_layer *layer = text_input->layer;
	if (layer->active_text_input == NULL) {
		update_active_text_input(layer);
	}
}

static void text_input_handle_disable(struct wl_listener *listener, void *data) {
	struct text_input *text_input = wl_container_of(listener, text_input, disable);
	struct wlr_text_input_v3_input_router_layer *layer = text_input->layer;
	if (layer->active_text_input == text_input->wlr_text_input) {
		update_active_text_input(layer);
	}
}

static void create_text_input(struct wlr_text_input_v3_input_router_layer *layer,
		struct wlr_text_input_v3 *wlr_text_input) {
	if (wlr_text_input->seat != layer->seat) {
		return;
	}

	struct text_input *text_input = calloc(1, sizeof(*text_input));
	if (text_input == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return;
	}

	text_input->wlr_text_input = wlr_text_input;
	text_input->layer = layer;

	wl_list_insert(&layer->text_inputs, &text_input->link);

	text_input->destroy.notify = text_input_handle_destroy;
	wl_signal_add(&wlr_text_input->events.destroy, &text_input->destroy);
	text_input->enable.notify = text_input_handle_enable;
	wl_signal_add(&wlr_text_input->events.enable, &text_input->enable);
	text_input->disable.notify = text_input_handle_disable;
	wl_signal_add(&wlr_text_input->events.disable, &text_input->disable);

	struct wlr_surface *surface = wlr_input_router_focus_get_surface(&layer->keyboard.focus);
	text_input_safe_enter(text_input, surface);
	update_active_text_input(layer);
}

static uint32_t keyboard_focus(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_focus_event *event) {
	struct wlr_text_input_v3_input_router_layer *layer =
		wl_container_of(keyboard, layer, keyboard);

	// Relay the event first; "the text-input focus follows the keyboard focus".
	uint32_t serial = wlr_input_router_keyboard_notify_focus(keyboard, event);

	struct wlr_surface *surface = wlr_input_router_focus_get_surface(event->focus);
	struct text_input *text_input;
	wl_list_for_each(text_input, &layer->text_inputs, link) {
		text_input_safe_enter(text_input, surface);
	}
	update_active_text_input(layer);

	return serial;
}

static const struct wlr_input_router_keyboard_interface keyboard_impl = {
	.base = {
		.name = "wlr_text_input_v3_input_router_layer-keyboard",
	},
	.focus = keyboard_focus,
};

static void handle_manager_destroy(struct wl_listener *listener, void *data) {
	struct wlr_text_input_v3_input_router_layer *layer =
		wl_container_of(listener, layer, manager_destroy);
	wlr_text_input_v3_input_router_layer_destroy(layer);
}

static void handle_manager_text_input(struct wl_listener *listener, void *data) {
	struct wlr_text_input_v3_input_router_layer *layer =
		wl_container_of(listener, layer, manager_text_input);
	struct wlr_text_input_v3 *wlr_text_input = data;
	create_text_input(layer, wlr_text_input);
}

static void handle_router_destroy(struct wl_listener *listener, void *data) {
	struct wlr_text_input_v3_input_router_layer *layer =
		wl_container_of(listener, layer, router_destroy);
	wlr_text_input_v3_input_router_layer_destroy(layer);
}

static void handle_seat_destroy(struct wl_listener *listener, void *data) {
	struct wlr_text_input_v3_input_router_layer *layer =
		wl_container_of(listener, layer, seat_destroy);
	wlr_text_input_v3_input_router_layer_destroy(layer);
}

bool wlr_text_input_v3_input_router_layer_register(int32_t priority) {
	if (!wlr_input_router_keyboard_register_interface(&keyboard_impl, priority)) {
		return false;
	}
	return true;
}

struct wlr_text_input_v3_input_router_layer *wlr_text_input_v3_input_router_layer_create(
		struct wlr_input_router *router, struct wlr_text_input_manager_v3 *manager,
		struct wlr_seat *seat) {
	struct wlr_text_input_v3_input_router_layer *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_input_router_keyboard_init(&layer->keyboard,
		router, &keyboard_impl);

	layer->manager = manager;
	layer->manager_destroy.notify = handle_manager_destroy;
	wl_signal_add(&manager->events.destroy, &layer->manager_destroy);
	layer->manager_text_input.notify = handle_manager_text_input;
	wl_signal_add(&manager->events.text_input, &layer->manager_text_input);

	layer->router = router;
	layer->router_destroy.notify = handle_router_destroy;
	wl_signal_add(&router->events.destroy, &layer->router_destroy);

	layer->seat = seat;
	layer->seat_destroy.notify = handle_seat_destroy;
	wl_signal_add(&seat->events.destroy, &layer->seat_destroy);

	wl_list_init(&layer->text_inputs);

	struct wlr_text_input_v3 *wlr_text_input;
	wl_list_for_each(wlr_text_input, &manager->text_inputs, link) {
		create_text_input(layer, wlr_text_input);
	}

	wl_signal_init(&layer->events.destroy);
	wl_signal_init(&layer->events.set_active_text_input);

	return layer;
}

void wlr_text_input_v3_input_router_layer_destroy(
		struct wlr_text_input_v3_input_router_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	assert(wl_list_empty(&layer->events.destroy.listener_list));
	assert(wl_list_empty(&layer->events.set_active_text_input.listener_list));

	layer->active_text_input = NULL;

	struct text_input *text_input, *tmp;
	wl_list_for_each_safe(text_input, tmp, &layer->text_inputs, link) {
		destroy_text_input(text_input);
	}

	wlr_input_router_keyboard_finish(&layer->keyboard);

	wl_list_remove(&layer->manager_destroy.link);
	wl_list_remove(&layer->manager_text_input.link);

	wl_list_remove(&layer->router_destroy.link);
	wl_list_remove(&layer->seat_destroy.link);

	free(layer);
}
