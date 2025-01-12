#include <wlr/types/wlr_input_router.h>

static const struct wlr_input_router_focus none_focus = {
	.type = WLR_INPUT_ROUTER_FOCUS_NONE,
};

static struct wlr_input_router_handler_priority_list pointer_priority_list = {0};

static struct wlr_input_router_pointer_handler *get_handler(
		struct wlr_input_router_handler *base_handler, uintptr_t func_offset) {
	for (; base_handler != NULL; base_handler = base_handler->next) {
		struct wlr_input_router_pointer_handler *handler =
			wl_container_of(base_handler, handler, base);
		if (*(void **)((const char *)handler->impl + func_offset) != NULL) {
			return handler;
		}
	}
	return NULL;
}

static void pointer_handler_position(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_pointer_position *event) {
	struct wlr_input_router_pointer_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_pointer_handler_interface, position));
	if (handler != NULL) {
		struct wlr_input_router_pointer_position copy;
		if (event->focus == NULL) {
			copy = *event;
			copy.focus = &none_focus;
			event = &copy;
		}
		handler->impl->position(handler, event);
	}
}

void wlr_input_router_pointer_handler_relay_position(
		struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_position *event) {
	pointer_handler_position(handler->base.next, event);
}

void wlr_input_router_notify_pointer_position(struct wlr_input_router *router,
		const struct wlr_input_router_pointer_position *event) {
	pointer_handler_position(router->pointer_chain.head, event);
}

static void pointer_handler_clear_focus(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_pointer_clear_focus *event) {
	struct wlr_input_router_pointer_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_pointer_handler_interface, clear_focus));
	if (handler != NULL) {
		handler->impl->clear_focus(handler, event);
	}
}

void wlr_input_router_pointer_handler_relay_clear_focus(
		struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_clear_focus *event) {
	pointer_handler_clear_focus(handler->base.next, event);
}

void wlr_input_router_notify_pointer_clear_focus(struct wlr_input_router *router,
		const struct wlr_input_router_pointer_clear_focus *event) {
	pointer_handler_clear_focus(router->pointer_chain.head, event);
}

static uint32_t pointer_handler_button(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_pointer_button *event) {
	struct wlr_input_router_pointer_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_pointer_handler_interface, button));
	if (handler != NULL) {
		return handler->impl->button(handler, event);
	}
	return 0;
}

uint32_t wlr_input_router_pointer_handler_relay_button(
		struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_button *event) {
	return pointer_handler_button(handler->base.next, event);
}

uint32_t wlr_input_router_notify_pointer_button(struct wlr_input_router *router,
		const struct wlr_input_router_pointer_button *event) {
	return pointer_handler_button(router->pointer_chain.head, event);
}

static void pointer_handler_axis(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_pointer_axis *event) {
	struct wlr_input_router_pointer_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_pointer_handler_interface, axis));
	if (handler != NULL) {
		handler->impl->axis(handler, event);
	}
}

void wlr_input_router_pointer_handler_relay_axis(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_axis *event) {
	pointer_handler_axis(handler->base.next, event);
}

void wlr_input_router_notify_pointer_axis(struct wlr_input_router *router,
		const struct wlr_input_router_pointer_axis *event) {
	pointer_handler_axis(router->pointer_chain.head, event);
}

static void pointer_handler_frame(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_pointer_frame *event) {
	struct wlr_input_router_pointer_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_pointer_handler_interface, frame));
	if (handler != NULL) {
		handler->impl->frame(handler, event);
	}
}

void wlr_input_router_pointer_handler_relay_frame(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_frame *event) {
	pointer_handler_frame(handler->base.next, event);
}

void wlr_input_router_notify_pointer_frame(struct wlr_input_router *router,
		const struct wlr_input_router_pointer_frame *event) {
	pointer_handler_frame(router->pointer_chain.head, event);
}

bool wlr_input_router_register_pointer_handler_interface(
		const struct wlr_input_router_pointer_handler_interface *iface, int32_t priority) {
	return wlr_input_router_register_handler_interface(&iface->base,
		priority, &pointer_priority_list);
}

void wlr_input_router_pointer_handler_init(
		struct wlr_input_router_pointer_handler *handler, struct wlr_input_router *router,
		const struct wlr_input_router_pointer_handler_interface *impl) {
	*handler = (struct wlr_input_router_pointer_handler){
		.impl = impl,
	};
	wlr_input_router_handler_init(&handler->base, &router->pointer_chain,
		&impl->base, &pointer_priority_list);
}

void wlr_input_router_pointer_handler_finish(struct wlr_input_router_pointer_handler *handler) {
	wlr_input_router_handler_finish(&handler->base);
}
