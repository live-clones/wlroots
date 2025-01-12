/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_INPUT_ROUTER_H
#define WLR_TYPES_WLR_INPUT_ROUTER_H

#include <stdint.h>
#include <wayland-server-protocol.h>

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/addon.h>

struct wlr_input_router_handler {
	struct {
		int32_t priority;
		struct wlr_input_router_handler_chain *chain;
		struct wlr_input_router_handler *next;
	} WLR_PRIVATE;
};

struct wlr_input_router_handler_interface {
	const char *name;
};

struct wlr_input_router_handler_priority_list {
	struct {
		// struct wlr_input_router_handler_priority_entry
		struct wl_array array;
	} WLR_PRIVATE;
};

struct wlr_input_router_handler_chain {
	struct wlr_input_router_handler *head;
};

enum wlr_input_router_focus_type {
	WLR_INPUT_ROUTER_FOCUS_NONE,
	WLR_INPUT_ROUTER_FOCUS_SURFACE,
	WLR_INPUT_ROUTER_FOCUS_USER,
};

struct wlr_input_router_focus {
	enum wlr_input_router_focus_type type;
	union {
		struct wlr_surface *surface;
		struct {
			void *user;
			struct wl_signal *destroy_signal;
		};
	};
	struct wl_listener destroy;
};

struct wlr_input_router;

struct wlr_input_router_interface {
	const char *name;

	void (*at)(struct wlr_input_router *router, double x, double y,
		struct wlr_input_router_focus *focus, double *local_x, double *local_y);
	bool (*get_surface_position)(struct wlr_input_router *router,
		struct wlr_surface *surface, double *x, double *y);

	void (*invalidate_keyboard_focus)(struct wlr_input_router *router);
	void (*invalidate_keyboard_device)(struct wlr_input_router *router);

	void (*invalidate_pointer_position)(struct wlr_input_router *router);
};

struct wlr_input_router {
	const struct wlr_input_router_interface *impl;

	struct {
		struct wl_signal destroy;

		struct wl_signal invalidate_keyboard_focus;
		struct wl_signal invalidate_pointer_position;
	} events;

	struct wlr_addon_set addons;

	void *data;

	struct {
		struct wlr_input_router_handler_chain keyboard_chain;
		struct wlr_input_router_handler_chain pointer_chain;
		struct wlr_input_router_handler_chain touch_chain;
	} WLR_PRIVATE;
};

struct wlr_input_router_keyboard_handler;

struct wlr_input_router_keyboard_set_focus {
	const struct wlr_input_router_focus *focus;

	void *data;
};

struct wlr_input_router_keyboard_set_device {
	struct wlr_keyboard *device;

	void *data;
};

struct wlr_input_router_keyboard_key {
	uint32_t time_msec;
	uint32_t key;
	enum wl_keyboard_key_state state;

	bool intercepted;

	void *data;
};

struct wlr_input_router_keyboard_modifiers {
	const struct wlr_keyboard_modifiers *modifiers;

	void *data;
};

struct wlr_input_router_keyboard_handler_interface {
	struct wlr_input_router_handler_interface base;

	void (*set_focus)(struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_set_focus *event);
	void (*set_device)(struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_set_device *event);
	void (*key)(struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_key *event);
	void (*modifiers)(struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_modifiers *event);
};

struct wlr_input_router_keyboard_handler {
	struct wlr_input_router_handler base;
	const struct wlr_input_router_keyboard_handler_interface *impl;
};

enum wlr_input_router_pointer_grab_state {
	WLR_INPUT_ROUTER_POINTER_GRAB_STATE_UNKNOWN = 0,

	WLR_INPUT_ROUTER_POINTER_GRAB_STATE_STARTED,
	WLR_INPUT_ROUTER_POINTER_GRAB_STATE_GRABBED,
	WLR_INPUT_ROUTER_POINTER_GRAB_STATE_STOPPED,
};

struct wlr_input_router_pointer_position {
	uint32_t time_msec;

	/**
	 * Global position in the layout space. Note that (x - dx, y - dy) is not
	 * guaranteed to be equal to the previous position.
	 */
	double x, y;

	double dx, dy;
	double unaccel_dx, unaccel_dy;

	const struct wlr_input_router_focus *focus;

	void *data;
};

struct wlr_input_router_pointer_clear_focus {
	void *data;
};

struct wlr_input_router_pointer_button {
	uint32_t time_msec;
	uint32_t button;
	enum wl_pointer_button_state state;

	enum wlr_input_router_pointer_grab_state grab_state;

	void *data;
};

struct wlr_input_router_pointer_axis {
	uint32_t time_msec;
	enum wl_pointer_axis_source source;
	enum wl_pointer_axis orientation;
	enum wl_pointer_axis_relative_direction relative_direction;
	double delta;
	int32_t delta_discrete;

	void *data;
};

struct wlr_input_router_pointer_frame {
	void *data;
};

struct wlr_input_router_pointer_handler;

struct wlr_input_router_pointer_handler_interface {
	struct wlr_input_router_handler_interface base;

	void (*position)(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_position *event);
	void (*clear_focus)(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_clear_focus *event);
	uint32_t (*button)(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_button *event);
	void (*axis)(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_axis *event);
	void (*frame)(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_frame *event);
};

struct wlr_input_router_pointer_handler {
	struct wlr_input_router_handler base;
	const struct wlr_input_router_pointer_handler_interface *impl;
};

struct wlr_input_router_touch_position {
	uint32_t time_msec;
	int32_t touch_id;

	/**
	 * Global position in the layout space.
	 */
	double x, y;

	const struct wlr_input_router_focus *focus;

	void *data;
};

struct wlr_input_router_touch_down {
	uint32_t time_msec;
	int32_t id;

	/**
	 * Global position in the layout space.
	 */
	double x, y;

	const struct wlr_input_router_focus *focus;

	void *data;
};

struct wlr_input_router_touch_up {
	uint32_t time_msec;
	int32_t id;

	void *data;
};

struct wlr_input_router_touch_cancel {
	void *data;
};

struct wlr_input_router_touch_frame {
	void *data;
};

struct wlr_input_router_touch_handler;

struct wlr_input_router_touch_handler_interface {
	struct wlr_input_router_handler_interface base;

	void (*position)(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_position *event);
	uint32_t (*down)(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_down *event);
	uint32_t (*up)(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_up *event);
	void (*cancel)(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_cancel *event);
	void (*frame)(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_frame *event);
};

struct wlr_input_router_touch_handler {
	struct wlr_input_router_handler base;
	const struct wlr_input_router_touch_handler_interface *impl;
};

struct wlr_input_router_pointer_grab_layer {
	struct wlr_input_router *router;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct wlr_input_router_pointer_handler pointer_handler;
		size_t pointer_n_buttons;

		struct wl_listener router_destroy;
	} WLR_PRIVATE;
};

struct wlr_input_router_focus_layer {
	struct wlr_input_router *router;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct wlr_input_router_pointer_handler pointer_handler;
		struct wlr_input_router_touch_handler touch_handler;

		struct wlr_input_router_focus focus;

		struct wl_listener router_destroy;
	} WLR_PRIVATE;
};

struct wlr_input_router_implicit_grab_layer {
	struct wlr_input_router *router;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct wlr_input_router_pointer_handler pointer_handler;
		struct wlr_input_router_focus pointer_focus;
		uint32_t pointer_button;
		uint32_t pointer_serial;
		bool pointer_grabbed;

		struct wlr_input_router_touch_handler touch_handler;
		struct wl_list touch_points;

		struct wl_listener router_destroy;
	} WLR_PRIVATE;
};

void wlr_input_router_init(struct wlr_input_router *router,
		const struct wlr_input_router_interface *impl);
void wlr_input_router_finish(struct wlr_input_router *router);

void wlr_input_router_focus_init(struct wlr_input_router_focus *focus);
void wlr_input_router_focus_finish(struct wlr_input_router_focus *focus);

struct wlr_surface *wlr_input_router_focus_get_surface(
		const struct wlr_input_router_focus *focus);
void *wlr_input_router_focus_get_user(const struct wlr_input_router_focus *focus);

void wlr_input_router_focus_clear(struct wlr_input_router_focus *focus);
void wlr_input_router_focus_set_surface(struct wlr_input_router_focus *focus,
		struct wlr_surface *surface);
void wlr_input_router_focus_set_user(struct wlr_input_router_focus *focus,
		void *user, struct wl_signal *destroy_signal);

void wlr_input_router_focus_copy(struct wlr_input_router_focus *dst,
		const struct wlr_input_router_focus *src);

void wlr_input_router_at(struct wlr_input_router *router, double x, double y,
		struct wlr_input_router_focus *focus, double *local_x, double *local_y);

bool wlr_input_router_get_surface_position(struct wlr_input_router *router,
		struct wlr_surface *surface, double *x, double *y);

void wlr_input_router_invalidate_keyboard_focus(struct wlr_input_router *router);
void wlr_input_router_invalidate_keyboard_device(struct wlr_input_router *router);

void wlr_input_router_invalidate_pointer_position(struct wlr_input_router *router);

bool wlr_input_router_register_handler_interface(
		const struct wlr_input_router_handler_interface *iface,
		int32_t priority, struct wlr_input_router_handler_priority_list *priority_list);

void wlr_input_router_handler_init(struct wlr_input_router_handler *handler,
		struct wlr_input_router_handler_chain *chain,
		const struct wlr_input_router_handler_interface *impl,
		const struct wlr_input_router_handler_priority_list *priority_list);

void wlr_input_router_handler_finish(struct wlr_input_router_handler *handler);

void wlr_input_router_keyboard_handler_relay_set_focus(
		struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_set_focus *event);

void wlr_input_router_notify_keyboard_set_focus(struct wlr_input_router *router,
		const struct wlr_input_router_keyboard_set_focus *event);

void wlr_input_router_keyboard_handler_relay_set_device(
		struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_set_device *event);

void wlr_input_router_notify_keyboard_set_device(struct wlr_input_router *router,
		const struct wlr_input_router_keyboard_set_device *event);

void wlr_input_router_keyboard_handler_relay_key(
		struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_key *event);

void wlr_input_router_notify_keyboard_key(struct wlr_input_router *router,
		const struct wlr_input_router_keyboard_key *event);

void wlr_input_router_keyboard_handler_relay_modifiers(
		struct wlr_input_router_keyboard_handler *handler,
		const struct wlr_input_router_keyboard_modifiers *event);

void wlr_input_router_notify_keyboard_modifiers(struct wlr_input_router *router,
		const struct wlr_input_router_keyboard_modifiers *event);

bool wlr_input_router_register_keyboard_handler_interface(
		const struct wlr_input_router_keyboard_handler_interface *iface, int32_t priority);

void wlr_input_router_keyboard_handler_init(
		struct wlr_input_router_keyboard_handler *handler, struct wlr_input_router *router,
		const struct wlr_input_router_keyboard_handler_interface *impl);

void wlr_input_router_keyboard_handler_finish(struct wlr_input_router_keyboard_handler *handler);

void wlr_input_router_pointer_handler_relay_position(
		struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_position *event);

void wlr_input_router_notify_pointer_position(struct wlr_input_router *router,
		const struct wlr_input_router_pointer_position *event);

void wlr_input_router_pointer_handler_relay_clear_focus(
		struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_clear_focus *event);

void wlr_input_router_notify_pointer_clear_focus(struct wlr_input_router *router,
		const struct wlr_input_router_pointer_clear_focus *event);

uint32_t wlr_input_router_pointer_handler_relay_button(
		struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_button *event);

uint32_t wlr_input_router_notify_pointer_button(struct wlr_input_router *router,
		const struct wlr_input_router_pointer_button *event);

void wlr_input_router_pointer_handler_relay_axis(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_axis *event);

void wlr_input_router_notify_pointer_axis(struct wlr_input_router *router,
		const struct wlr_input_router_pointer_axis *event);

void wlr_input_router_pointer_handler_relay_frame(struct wlr_input_router_pointer_handler *handler,
		const struct wlr_input_router_pointer_frame *event);

void wlr_input_router_notify_pointer_frame(struct wlr_input_router *router,
		const struct wlr_input_router_pointer_frame *event);

bool wlr_input_router_register_pointer_handler_interface(
		const struct wlr_input_router_pointer_handler_interface *iface, int32_t priority);

void wlr_input_router_pointer_handler_init(
		struct wlr_input_router_pointer_handler *handler, struct wlr_input_router *router,
		const struct wlr_input_router_pointer_handler_interface *impl);
void wlr_input_router_pointer_handler_finish(struct wlr_input_router_pointer_handler *handler);

void wlr_input_router_touch_handler_relay_position(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_position *event);

void wlr_input_router_notify_touch_position(struct wlr_input_router *router,
		const struct wlr_input_router_touch_position *event);

uint32_t wlr_input_router_touch_handler_relay_down(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_down *event);

uint32_t wlr_input_router_notify_touch_down(struct wlr_input_router *router,
		const struct wlr_input_router_touch_down *event);

uint32_t wlr_input_router_touch_handler_relay_up(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_up *event);

uint32_t wlr_input_router_notify_touch_up(struct wlr_input_router *router,
		const struct wlr_input_router_touch_up *event);

void wlr_input_router_touch_handler_relay_cancel(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_cancel *event);

void wlr_input_router_notify_touch_cancel(struct wlr_input_router *router,
		const struct wlr_input_router_touch_cancel *event);

void wlr_input_router_touch_handler_relay_frame(struct wlr_input_router_touch_handler *handler,
		const struct wlr_input_router_touch_frame *event);

void wlr_input_router_notify_touch_frame(struct wlr_input_router *router,
		const struct wlr_input_router_touch_frame *event);

bool wlr_input_router_register_touch_handler_interface(
		const struct wlr_input_router_touch_handler_interface *iface, int32_t priority);

void wlr_input_router_touch_handler_init(
		struct wlr_input_router_touch_handler *handler, struct wlr_input_router *router,
		const struct wlr_input_router_touch_handler_interface *impl);
void wlr_input_router_touch_handler_finish(struct wlr_input_router_touch_handler *handler);

bool wlr_input_router_pointer_grab_layer_register(int32_t priority);

struct wlr_input_router_pointer_grab_layer *wlr_input_router_pointer_grab_layer_create(
		struct wlr_input_router *router);
void wlr_input_router_pointer_grab_layer_destroy(
		struct wlr_input_router_pointer_grab_layer *layer);

bool wlr_input_router_focus_layer_register(int32_t priority);

struct wlr_input_router_focus_layer *wlr_input_router_focus_layer_create(
		struct wlr_input_router *router);
void wlr_input_router_focus_layer_destroy(struct wlr_input_router_focus_layer *layer);

bool wlr_input_router_implicit_grab_layer_register(int32_t priority);

struct wlr_input_router_implicit_grab_layer *wlr_input_router_implicit_grab_layer_create(
		struct wlr_input_router *router);
void wlr_input_router_implicit_grab_layer_destroy(
		struct wlr_input_router_implicit_grab_layer *layer);

bool wlr_input_router_implicit_grab_layer_validate_pointer_serial(
		struct wlr_input_router_implicit_grab_layer *layer, struct wlr_surface *origin,
		uint32_t serial, uint32_t *button);

bool wlr_input_router_implicit_grab_layer_validate_touch_serial(
		struct wlr_input_router_implicit_grab_layer *layer, struct wlr_surface *origin,
		uint32_t serial, int32_t *id);

#endif
