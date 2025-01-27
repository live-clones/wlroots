#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>

// TODO: better xdg_popup grab state management

static const struct wlr_input_router_focus *filter_focus(
		struct wlr_xdg_popup_grab_input_router_layer *layer,
		const struct wlr_input_router_focus *focus) {
	struct wlr_surface *surface = wlr_input_router_focus_get_surface(focus);
	return surface != NULL && wl_resource_get_client(surface->resource) ==
		wl_resource_get_client(layer->popup->resource) ? focus : NULL;
}

static void dismiss_grab(struct wlr_xdg_popup_grab_input_router_layer *layer) {
	struct wlr_xdg_popup *popup = layer->popup;
	wlr_xdg_popup_grab_input_router_layer_destroy(layer);

	// Find the grabbing popup chain start
	struct wlr_surface *parent;
	while ((parent = popup->parent) != NULL) {
		struct wlr_xdg_popup *parent_popup = wlr_xdg_popup_try_from_wlr_surface(parent);
		// Note: wlr_xdg_popup.seat is non-NULL if it's a grabbing popup
		if (parent_popup == NULL || parent_popup->seat == NULL) {
			break;
		}
		popup = parent_popup;
	}

	wlr_xdg_popup_destroy(popup);
}

static uint32_t keyboard_focus(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_focus_event *event) {
	struct wlr_xdg_popup_grab_input_router_layer *layer =
		wl_container_of(keyboard, layer, keyboard);

	struct wlr_input_router_keyboard_focus_event relayed = *event;
	relayed.focus = &layer->keyboard_focus;
	return wlr_input_router_keyboard_notify_focus(keyboard, &relayed);
}

static const struct wlr_input_router_keyboard_interface keyboard_impl = {
	.base = {
		.name = "wlr_xdg_popup_grab_input_router_layer-keyboard",
	},
	.focus = keyboard_focus,
};

static uint32_t pointer_position(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_position_event *event) {
	struct wlr_xdg_popup_grab_input_router_layer *layer = wl_container_of(pointer, layer, pointer);

	struct wlr_input_router_pointer_position_event relayed = *event;
	relayed.focus = event->explicit_focus ? event->focus : filter_focus(layer, event->focus);
	relayed.explicit_focus = true;
	return wlr_input_router_pointer_notify_position(pointer, &relayed);
}

static uint32_t pointer_button(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_button_event *event) {
	struct wlr_xdg_popup_grab_input_router_layer *layer = wl_container_of(pointer, layer, pointer);
	uint32_t serial = wlr_input_router_pointer_notify_button(pointer, event);

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED &&
			filter_focus(layer, &pointer->focus) == NULL) {
		dismiss_grab(layer);
	}

	return serial;
}

static const struct wlr_input_router_pointer_interface pointer_impl = {
	.base = {
		.name = "wlr_xdg_popup_grab_input_router_layer-pointer",
	},
	.position = pointer_position,
	.button = pointer_button,
};

static void touch_position(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_position_event *event) {
	struct wlr_xdg_popup_grab_input_router_layer *layer = wl_container_of(touch, layer, touch);

	struct wlr_input_router_touch_position_event relayed = *event;
	relayed.focus = filter_focus(layer, event->focus);
	wlr_input_router_touch_notify_position(touch, &relayed);
}

static uint32_t touch_down(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_down_event *event) {
	struct wlr_xdg_popup_grab_input_router_layer *layer = wl_container_of(touch, layer, touch);

	struct wlr_input_router_touch_down_event relayed = *event;
	relayed.focus = filter_focus(layer, event->focus);
	uint32_t serial = wlr_input_router_touch_notify_down(touch, &relayed);

	if (relayed.focus == NULL) {
		dismiss_grab(layer);
	}

	return serial;
}

static const struct wlr_input_router_touch_interface touch_impl = {
	.base = {
		.name = "wlr_xdg_popup_grab_input_router_layer-touch",
	},
	.position = touch_position,
	.down = touch_down,
};

static void set_topmost_popup(struct wlr_xdg_popup_grab_input_router_layer *layer,
		struct wlr_xdg_popup *popup) {
	layer->popup = popup;

	wl_list_remove(&layer->popup_destroy.link);
	wl_signal_add(&popup->events.destroy, &layer->popup_destroy);

	wlr_input_router_focus_set_surface(&layer->keyboard_focus, popup->base->surface);
	wlr_input_router_keyboard_notify_focus(&layer->keyboard,
		&(struct wlr_input_router_keyboard_focus_event){
			.focus = &layer->keyboard_focus,
		});
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

static void handle_popup_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup_grab_input_router_layer *layer =
		wl_container_of(listener, layer, popup_destroy);
	struct wlr_surface *parent = layer->popup->parent;
	if (parent != NULL) {
		struct wlr_xdg_popup *parent_popup = wlr_xdg_popup_try_from_wlr_surface(parent);
		// Note: wlr_xdg_popup.seat is non-NULL if it's a grabbing popup
		if (parent_popup != NULL && parent_popup->seat != NULL) {
			set_topmost_popup(layer, parent_popup);
			return;
		}
	}

	wlr_xdg_popup_grab_input_router_layer_destroy(layer);
}

bool wlr_xdg_popup_grab_input_router_layer_register(int32_t priority) {
	if (!wlr_input_router_keyboard_register_interface(&keyboard_impl, priority)) {
		return false;
	}
	if (!wlr_input_router_pointer_register_interface(&pointer_impl, priority)) {
		return false;
	}
	if (!wlr_input_router_touch_register_interface(&touch_impl, priority)) {
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

	wlr_input_router_keyboard_init(&layer->keyboard,
		router, &keyboard_impl);
	wlr_input_router_focus_init(&layer->keyboard_focus);

	wlr_input_router_pointer_init(&layer->pointer, router, &pointer_impl);

	wlr_input_router_touch_init(&layer->touch, router, &touch_impl);

	layer->popup = popup;
	layer->popup_destroy.notify = handle_popup_destroy;
	wl_list_init(&layer->popup_destroy.link);

	layer->router = router;
	wlr_addon_init(&layer->router_addon, &router->addons, NULL, &router_addon_impl);

	wl_signal_init(&layer->events.destroy);

	set_topmost_popup(layer, popup);

	wlr_input_router_pointer_clear_focus(&layer->pointer);

	return layer;
}

void wlr_xdg_popup_grab_input_router_layer_destroy(
		struct wlr_xdg_popup_grab_input_router_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	assert(wl_list_empty(&layer->events.destroy.listener_list));

	wlr_input_router_focus_finish(&layer->keyboard_focus);

	wlr_input_router_keyboard_notify_focus(&layer->keyboard,
		&(struct wlr_input_router_keyboard_focus_event){
			.focus = &layer->keyboard.focus,
		});
	wlr_input_router_keyboard_finish(&layer->keyboard);

	wlr_input_router_pointer_refresh_position(&layer->pointer);
	wlr_input_router_pointer_finish(&layer->pointer);

	wlr_input_router_touch_finish(&layer->touch);

	wlr_addon_finish(&layer->router_addon);
	wl_list_remove(&layer->popup_destroy.link);

	free(layer);
}
