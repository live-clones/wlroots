#include <wlr/types/wlr_input_router.h>
#include <wlr/util/log.h>

static struct wlr_input_router_handler_priority_list pointer_priority_list = {0};

uint32_t wlr_input_router_pointer_notify_position(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_position_event *event) {
	while ((pointer = wl_container_of(pointer->base.next, pointer, base)) != NULL) {
		pointer->x = event->x;
		pointer->y = event->y;
		wlr_input_router_focus_copy(&pointer->focus, event->focus);

		if (pointer->impl->position != NULL) {
			return pointer->impl->position(pointer, event);
		}
	}
	return 0;
}

uint32_t wlr_input_router_pointer_notify_button(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_button_event *event) {
	while ((pointer = wl_container_of(pointer->base.next, pointer, base)) != NULL) {
		struct wlr_input_router_pointer_button *button = NULL;
		for (size_t i = 0; i < pointer->n_buttons; i++) {
			if (pointer->buttons[i].button == event->button) {
				button = &pointer->buttons[i];
				break;
			}
		}

		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
			if (button == NULL) {
				if (pointer->n_buttons == WLR_INPUT_ROUTER_MAX_POINTER_BUTTONS) {
					wlr_log(WLR_ERROR, "%s has too many pressed buttons, ignoring %"PRIi32,
						pointer->impl->base.name, event->button);
					return 0;
				}
				button = &pointer->buttons[pointer->n_buttons++];
				*button = (struct wlr_input_router_pointer_button){
					.button = event->button,
				};
			}
			++button->count;
			if (button->count != 1) {
				// Already pressed
				return 0;
			}
		} else {
			if (button == NULL) {
				wlr_log(WLR_ERROR, "%s received a release for a non-pressed button %"PRIi32,
					pointer->impl->base.name, event->button);
				return 0;
			}
			--button->count;
			if (button->count != 0) {
				// Still pressed
				return 0;
			}
			*button = pointer->buttons[--pointer->n_buttons];
		}

		if (pointer->impl->button != NULL) {
			struct wlr_input_router_pointer_button_event relayed = *event;
			relayed.index = button - pointer->buttons;
			return pointer->impl->button(pointer, &relayed);
		}
	}
	return 0;
}

void wlr_input_router_pointer_notify_axis(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_axis_event *event) {
	while ((pointer = wl_container_of(pointer->base.next, pointer, base)) != NULL) {
		if (pointer->impl->axis != NULL) {
			pointer->impl->axis(pointer, event);
		}
	}
}

void wlr_input_router_pointer_notify_frame(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_frame_event *event) {
	while ((pointer = wl_container_of(pointer->base.next, pointer, base)) != NULL) {
		if (pointer->impl->frame != NULL) {
			pointer->impl->frame(pointer, event);
			return;
		}
	}
}

uint32_t wlr_input_router_pointer_refresh_position(struct wlr_input_router_pointer *pointer) {
	return wlr_input_router_pointer_notify_position(pointer,
		&(struct wlr_input_router_pointer_position_event){
			.x = pointer->x,
			.y = pointer->y,
			.focus = &pointer->focus,
			.synthetic = true,
		});
}

uint32_t wlr_input_router_pointer_clear_focus(struct wlr_input_router_pointer *pointer) {
	return wlr_input_router_pointer_notify_position(pointer,
		&(struct wlr_input_router_pointer_position_event){
			.x = pointer->x,
			.y = pointer->y,
			.focus = NULL,
			.explicit_focus = true,
			.synthetic = true,
		});
}

bool wlr_input_router_pointer_register_interface(
		const struct wlr_input_router_pointer_interface *iface, int32_t priority) {
	return wlr_input_router_register_handler_interface(&iface->base,
		priority, &pointer_priority_list);
}

void wlr_input_router_pointer_init(struct wlr_input_router_pointer *pointer,
		struct wlr_input_router *router, const struct wlr_input_router_pointer_interface *impl) {
	*pointer = (struct wlr_input_router_pointer){
		.impl = impl,
	};
	wlr_input_router_handler_init(&pointer->base, &router->pointer.base,
		&impl->base, &pointer_priority_list);
	wlr_input_router_focus_init(&pointer->focus);

	struct wlr_input_router_pointer *next = wl_container_of(pointer->base.next, next, base);
	if (next != NULL) {
		pointer->x = next->x;
		pointer->y = next->y;
		wlr_input_router_focus_copy(&pointer->focus, &next->focus);
		for (size_t i = 0; i < next->n_buttons; i++) {
			pointer->buttons[i] = next->buttons[i];
		}
		pointer->n_buttons = next->n_buttons;
	}
}

void wlr_input_router_pointer_finish(struct wlr_input_router_pointer *pointer) {
	wlr_input_router_focus_finish(&pointer->focus);
	wlr_input_router_handler_finish(&pointer->base);
}
