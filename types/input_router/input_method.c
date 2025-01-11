#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_input_router.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/util/log.h>

struct text_input {
	struct wlr_input_method_v2_input_router_layer *layer;
	struct wlr_text_input_v3 *wlr_text_input;

	struct wl_list link;

	struct wl_listener destroy;
	struct wl_listener enable;
	struct wl_listener disable;
	struct wl_listener commit;
};

static void update_device_grab(struct wlr_input_method_v2_input_router_layer *layer) {
	layer->device_grabbed = false;
	struct wlr_keyboard *device = layer->keyboard.device;
	if (layer->grab == NULL || device == NULL) {
		return;
	}

	struct wlr_virtual_keyboard_v1 *virtual_device =
		wlr_input_device_get_virtual_keyboard(&device->base);
	if (virtual_device != NULL && wl_resource_get_client(virtual_device->resource) ==
			wl_resource_get_client(layer->grab->resource)) {
		// The current device is a virtual keyboard created by the input method
		// client to relay events to other clients, don't grab it
		return;
	}

	layer->device_grabbed = true;
	if (layer->grab->keyboard != device) {
		wlr_input_method_keyboard_grab_v2_set_keyboard(layer->grab, device);

		// We can't relay the effective keyboard state without
		// zwp_input_method_keyboard_grab_v2.key.
		layer->n_forwarded_keys = 0;
	}
}

// Returns true if needs wlr_input_method_v2_send_done() afterwards
static bool update_input_method_active(struct wlr_input_method_v2_input_router_layer *layer) {
	assert(layer->input_method != NULL);

	bool active = layer->active_text_input != NULL;
	if (layer->input_method->active != active) {
		if (active) {
			wlr_input_method_v2_send_activate(layer->input_method);
		} else {
			wlr_input_method_v2_send_deactivate(layer->input_method);
		}
		return true;
	}

	return false;
}

// Returns true if needs wlr_input_method_v2_send_done() afterwards
static bool active_text_input_state(struct wlr_input_method_v2_input_router_layer *layer,
		bool send_full) {
	assert(layer->input_method != NULL);

	struct wlr_text_input_v3 *text_input = layer->active_text_input;
	if (text_input == NULL) {
		return false;
	}
	struct wlr_text_input_v3_state *state = &text_input->current;

	bool sent = false;
	if (send_full) {
		if (text_input->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
			wlr_input_method_v2_send_surrounding_text(layer->input_method,
				state->surrounding.text, state->surrounding.cursor, state->surrounding.anchor);
			sent = true;
		}
		if (text_input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
			wlr_input_method_v2_send_content_type(layer->input_method,
				state->content_type.hint, state->content_type.purpose);
			sent = true;
		}
	} else {
		if (state->features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
			wlr_input_method_v2_send_surrounding_text(layer->input_method,
				state->surrounding.text, state->surrounding.cursor, state->surrounding.anchor);
			sent = true;
		}

		// TODO: add proper wlr_text_input_v3 state field bitmask
		if (true) {
			wlr_input_method_v2_send_text_change_cause(layer->input_method,
				state->text_change_cause);
			sent = true;
		}

		if (state->features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
			wlr_input_method_v2_send_content_type(layer->input_method,
				state->content_type.hint, state->content_type.purpose);
			sent = true;
		}
	}

	return sent;
}

static void set_grab(struct wlr_input_method_v2_input_router_layer *layer,
		struct wlr_input_method_keyboard_grab_v2 *grab) {
	if (layer->grab == grab) {
		return;
	}

	layer->grab = grab;
	wl_list_remove(&layer->grab_destroy.link);
	if (grab != NULL) {
		wl_signal_add(&grab->events.destroy, &layer->grab_destroy);
	} else {
		wl_list_init(&layer->grab_destroy.link);
	}

	update_device_grab(layer);
}

static void keyboard_device(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_device_event *event) {
	struct wlr_input_method_v2_input_router_layer *layer =
		wl_container_of(keyboard, layer, keyboard);
	update_device_grab(layer);

	if (layer->device_grabbed) {
		return;
	}

	wlr_input_router_keyboard_notify_device(keyboard, event);
}

static uint32_t keyboard_key(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_key_event *event) {
	struct wlr_input_method_v2_input_router_layer *layer =
		wl_container_of(keyboard, layer, keyboard);
	if (layer->device_grabbed) {
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			// We can't relay the effective keyboard state without
			// zwp_input_method_keyboard_grab_v2.key, so the event is dropped.
			if (event->intercepted) {
				return 0;
			}
			assert(layer->n_forwarded_keys < WLR_KEYBOARD_KEYS_CAP);
			layer->forwarded_keys[layer->n_forwarded_keys++] = event->key;
		} else {
			bool found = false;
			for (size_t i = 0; i < layer->n_forwarded_keys; i++) {
				if (layer->forwarded_keys[i] == event->key) {
					layer->forwarded_keys[i] = layer->forwarded_keys[--layer->n_forwarded_keys];
					found = true;
					break;
				}
			}
			// We can't relay the effective keyboard state without
			// zwp_input_method_keyboard_grab_v2.key, so intercepted releases of
			// relayed pressed keys are sent too.
			if (!found) {
				return 0;
			}
		}

		wlr_input_method_keyboard_grab_v2_send_key(layer->grab,
			event->time_msec, event->key, event->state);
		return 0;
	}

	return wlr_input_router_keyboard_notify_key(keyboard, event);
}

static void keyboard_modifiers(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_modifiers_event *event) {
	struct wlr_input_method_v2_input_router_layer *layer =
		wl_container_of(keyboard, layer, keyboard);
	if (layer->device_grabbed) {
		wlr_input_method_keyboard_grab_v2_send_modifiers(layer->grab,
			&keyboard->device->modifiers);
		return;
	}

	wlr_input_router_keyboard_notify_modifiers(keyboard, event);
}

static const struct wlr_input_router_keyboard_interface keyboard_impl = {
	.base = {
		.name = "wlr_input_method_v2_input_router_layer-keyboard",
	},
	.device = keyboard_device,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
};

static void handle_active_text_input_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_method_v2_input_router_layer *layer =
		wl_container_of(listener, layer, active_text_input_destroy);
	wlr_input_method_v2_input_router_layer_set_active_text_input(layer, NULL);
}

static void handle_active_text_input_commit(struct wl_listener *listener, void *data) {
	struct wlr_input_method_v2_input_router_layer *layer =
		wl_container_of(listener, layer, active_text_input_commit);
	if (layer->input_method != NULL && active_text_input_state(layer, false)) {
		wlr_input_method_v2_send_done(layer->input_method);
	}
}

static void handle_input_method_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_method_v2_input_router_layer *layer =
		wl_container_of(listener, layer, input_method_destroy);
	wlr_input_method_v2_input_router_layer_set_input_method(layer, NULL);
}

static void handle_input_method_commit(struct wl_listener *listener, void *data) {
	struct wlr_input_method_v2_input_router_layer *layer =
		wl_container_of(listener, layer, input_method_commit);
	struct wlr_text_input_v3 *text_input = layer->active_text_input;
	if (text_input == NULL) {
		return;
	}

	struct wlr_input_method_v2_state *state = &layer->input_method->current;
	bool sent = false;

	if (state->commit_text != NULL) {
		wlr_text_input_v3_send_commit_string(text_input, state->commit_text);
		sent = true;
	}
	if (state->delete.before_length != 0 || state->delete.after_length != 0) {
		wlr_text_input_v3_send_delete_surrounding_text(text_input,
			state->delete.before_length, state->delete.after_length);
		sent = true;
	}
	if (state->preedit.text != NULL) {
		wlr_text_input_v3_send_preedit_string(text_input, state->preedit.text,
			state->preedit.cursor_begin, state->preedit.cursor_end);
		sent = true;
	}

	if (sent) {
		wlr_text_input_v3_send_done(text_input);
	}
}

static void handle_input_method_grab_keyboard(struct wl_listener *listener, void *data) {
	struct wlr_input_method_v2_input_router_layer *layer =
		wl_container_of(listener, layer, input_method_grab_keyboard);
	struct wlr_input_method_keyboard_grab_v2 *grab = data;
	set_grab(layer, grab);
}

static void handle_router_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_method_v2_input_router_layer *layer =
		wl_container_of(listener, layer, router_destroy);
	wlr_input_method_v2_input_router_layer_destroy(layer);
}

static void handle_grab_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_method_v2_input_router_layer *layer =
		wl_container_of(listener, layer, grab_destroy);
	set_grab(layer, NULL);
}

void wlr_input_method_v2_input_router_layer_set_input_method(
		struct wlr_input_method_v2_input_router_layer *layer,
		struct wlr_input_method_v2 *input_method) {
	if (layer->input_method == input_method) {
		return;
	}

	if (layer->input_method != NULL && layer->input_method->active) {
		// XXX: this shouldn't be possible actually
		wlr_input_method_v2_send_deactivate(layer->input_method);
		wlr_input_method_v2_send_done(layer->input_method);
	}

	layer->input_method = input_method;

	wl_list_remove(&layer->input_method_destroy.link);
	wl_list_remove(&layer->input_method_commit.link);
	wl_list_remove(&layer->input_method_grab_keyboard.link);

	if (input_method != NULL) {
		wl_signal_add(&input_method->events.destroy, &layer->input_method_destroy);
		wl_signal_add(&input_method->events.commit, &layer->input_method_commit);
		wl_signal_add(&input_method->events.grab_keyboard, &layer->input_method_grab_keyboard);

		if (update_input_method_active(layer)) {
			wlr_input_method_v2_send_done(layer->input_method);
		}
	} else {
		wl_list_init(&layer->input_method_destroy.link);
		wl_list_init(&layer->input_method_commit.link);
		wl_list_init(&layer->input_method_grab_keyboard.link);
	}

	set_grab(layer, input_method != NULL ? input_method->keyboard_grab : NULL);
}

void wlr_input_method_v2_input_router_layer_set_active_text_input(
		struct wlr_input_method_v2_input_router_layer *layer,
		struct wlr_text_input_v3 *text_input) {
	if (layer->active_text_input == text_input) {
		return;
	}

	layer->active_text_input = text_input;

	wl_list_remove(&layer->active_text_input_destroy.link);
	wl_list_remove(&layer->active_text_input_commit.link);

	if (text_input != NULL) {
		wl_signal_add(&text_input->events.destroy, &layer->active_text_input_destroy);
		wl_signal_add(&text_input->events.commit, &layer->active_text_input_commit);
	} else {
		wl_list_init(&layer->active_text_input_destroy.link);
		wl_list_init(&layer->active_text_input_commit.link);
	}

	if (layer->input_method != NULL) {
		bool sent = false;
		sent |= update_input_method_active(layer);
		sent |= active_text_input_state(layer, true);
		if (sent) {
			wlr_input_method_v2_send_done(layer->input_method);
		}
	}
}

bool wlr_input_method_v2_input_router_layer_register(int32_t priority) {
	if (!wlr_input_router_keyboard_register_interface(&keyboard_impl, priority)) {
		return false;
	}
	return true;
}

struct wlr_input_method_v2_input_router_layer *wlr_input_method_v2_input_router_layer_create(
		struct wlr_input_router *router) {
	struct wlr_input_method_v2_input_router_layer *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_input_router_keyboard_init(&layer->keyboard,
		router, &keyboard_impl);

	layer->active_text_input_destroy.notify = handle_active_text_input_destroy;
	wl_list_init(&layer->active_text_input_destroy.link);
	layer->active_text_input_commit.notify = handle_active_text_input_commit;
	wl_list_init(&layer->active_text_input_commit.link);

	layer->input_method_destroy.notify = handle_input_method_destroy;
	wl_list_init(&layer->input_method_destroy.link);
	layer->input_method_commit.notify = handle_input_method_commit;
	wl_list_init(&layer->input_method_commit.link);
	layer->input_method_grab_keyboard.notify = handle_input_method_grab_keyboard;
	wl_list_init(&layer->input_method_grab_keyboard.link);

	layer->router = router;
	layer->router_destroy.notify = handle_router_destroy;
	wl_signal_add(&router->events.destroy, &layer->router_destroy);

	layer->grab_destroy.notify = handle_grab_destroy;
	wl_list_init(&layer->grab_destroy.link);

	wl_signal_init(&layer->events.destroy);

	update_device_grab(layer);

	return layer;
}

void wlr_input_method_v2_input_router_layer_destroy(
		struct wlr_input_method_v2_input_router_layer *layer) {
	if (layer == NULL) {
		return;
	}

	wl_signal_emit_mutable(&layer->events.destroy, NULL);

	assert(wl_list_empty(&layer->events.destroy.listener_list));

	wlr_input_router_keyboard_notify_device(&layer->keyboard,
		&(struct wlr_input_router_keyboard_device_event){
			.device = layer->keyboard.device,
		});
	wlr_input_router_keyboard_finish(&layer->keyboard);

	wl_list_remove(&layer->active_text_input_destroy.link);
	wl_list_remove(&layer->active_text_input_commit.link);

	wl_list_remove(&layer->input_method_destroy.link);
	wl_list_remove(&layer->input_method_commit.link);
	wl_list_remove(&layer->input_method_grab_keyboard.link);

	wl_list_remove(&layer->router_destroy.link);
	wl_list_remove(&layer->grab_destroy.link);

	free(layer);
}
