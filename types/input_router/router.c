#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/util/log.h>

struct wlr_input_router_handler_priority_entry {
	const struct wlr_input_router_handler_interface *iface;
	int32_t priority;
};

static const struct wlr_input_router_keyboard_interface keyboard_impl = {
	.base = {
		.name = "wlr_input_router-keyboard",
	},
};

static const struct wlr_input_router_pointer_interface pointer_impl = {
	.base = {
		.name = "wlr_input_router-pointer",
	},
};

static const struct wlr_input_router_touch_interface touch_impl = {
	.base = {
		.name = "wlr_input_router-touch",
	},
};

static void focus_handle_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_router_focus *focus = wl_container_of(listener, focus, destroy);
	wlr_input_router_focus_clear(focus);
}

static void focus_set_generic(struct wlr_input_router_focus *focus,
		enum wlr_input_router_focus_type type, struct wl_signal *destroy_signal) {
	focus->type = type;
	wl_list_remove(&focus->destroy.link);
	if (destroy_signal != NULL) {
		wl_signal_add(destroy_signal, &focus->destroy);
	} else {
		wl_list_init(&focus->destroy.link);
	}
}

bool wlr_input_router_register_handler_interface(
		const struct wlr_input_router_handler_interface *iface,
		int32_t priority, struct wlr_input_router_handler_priority_list *priority_list) {
	struct wlr_input_router_handler_priority_entry *entry;
	wl_array_for_each(entry, &priority_list->entries) {
		if (entry->iface == iface) {
			if (entry->priority == priority) {
				// Already registered with the same priority
				return true;
			}

			wlr_log(WLR_ERROR,
				"Tried to register an already registered input handler interface %s",
				entry->iface->name);
			abort();
		} else if (entry->priority == priority) {
			wlr_log(WLR_ERROR,
				"Tried to register a input handler interface %s with the same priority %"PRIi32
				" as %s",
				iface->name, priority, entry->iface->name);
			abort();
		}
	}

	entry = wl_array_add(&priority_list->entries, sizeof(*entry));
	if (entry == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return false;
	}
	*entry = (struct wlr_input_router_handler_priority_entry){
		.iface = iface,
		.priority = priority,
	};
	return true;
}

void wlr_input_router_handler_init(struct wlr_input_router_handler *handler,
		struct wlr_input_router_handler *head,
		const struct wlr_input_router_handler_interface *impl,
		const struct wlr_input_router_handler_priority_list *priority_list) {
	*handler = (struct wlr_input_router_handler){
		.head = head,
	};

	if (handler == head) {
		// Special case: initializing the top of the chain. Do nothing else.
		return;
	}

	bool found = false;
	struct wlr_input_router_handler_priority_entry *priority_entry;
	wl_array_for_each(priority_entry, &priority_list->entries) {
		if (priority_entry->iface == impl) {
			handler->priority = priority_entry->priority;
			found = true;
			break;
		}
	}
	if (!found) {
		wlr_log(WLR_ERROR,
			"Tried to init an input handler with unregistered interface %s", impl->name);
		abort();
	}

	struct wlr_input_router_handler **target_ptr = &head->next;
	struct wlr_input_router_handler *target;
	while (true) {
		target = *target_ptr;
		if (target == NULL) {
			break;
		}
		if (target->priority == handler->priority) {
			wlr_log(WLR_ERROR,
				"Tried to init an input handler with interface %s twice", impl->name);
			abort();
		} else if (handler->priority > target->priority) {
			break;
		}
		target_ptr = &target->next;
	}

	handler->next = target;
	*target_ptr = handler;
}

void wlr_input_router_handler_finish(struct wlr_input_router_handler *handler) {
	if (handler->head == handler) {
		// Special case: finalizing the top of the chain. Do nothing else.
		return;
	}

	struct wlr_input_router_handler **target_ptr = &handler->head->next;
	while (true) {
		struct wlr_input_router_handler *target = *target_ptr;
		if (target == handler) {
			*target_ptr = target->next;
			break;
		}
		target_ptr = &target->next;
	}
}

void wlr_input_router_at(struct wlr_input_router *router, double x, double y,
		struct wlr_input_router_focus *focus, double *local_x, double *local_y) {
	struct wlr_input_router_focus focus_placeholder;
	wlr_input_router_focus_init(&focus_placeholder);
	if (focus == NULL) {
		focus = &focus_placeholder;
	}

	double local_x_placeholder, local_y_placeholder;
	if (local_x == NULL) {
		local_x = &local_x_placeholder;
	}
	if (local_y == NULL) {
		local_y = &local_y_placeholder;
	}

	wlr_input_router_focus_clear(focus);
	*local_x = NAN;
	*local_y = NAN;

	if (router->impl->at != NULL) {
		router->impl->at(router, x, y, focus, local_x, local_y);
	}

	wlr_input_router_focus_finish(&focus_placeholder);
}

bool wlr_input_router_get_surface_position(struct wlr_input_router *router,
		struct wlr_surface *surface, double *x, double *y) {
	double x_placeholder, y_placeholder;
	if (x == NULL) {
		x = &x_placeholder;
	}
	if (y == NULL) {
		y = &y_placeholder;
	}

	*x = NAN;
	*y = NAN;

	if (router->impl->get_surface_position != NULL) {
		return router->impl->get_surface_position(router, surface, x, y);
	}
	return false;
}

void wlr_input_router_init(struct wlr_input_router *router,
		const struct wlr_input_router_interface *impl) {
	*router = (struct wlr_input_router){
		.impl = impl,
	};

	wlr_input_router_keyboard_init(&router->keyboard, router, &keyboard_impl);
	wlr_input_router_pointer_init(&router->pointer, router, &pointer_impl);
	wlr_input_router_touch_init(&router->touch, router, &touch_impl);

	wlr_addon_set_init(&router->addons);

	wl_signal_init(&router->events.destroy);
}

void wlr_input_router_finish(struct wlr_input_router *router) {
	wl_signal_emit_mutable(&router->events.destroy, NULL);

	wlr_addon_set_finish(&router->addons);

	assert(wl_list_empty(&router->events.destroy.listener_list));

	assert(router->keyboard.base.next == NULL);
	assert(router->pointer.base.next == NULL);
	assert(router->touch.base.next == NULL);

	wlr_input_router_keyboard_finish(&router->keyboard);
	wlr_input_router_pointer_finish(&router->pointer);
	wlr_input_router_touch_finish(&router->touch);
}

void wlr_input_router_focus_init(struct wlr_input_router_focus *focus) {
	*focus = (struct wlr_input_router_focus) {
		.type = WLR_INPUT_ROUTER_FOCUS_NONE,
		.destroy.notify = focus_handle_destroy,
	};
	wl_list_init(&focus->destroy.link);
}

void wlr_input_router_focus_finish(struct wlr_input_router_focus *focus) {
	wl_list_remove(&focus->destroy.link);
}

bool wlr_input_router_focus_is_none(const struct wlr_input_router_focus *focus) {
	return focus == NULL || focus->type == WLR_INPUT_ROUTER_FOCUS_NONE;
}

struct wlr_surface *wlr_input_router_focus_get_surface(
		const struct wlr_input_router_focus *focus) {
	return focus != NULL && focus->type == WLR_INPUT_ROUTER_FOCUS_SURFACE ? focus->surface : NULL;
}

void *wlr_input_router_focus_get_user(const struct wlr_input_router_focus *focus) {
	return focus != NULL && focus->type == WLR_INPUT_ROUTER_FOCUS_USER ? focus->user : NULL;
}

void wlr_input_router_focus_clear(struct wlr_input_router_focus *focus) {
	focus_set_generic(focus, WLR_INPUT_ROUTER_FOCUS_NONE, NULL);
}

void wlr_input_router_focus_set_surface(struct wlr_input_router_focus *focus,
		struct wlr_surface *surface) {
	if (surface != NULL) {
		focus_set_generic(focus, WLR_INPUT_ROUTER_FOCUS_SURFACE, &surface->events.destroy);
		focus->surface = surface;
	} else {
		wlr_input_router_focus_clear(focus);
	}
}

void wlr_input_router_focus_set_user(struct wlr_input_router_focus *focus,
		void *user, struct wl_signal *destroy_signal) {
	if (user != NULL) {
		focus_set_generic(focus, WLR_INPUT_ROUTER_FOCUS_USER, destroy_signal);
		focus->user = user;
		focus->destroy_signal = destroy_signal;
	} else {
		wlr_input_router_focus_clear(focus);
	}
}

void wlr_input_router_focus_copy(struct wlr_input_router_focus *dst,
		const struct wlr_input_router_focus *src) {
	if (src == NULL) {
		wlr_input_router_focus_clear(dst);
		return;
	}

	switch (src->type) {
	case WLR_INPUT_ROUTER_FOCUS_NONE:
		wlr_input_router_focus_clear(dst);
		break;
	case WLR_INPUT_ROUTER_FOCUS_SURFACE:
		wlr_input_router_focus_set_surface(dst, src->surface);
		break;
	case WLR_INPUT_ROUTER_FOCUS_USER:
		wlr_input_router_focus_set_user(dst, src->user, src->destroy_signal);
		break;
	}
}
