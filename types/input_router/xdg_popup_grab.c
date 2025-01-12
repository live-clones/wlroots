#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_input_router.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>

static bool is_same_client_focus(struct wlr_xdg_popup_grab_input_router_layer *layer,
		const struct wlr_input_router_focus *focus) {
	struct wlr_surface *surface = wlr_input_router_focus_get_surface(focus);
	return surface != NULL && wl_resource_get_client(surface->resource) ==
		wl_resource_get_client(layer->popup->resource);
}

static void keyboard_handler_set_focus(struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_set_focus *event) {
	struct wlr_xdg_popup_grab_input_router_layer *layer =
		wl_container_of(handler, layer, keyboard_handler);
	struct wlr_input_router_keyboard_set_focus relayed = *event;
	relayed.focus = &layer->keyboard_focus;
	wlr_input_router_keyboard_handler_relay_set_focus(handler, &relayed);
}

static const struct wlr_input_router_keyboard_handler_interface keyboard_handler_impl = {
	.base = {
		.name = "wlr_xdg_popup_grab_input_router_layer",
	},
	.set_focus = keyboard_handler_set_focus,
};

static void pointer_handler_position(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_position *event) {
	struct wlr_xdg_popup_grab_input_router_layer *layer =
		wl_container_of(handler, layer, pointer_handler);

	if (is_same_client_focus(layer, event->focus)) {
		wlr_input_router_focus_copy(&layer->pointer_focus, event->focus);
	} else {
		wlr_input_router_focus_clear(&layer->pointer_focus);
	}

	struct wlr_input_router_pointer_position relayed = *event;
	relayed.focus = &layer->pointer_focus;
	wlr_input_router_pointer_handler_relay_position(handler, &relayed);
}

static uint32_t pointer_handler_button(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_button *event) {
	struct wlr_xdg_popup_grab_input_router_layer *layer =
		wl_container_of(handler, layer, pointer_handler);
	uint32_t serial = wlr_input_router_pointer_handler_relay_button(handler, event);

	if (layer->pointer_focus.type == WLR_INPUT_ROUTER_FOCUS_NONE) {
		wlr_xdg_popup_grab_input_router_layer_destroy(layer);
	}

	return serial;
}

static const struct wlr_input_router_pointer_handler_interface pointer_handler_impl = {
	.base = {
		.name = "wlr_xdg_popup_grab_input_router_layer",
	},
	.position = pointer_handler_position,
	.button = pointer_handler_button,
};

static uint32_t touch_handler_down(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_down *event) {
	struct wlr_xdg_popup_grab_input_router_layer *layer =
		wl_container_of(handler, layer, touch_handler);

	if (is_same_client_focus(layer, event->focus)) {
		return wlr_input_router_touch_handler_relay_down(handler, event);
	}

	wlr_xdg_popup_grab_input_router_layer_destroy(layer);
	return 0;
}

static const struct wlr_input_router_touch_handler_interface touch_handler_impl = {
	.base = {
		.name = "wlr_xdg_popup_grab_input_router_layer",
	},
	.down = touch_handler_down,
};

static void set_topmost_popup(struct wlr_xdg_popup_grab_input_router_layer *layer,
		struct wlr_xdg_popup *popup) {
	layer->popup = popup;

	wl_list_remove(&layer->popup_reset.link);
	wl_signal_add(&popup->events.reset, &layer->popup_reset);

	wlr_input_router_focus_set_surface(&layer->keyboard_focus, popup->base->surface);
	wlr_input_router_keyboard_handler_relay_set_focus(&layer->keyboard_handler,
		&(struct wlr_input_router_keyboard_set_focus){
			.focus = &layer->keyboard_focus,
		});
}

static void layer_destroy(struct wlr_xdg_popup_grab_input_router_layer *layer) {
	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	wlr_input_router_keyboard_handler_finish(&layer->keyboard_handler);
	wlr_input_router_focus_finish(&layer->keyboard_focus);

	wlr_input_router_pointer_handler_finish(&layer->pointer_handler);
	wlr_input_router_focus_finish(&layer->pointer_focus);

	wlr_input_router_touch_handler_finish(&layer->touch_handler);

	wlr_input_router_invalidate_pointer_position(layer->router);

	wlr_addon_finish(&layer->router_addon);
	wl_list_remove(&layer->popup_reset.link);

	free(layer);
}

static void router_addon_destroy(struct wlr_addon *addon) {
	struct wlr_xdg_popup_grab_input_router_layer *layer =
		wl_container_of(addon, layer, router_addon);
	wlr_xdg_popup_grab_input_router_layer_destroy(layer);
}

static const struct wlr_addon_interface router_addon_impl = {
	.name = "wlr_xdg_popup_grab_input_router_layer",
	.destroy = router_addon_destroy,
};

static void handle_popup_reset(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup_grab_input_router_layer *layer =
		wl_container_of(listener, layer, popup_reset);
	struct wlr_surface *parent = layer->popup->parent;
	if (parent != NULL) {
		struct wlr_xdg_popup *parent_popup = wlr_xdg_popup_try_from_wlr_surface(parent);
		if (parent_popup != NULL && parent_popup->grabbing) {
			set_topmost_popup(layer, parent_popup);
			return;
		}
	}

	layer_destroy(layer);
}

bool wlr_xdg_popup_grab_input_router_layer_register(int32_t priority) {
	if (!wlr_input_router_register_keyboard_handler_interface(&keyboard_handler_impl, priority)) {
		return false;
	}
	if (!wlr_input_router_register_pointer_handler_interface(&pointer_handler_impl, priority)) {
		return false;
	}
	if (!wlr_input_router_register_touch_handler_interface(&touch_handler_impl, priority)) {
		return false;
	}
	return true;
}

struct wlr_xdg_popup_grab_input_router_layer *wlr_xdg_popup_grab_input_router_layer_get_or_create(
		struct wlr_input_router *router, struct wlr_xdg_popup *popup) {
	struct wlr_xdg_popup_grab_input_router_layer *layer;

	struct wlr_addon *addon = wlr_addon_find(&router->addons, NULL, &router_addon_impl);
	if (addon != NULL) {
		layer = wl_container_of(addon, layer, router_addon);
		set_topmost_popup(layer, popup);
		return layer;
	}

	layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_input_router_keyboard_handler_init(&layer->keyboard_handler,
		router, &keyboard_handler_impl);
	wlr_input_router_focus_init(&layer->keyboard_focus);

	wlr_input_router_pointer_handler_init(&layer->pointer_handler, router, &pointer_handler_impl);
	wlr_input_router_focus_init(&layer->pointer_focus);

	wlr_input_router_touch_handler_init(&layer->touch_handler, router, &touch_handler_impl);

	layer->popup = popup;
	layer->popup_reset.notify = handle_popup_reset;
	wl_list_init(&layer->popup_reset.link);

	layer->router = router;
	wlr_addon_init(&layer->router_addon, &router->addons, NULL, &router_addon_impl);

	wl_signal_init(&layer->events.destroy);

	set_topmost_popup(layer, popup);
	wlr_input_router_invalidate_pointer_position(router);

	return layer;
}

void wlr_xdg_popup_grab_input_router_layer_destroy(
		struct wlr_xdg_popup_grab_input_router_layer *layer) {
	if (layer == NULL) {
		return;
	}

	// Find the topmost grabbing popup
	struct wlr_xdg_popup *popup = layer->popup;
	while (popup->parent != NULL) {
		struct wlr_xdg_popup *parent_popup = wlr_xdg_popup_try_from_wlr_surface(popup->parent);
		if (parent_popup == NULL || !parent_popup->grabbing) {
			break;
		}
		popup = parent_popup;
	}

	// Avoid extra keyboard focus updates
	layer->popup = popup;
	wl_list_remove(&layer->popup_reset.link);
	wl_signal_add(&popup->events.reset, &layer->popup_reset);

	// Dismiss the topmost grabbing popup
	wlr_xdg_popup_destroy(popup);
}
