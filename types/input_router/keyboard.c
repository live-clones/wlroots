#include <assert.h>
#include <wlr/types/wlr_input_router.h>

static const struct wlr_input_router_focus none_focus = {
	.type = WLR_INPUT_ROUTER_FOCUS_NONE,
};

static struct wlr_input_router_handler_priority_list keyboard_priority_list = {0};

static struct wlr_input_router_keyboard_handler *get_handler(
		struct wlr_input_router_handler *base_handler, uintptr_t func_offset) {
	for (; base_handler != NULL; base_handler = base_handler->next) {
		struct wlr_input_router_keyboard_handler *handler =
			wl_container_of(base_handler, handler, base);
		if (*(void **)((const char *)handler->impl + func_offset) != NULL) {
			return handler;
		}
	}
	return NULL;
}

static void keyboard_handler_set_focus(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_keyboard_set_focus *event) {
	struct wlr_input_router_keyboard_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_keyboard_handler_interface, set_focus));
	if (handler != NULL) {
		struct wlr_input_router_keyboard_set_focus copy;
		if (event->focus == NULL) {
			copy = *event;
			copy.focus = &none_focus;
			event = &copy;
		}
		handler->impl->set_focus(handler, event);
	}
}

void wlr_input_router_keyboard_handler_relay_set_focus(
		struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_set_focus *event) {
	keyboard_handler_set_focus(handler->base.next, event);
}

void wlr_input_router_notify_keyboard_set_focus(struct wlr_input_router *router,
		const struct wlr_input_router_keyboard_set_focus *event) {
	keyboard_handler_set_focus(router->keyboard_chain.head, event);
}

static void keyboard_handler_set_device(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_keyboard_set_device *event) {
	struct wlr_input_router_keyboard_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_keyboard_handler_interface, set_device));
	if (handler != NULL) {
		handler->impl->set_device(handler, event);
	}
}

void wlr_input_router_keyboard_handler_relay_set_device(
		struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_set_device *event) {
	keyboard_handler_set_device(handler->base.next, event);
}

void wlr_input_router_notify_keyboard_set_device(struct wlr_input_router *router,
		const struct wlr_input_router_keyboard_set_device *event) {
	keyboard_handler_set_device(router->keyboard_chain.head, event);
}

static void keyboard_handler_key(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_keyboard_key *event) {
	struct wlr_input_router_keyboard_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_keyboard_handler_interface, key));
	if (handler != NULL) {
		handler->impl->key(handler, event);
	}
}

void wlr_input_router_keyboard_handler_relay_key(
		struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_key *event) {
	keyboard_handler_key(handler->base.next, event);
}

void wlr_input_router_notify_keyboard_key(struct wlr_input_router *router,
		const struct wlr_input_router_keyboard_key *event) {
	keyboard_handler_key(router->keyboard_chain.head, event);
}

static void keyboard_handler_modifiers(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_keyboard_modifiers *event) {
	struct wlr_input_router_keyboard_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_keyboard_handler_interface, modifiers));
	if (handler != NULL) {
		handler->impl->modifiers(handler, event);
	}
}

void wlr_input_router_keyboard_handler_relay_modifiers(
		struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_modifiers *event) {
	keyboard_handler_modifiers(handler->base.next, event);
}

void wlr_input_router_notify_keyboard_modifiers(struct wlr_input_router *router,
		const struct wlr_input_router_keyboard_modifiers *event) {
	keyboard_handler_modifiers(router->keyboard_chain.head, event);
}

bool wlr_input_router_register_keyboard_handler_interface(
		const struct wlr_input_router_keyboard_handler_interface *iface, int32_t priority) {
	return wlr_input_router_register_handler_interface(&iface->base,
		priority, &keyboard_priority_list);
}

void wlr_input_router_keyboard_handler_init(
		struct wlr_input_router_keyboard_handler *handler, struct wlr_input_router *router,
		const struct wlr_input_router_keyboard_handler_interface *impl) {
	*handler = (struct wlr_input_router_keyboard_handler){
		.impl = impl,
	};
	wlr_input_router_handler_init(&handler->base, &router->keyboard_chain,
		&impl->base, &keyboard_priority_list);
}

void wlr_input_router_keyboard_handler_finish(struct wlr_input_router_keyboard_handler *handler) {
	wlr_input_router_handler_finish(&handler->base);
}
