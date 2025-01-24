/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_INPUT_METHOD_V2_H
#define WLR_TYPES_WLR_INPUT_METHOD_V2_H
#include <stdint.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>

struct wlr_text_input_v3;

struct wlr_input_method_v2_preedit_string {
	char *text;
	int32_t cursor_begin;
	int32_t cursor_end;
};

struct wlr_input_method_v2_delete_surrounding_text {
	uint32_t before_length;
	uint32_t after_length;
};

struct wlr_input_method_v2_state {
	struct wlr_input_method_v2_preedit_string preedit;
	char *commit_text;
	struct wlr_input_method_v2_delete_surrounding_text delete;
};

struct wlr_input_method_v2 {
	struct wl_resource *resource;

	struct wlr_seat *seat;
	struct wlr_seat_client *seat_client;

	struct wlr_input_method_v2_state pending;
	struct wlr_input_method_v2_state current;
	bool active; // pending compositor-side state
	bool client_active; // state known to the client
	uint32_t current_serial; // received in last commit call

	struct wl_list popup_surfaces;
	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab;

	struct wl_list link;

	struct {
		struct wl_signal commit; // struct wlr_input_method_v2
		struct wl_signal new_popup_surface; // struct wlr_input_popup_surface_v2
		struct wl_signal grab_keyboard; // struct wlr_input_method_keyboard_grab_v2
		struct wl_signal destroy; // struct wlr_input_method_v2
	} events;

	struct {
		struct wl_listener seat_client_destroy;
	} WLR_PRIVATE;
};

struct wlr_input_popup_surface_v2 {
	struct wl_resource *resource;
	struct wlr_input_method_v2 *input_method;
	struct wl_list link;

	struct wlr_surface *surface;

	struct {
		struct wl_signal destroy;
	} events;

	void *data;
};

struct wlr_input_method_keyboard_grab_v2 {
	struct wl_resource *resource;
	struct wlr_input_method_v2 *input_method;
	struct wlr_keyboard *keyboard;

	struct {
		struct wl_signal destroy; // struct wlr_input_method_keyboard_grab_v2
	} events;

	struct {
		struct wl_listener keyboard_keymap;
		struct wl_listener keyboard_repeat_info;
		struct wl_listener keyboard_destroy;
	} WLR_PRIVATE;
};

struct wlr_input_method_manager_v2 {
	struct wl_global *global;
	struct wl_list input_methods; // struct wlr_input_method_v2.link

	struct {
		struct wl_signal input_method; // struct wlr_input_method_v2
		struct wl_signal destroy; // struct wlr_input_method_manager_v2
	} events;

	struct {
		struct wl_listener display_destroy;
	} WLR_PRIVATE;
};

/**
 * A zwp_input_method_v2 input router layer which redirects keyboard events to
 * an active zwp_input_method_keyboard_grab_v2 object, if one exists. This layer
 * detects virtual keyboard devices belonging to the input method client and
 * does not process events from them.
 */
struct wlr_input_method_v2_input_router_layer {
	struct wlr_input_router *router;
	struct wlr_input_method_v2 *input_method;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct wlr_input_router_keyboard keyboard;

		struct wlr_input_method_keyboard_grab_v2 *grab;
		bool device_grabbed;

		uint32_t forwarded_keys[WLR_KEYBOARD_KEYS_CAP];
		size_t n_forwarded_keys;

		struct wlr_text_input_v3 *active_text_input;

		struct wl_listener active_text_input_destroy;
		struct wl_listener active_text_input_commit;

		struct wl_listener input_method_destroy;
		struct wl_listener input_method_commit;
		struct wl_listener input_method_grab_keyboard;

		struct wl_listener router_destroy;
		struct wl_listener grab_destroy;
	} WLR_PRIVATE;
};

struct wlr_input_method_manager_v2 *wlr_input_method_manager_v2_create(
	struct wl_display *display);

void wlr_input_method_v2_send_activate(
	struct wlr_input_method_v2 *input_method);
void wlr_input_method_v2_send_deactivate(
	struct wlr_input_method_v2 *input_method);
void wlr_input_method_v2_send_surrounding_text(
	struct wlr_input_method_v2 *input_method, const char *text,
	uint32_t cursor, uint32_t anchor);
void wlr_input_method_v2_send_content_type(
	struct wlr_input_method_v2 *input_method, uint32_t hint,
	uint32_t purpose);
void wlr_input_method_v2_send_text_change_cause(
	struct wlr_input_method_v2 *input_method, uint32_t cause);
void wlr_input_method_v2_send_done(struct wlr_input_method_v2 *input_method);
void wlr_input_method_v2_send_unavailable(
	struct wlr_input_method_v2 *input_method);

/**
 * Get a struct wlr_input_popup_surface_v2 from a struct wlr_surface.
 *
 * Returns NULL if the surface has a different role or if the input popup
 * surface has been destroyed.
 */
struct wlr_input_popup_surface_v2 *wlr_input_popup_surface_v2_try_from_wlr_surface(
	struct wlr_surface *surface);

void wlr_input_popup_surface_v2_send_text_input_rectangle(
    struct wlr_input_popup_surface_v2 *popup_surface, struct wlr_box *sbox);

void wlr_input_method_keyboard_grab_v2_send_key(
	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab,
	uint32_t time, uint32_t key, uint32_t state);
void wlr_input_method_keyboard_grab_v2_send_modifiers(
	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab,
	struct wlr_keyboard_modifiers *modifiers);
void wlr_input_method_keyboard_grab_v2_set_keyboard(
	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab,
	struct wlr_keyboard *keyboard);
void wlr_input_method_keyboard_grab_v2_destroy(
	struct wlr_input_method_keyboard_grab_v2 *keyboard_grab);

bool wlr_input_method_v2_input_router_layer_register(int32_t priority);

struct wlr_input_method_v2_input_router_layer *wlr_input_method_v2_input_router_layer_create(
		struct wlr_input_router *router);
void wlr_input_method_v2_input_router_layer_destroy(
		struct wlr_input_method_v2_input_router_layer *layer);

void wlr_input_method_v2_input_router_layer_set_input_method(
		struct wlr_input_method_v2_input_router_layer *layer,
		struct wlr_input_method_v2 *input_method);
void wlr_input_method_v2_input_router_layer_set_active_text_input(
		struct wlr_input_method_v2_input_router_layer *layer,
		struct wlr_text_input_v3 *text_input);

#endif
