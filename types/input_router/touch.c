#include <wlr/types/wlr_input_router.h>

static const struct wlr_input_router_focus none_focus = {
	.type = WLR_INPUT_ROUTER_FOCUS_NONE,
};

static struct wlr_input_router_handler_priority_list touch_priority_list = {0};

static struct wlr_input_router_touch_handler *get_handler(
		struct wlr_input_router_handler *base_handler, uintptr_t func_offset) {
	for (; base_handler != NULL; base_handler = base_handler->next) {
		struct wlr_input_router_touch_handler *handler =
			wl_container_of(base_handler, handler, base);
		if (*(void **)((const char *)handler->impl + func_offset) != NULL) {
			return handler;
		}
	}
	return NULL;
}

static void touch_handler_position(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_touch_position *event) {
	struct wlr_input_router_touch_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_touch_handler_interface, position));
	if (handler != NULL) {
		struct wlr_input_router_touch_position copy;
		if (event->focus == NULL) {
			copy = *event;
			copy.focus = &none_focus;
			event = &copy;
		}
		handler->impl->position(handler, event);
	}
}

void wlr_input_router_touch_handler_relay_position(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_position *event) {
	return touch_handler_position(handler->base.next, event);
}

void wlr_input_router_notify_touch_position(struct wlr_input_router *router,
		const struct wlr_input_router_touch_position *event) {
	return touch_handler_position(router->touch_chain.head, event);
}

static uint32_t touch_handler_down(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_touch_down *event) {
	struct wlr_input_router_touch_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_touch_handler_interface, down));
	if (handler != NULL) {
		struct wlr_input_router_touch_down copy;
		if (event->focus == NULL) {
			copy = *event;
			copy.focus = &none_focus;
			event = &copy;
		}
		return handler->impl->down(handler, event);
	}
	return 0;
}

uint32_t wlr_input_router_touch_handler_relay_down(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_down *event) {
	return touch_handler_down(handler->base.next, event);
}

uint32_t wlr_input_router_notify_touch_down(struct wlr_input_router *router,
		const struct wlr_input_router_touch_down *event) {
	return touch_handler_down(router->touch_chain.head, event);
}

static uint32_t touch_handler_up(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_touch_up *event) {
	struct wlr_input_router_touch_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_touch_handler_interface, up));
	if (handler != NULL) {
		return handler->impl->up(handler, event);
	}
	return 0;
}

uint32_t wlr_input_router_touch_handler_relay_up(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_up *event) {
	return touch_handler_up(handler->base.next, event);
}

uint32_t wlr_input_router_notify_touch_up(struct wlr_input_router *router,
		const struct wlr_input_router_touch_up *event) {
	return touch_handler_up(router->touch_chain.head, event);
}

static void touch_handler_cancel(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_touch_cancel *event) {
	struct wlr_input_router_touch_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_touch_handler_interface, cancel));
	if (handler != NULL) {
		handler->impl->cancel(handler, event);
	}
}

void wlr_input_router_touch_handler_relay_cancel(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_cancel *event) {
	touch_handler_cancel(handler->base.next, event);
}

void wlr_input_router_notify_touch_cancel(struct wlr_input_router *router,
		const struct wlr_input_router_touch_cancel *event) {
	touch_handler_cancel(router->touch_chain.head, event);
}

static void touch_handler_frame(struct wlr_input_router_handler *base_handler,
		const struct wlr_input_router_touch_frame *event) {
	struct wlr_input_router_touch_handler *handler = get_handler(base_handler,
		offsetof(struct wlr_input_router_touch_handler_interface, frame));
	if (handler != NULL) {
		handler->impl->frame(handler, event);
	}
}

void wlr_input_router_touch_handler_relay_frame(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_frame *event) {
	touch_handler_frame(handler->base.next, event);
}

void wlr_input_router_notify_touch_frame(struct wlr_input_router *router,
		const struct wlr_input_router_touch_frame *event) {
	touch_handler_frame(router->touch_chain.head, event);
}

bool wlr_input_router_register_touch_handler_interface(
		const struct wlr_input_router_touch_handler_interface *iface, int32_t priority) {
	return wlr_input_router_register_handler_interface(&iface->base,
		priority, &touch_priority_list);
}

void wlr_input_router_touch_handler_init(
		struct wlr_input_router_touch_handler *handler, struct wlr_input_router *router,
		const struct wlr_input_router_touch_handler_interface *impl) {
	*handler = (struct wlr_input_router_touch_handler){
		.impl = impl,
	};
	wlr_input_router_handler_init(&handler->base, &router->touch_chain,
		&impl->base, &touch_priority_list);
}

void wlr_input_router_touch_handler_finish(struct wlr_input_router_touch_handler *handler) {
	wlr_input_router_handler_finish(&handler->base);
}
