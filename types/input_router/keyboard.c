#include <wlr/types/wlr_input_router.h>
#include <wlr/util/log.h>

static struct wlr_input_router_handler_priority_list keyboard_priority_list = {0};

static void set_device(struct wlr_input_router_keyboard *keyboard, struct wlr_keyboard *device) {
	keyboard->device = device;
	wl_list_remove(&keyboard->device_destroy.link);
	if (device != NULL) {
		wl_signal_add(&device->base.events.destroy, &keyboard->device_destroy);
	} else {
		wl_list_init(&keyboard->device_destroy.link);
	}
}

static void handle_device_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_router_keyboard *keyboard =
		wl_container_of(listener, keyboard, device_destroy);
}

uint32_t wlr_input_router_keyboard_notify_focus(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_focus_event *event) {
	while ((keyboard = wl_container_of(keyboard->base.next, keyboard, base)) != NULL) {
		wlr_input_router_focus_copy(&keyboard->focus, event->focus);

		if (keyboard->impl->focus != NULL) {
			return keyboard->impl->focus(keyboard, event);
		}
	}
	return 0;
}

void wlr_input_router_keyboard_notify_device(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_device_event *event) {
	while ((keyboard = wl_container_of(keyboard->base.next, keyboard, base)) != NULL) {
		if (keyboard->device == event->device) {
			return;
		}
		set_device(keyboard, event->device);

		if (keyboard->impl->device != NULL) {
			keyboard->impl->device(keyboard, event);
			return;
		}
	}
}

uint32_t wlr_input_router_keyboard_notify_key(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_key_event *event) {
	while ((keyboard = wl_container_of(keyboard->base.next, keyboard, base)) != NULL) {
		if (keyboard->device == NULL) {
			wlr_log(WLR_ERROR, "%s received a key event without an active device",
				keyboard->impl->base.name);
			return 0;
		}

		if (keyboard->impl->key != NULL) {
			return keyboard->impl->key(keyboard, event);
		}
	}
	return 0;
}

void wlr_input_router_keyboard_notify_modifiers(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_modifiers_event *event) {
	while ((keyboard = wl_container_of(keyboard->base.next, keyboard, base)) != NULL) {
		if (keyboard->device == NULL) {
			wlr_log(WLR_ERROR, "%s received a modifiers event without an active device",
				keyboard->impl->base.name);
			return;
		}

		if (keyboard->impl->modifiers != NULL) {
			keyboard->impl->modifiers(keyboard, event);
			return;
		}
	}
}

bool wlr_input_router_keyboard_register_interface(
		const struct wlr_input_router_keyboard_interface *iface, int32_t priority) {
	return wlr_input_router_register_handler_interface(&iface->base,
		priority, &keyboard_priority_list);
}

void wlr_input_router_keyboard_init(
		struct wlr_input_router_keyboard *keyboard, struct wlr_input_router *router,
		const struct wlr_input_router_keyboard_interface *impl) {
	*keyboard = (struct wlr_input_router_keyboard){
		.impl = impl,
	};
	wlr_input_router_handler_init(&keyboard->base, &router->keyboard.base,
		&impl->base, &keyboard_priority_list);
	wlr_input_router_focus_init(&keyboard->focus);

	keyboard->device_destroy.notify = handle_device_destroy;
	wl_list_init(&keyboard->device_destroy.link);

	struct wlr_input_router_keyboard *next = wl_container_of(keyboard->base.next, next, base);
	if (next != NULL) {
		wlr_input_router_focus_copy(&keyboard->focus, &next->focus);
		set_device(keyboard, next->device);
	}
}

void wlr_input_router_keyboard_finish(struct wlr_input_router_keyboard *keyboard) {
	wlr_input_router_focus_finish(&keyboard->focus);
	wl_list_remove(&keyboard->device_destroy.link);
	wlr_input_router_handler_finish(&keyboard->base);
}
