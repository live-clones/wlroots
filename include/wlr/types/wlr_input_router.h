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

#define WLR_INPUT_ROUTER_MAX_POINTER_BUTTONS 32
#define WLR_INPUT_ROUTER_MAX_TOUCH_POINTS 16

struct wlr_input_router_handler {
	struct {
		int32_t priority;
		struct wlr_input_router_handler *head;
		struct wlr_input_router_handler *next;
	} WLR_PRIVATE;
};

struct wlr_input_router_handler_interface {
	const char *name;
};

/**
 * A registry of input event handler interface priorities.
 */
struct wlr_input_router_handler_priority_list {
	struct {
		struct wl_array entries; // struct wlr_input_router_handler_priority_entry
	} WLR_PRIVATE;
};

enum wlr_input_router_focus_type {
	WLR_INPUT_ROUTER_FOCUS_NONE,
	WLR_INPUT_ROUTER_FOCUS_SURFACE,
	WLR_INPUT_ROUTER_FOCUS_USER,
};

/**
 * A helper object to store focus information. When the underlying object is
 * destroyed, the focus is automatically reset.
 *
 * Input router focus readers accept NULL, which is treated the same way as an
 * empty focus.
 */
struct wlr_input_router_focus {
	enum wlr_input_router_focus_type type;
	union {
		struct wlr_surface *surface;
		struct {
			void *user;
			struct wl_signal *destroy_signal;
		};
	};

	struct {
		struct wl_listener destroy;
	} WLR_PRIVATE;
};

struct wlr_input_router;

struct wlr_input_router_interface {
	const char *name;

	void (*at)(struct wlr_input_router *router, double x, double y,
		struct wlr_input_router_focus *focus, double *local_x, double *local_y);
	bool (*get_surface_position)(struct wlr_input_router *router,
		struct wlr_surface *surface, double *x, double *y);
};

struct wlr_input_router_keyboard;

/**
 * Event notifying of a new keyboard focus. It is not guaranteed that the focus
 * has actually changed.
 */
struct wlr_input_router_keyboard_focus_event {
	const struct wlr_input_router_focus *focus;
};

/**
 * Event notifying of a new active keyboard device.
 */
struct wlr_input_router_keyboard_device_event {
	// May be NULL
	struct wlr_keyboard *device;
};

/**
 * Event notifying of a key press or release. It is guaranteed that the current
 * keyboard device is not NULL.
 */
struct wlr_input_router_keyboard_key_event {
	uint32_t time_msec;
	uint32_t key;
	enum wl_keyboard_key_state state;

	// If true, this event has already been consumed and should only be used for
	// bookkeeping.
	bool intercepted;
};

/**
 * Event notifying that the keyboard device modifiers have changed. It is
 * guaranteed that the current keyboard device is not NULL.
 */
struct wlr_input_router_keyboard_modifiers_event {
	struct {
		char unused;
	} WLR_PRIVATE;
};

struct wlr_input_router_keyboard_interface {
	struct wlr_input_router_handler_interface base;

	uint32_t (*focus)(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_focus_event *event);
	void (*device)(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_device_event *event);
	uint32_t (*key)(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_key_event *event);
	void (*modifiers)(struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_modifiers_event *event);
};

struct wlr_input_router_keyboard {
	struct wlr_input_router_handler base;
	const struct wlr_input_router_keyboard_interface *impl;

	struct wlr_input_router_focus focus;
	struct wlr_keyboard *device;

	struct {
		struct wl_listener device_destroy;
	} WLR_PRIVATE;
};

/**
 * Event notifying of a pointer position. The position is not guaranteed to
 * be different from the previous one.
 */
struct wlr_input_router_pointer_position_event {
	uint32_t time_msec;

	double x, y;
	const struct wlr_input_router_focus *focus;
	/**
	 * If true, the focus provided with this event should be prioritized over
	 * focus determined by the handler implementation.
	 */
	bool explicit_focus;

	/**
	 * If true, this event has not been caused by a physical action.
	 */
	bool synthetic;

	/**
	 * Components of pointer motion vectors. It is not guaranteed that (x - dx,
	 * y - dy) is equal to the previous position.
	 */
	double dx, dy;
	double unaccel_dx, unaccel_dy;
};

/**
 * Event notifying of a pointer button press or release.
 */
struct wlr_input_router_pointer_button_event {
	uint32_t time_msec;

	uint32_t button;
	enum wl_pointer_button_state state;

	// The index of the button in the input router pointer, set automatically.
	size_t index;
};

struct wlr_input_router_pointer_axis_event {
	uint32_t time_msec;

	enum wl_pointer_axis_source source;
	enum wl_pointer_axis orientation;
	enum wl_pointer_axis_relative_direction relative_direction;
	double delta;
	int32_t delta_discrete;
};

struct wlr_input_router_pointer_frame_event {
	struct {
		char unused;
	} WLR_PRIVATE;
};

struct wlr_input_router_pointer;

struct wlr_input_router_pointer_interface {
	struct wlr_input_router_handler_interface base;

	uint32_t (*position)(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_position_event *event);
	uint32_t (*button)(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_button_event *event);
	void (*axis)(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_axis_event *event);
	void (*frame)(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_frame_event *event);
};

struct wlr_input_router_pointer_button {
	uint32_t button;
	// The number of times the button has been pressed.
	uint32_t count;
};

struct wlr_input_router_pointer {
	struct wlr_input_router_handler base;
	const struct wlr_input_router_pointer_interface *impl;

	double x, y;
	struct wlr_input_router_focus focus;

	struct wlr_input_router_pointer_button buttons[WLR_INPUT_ROUTER_MAX_POINTER_BUTTONS];
	uint32_t n_buttons;
};

/**
 * Event notifying of a touch point position. The position is not guaranteed to
 * be different from the previous one. id is guaranteed to abe a valid touch
 * point ID.
 */
struct wlr_input_router_touch_position_event {
	uint32_t time_msec;
	int32_t id;

	double x, y;
	const struct wlr_input_router_focus *focus;

	// The index of the touch point in the input router touch, set
	// automatically.
	size_t index;
};

/**
 * Event notifying of a new touch point. id is guaranteed to be unique among
 * all touch points. This event adds a touch point.
 */
struct wlr_input_router_touch_down_event {
	uint32_t time_msec;
	int32_t id;

	double x, y;
	const struct wlr_input_router_focus *focus;

	// The index of the touch point in the input router touch, set
	// automatically.
	size_t index;
};

/**
 * Event notifying that a touch point has disappeared. id is guaranteed to be
 * a valid touch point ID. This event removes the touch point.
 */
struct wlr_input_router_touch_up_event {
	uint32_t time_msec;
	int32_t id;

	// The index of the touch point in the input router touch, set
	// automatically.
	size_t index;
};

/**
 * Event notifying that a touch point has been cancelled. id is guaranteed to be
 * a valid touch point ID. This event removes the touch point.
 */
struct wlr_input_router_touch_cancel_event {
	int32_t id;

	// The index of the touch point in the input router touch, set
	// automatically.
	size_t index;
};

struct wlr_input_router_touch_frame_event {
	struct {
		char unused;
	} WLR_PRIVATE;
};

struct wlr_input_router_touch;

struct wlr_input_router_touch_interface {
	struct wlr_input_router_handler_interface base;

	void (*position)(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_position_event *event);
	uint32_t (*down)(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_down_event *event);
	uint32_t (*up)(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_up_event *event);
	void (*cancel)(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_cancel_event *event);
	void (*frame)(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_frame_event *event);
};

struct wlr_input_router_touch_point {
	int32_t id;

	double x, y;
	struct wlr_input_router_focus focus;
};

struct wlr_input_router_touch {
	struct wlr_input_router_handler base;
	const struct wlr_input_router_touch_interface *impl;

	struct wlr_input_router_touch_point points[WLR_INPUT_ROUTER_MAX_TOUCH_POINTS];
	size_t n_points;
};

/**
 * An input router is an object which has keyboard, pointer, and touch event
 * handler chains.
 *
 * Each handler can receive events and send events to the next handler in the
 * chain. If the function in the handler interface responsible for handling
 * events of a specific type is NULL, events of that type are automatically
 * passed along to the next handler.
 *
 * When an input event handler is added, it's placed accordingly to its
 * priority, which must be registered beforehand. The newly added handler copies
 * the state from the next handler in the chain, if one exists.
 */
struct wlr_input_router {
	struct wlr_input_router_keyboard keyboard;
	struct wlr_input_router_pointer pointer;
	struct wlr_input_router_touch touch;

	const struct wlr_input_router_interface *impl;

	struct {
		struct wl_signal destroy;
	} events;

	struct wlr_addon_set addons;

	void *data;
};

/**
 * An input router focus layer which assigns focus for revelant pointer and
 * touch events based on position.
 */
struct wlr_input_router_focus_layer {
	struct wlr_input_router *router;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct wlr_input_router_pointer pointer;
		struct wlr_input_router_touch touch;

		struct wlr_input_router_focus focus;

		struct wl_listener router_destroy;
	} WLR_PRIVATE;
};

struct wlr_input_router_implicit_grab_layer_touch_point {
	struct {
		uint32_t serial;
		struct wlr_input_router_focus focus;
	} WLR_PRIVATE;
};

/**
 * An input router implicit grab layer which implements implicit grab semantics.
 * For pointer, it means that the focus is locked if at least one button is
 * pressed. For touch, it means the focus received with a new touch point always
 * stays the same.
 */
struct wlr_input_router_implicit_grab_layer {
	struct wlr_input_router *router;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct wlr_input_router_pointer pointer;
		struct wlr_input_router_focus pointer_focus;
		uint32_t pointer_init_button;
		uint32_t pointer_init_serial;
		bool pointer_grabbed;

		struct wlr_input_router_touch touch;
		struct wlr_input_router_implicit_grab_layer_touch_point
			touch_points[WLR_INPUT_ROUTER_MAX_TOUCH_POINTS];

		struct wl_listener router_destroy;
	} WLR_PRIVATE;
};

void wlr_input_router_init(struct wlr_input_router *router,
		const struct wlr_input_router_interface *impl);
void wlr_input_router_finish(struct wlr_input_router *router);

void wlr_input_router_focus_init(struct wlr_input_router_focus *focus);
void wlr_input_router_focus_finish(struct wlr_input_router_focus *focus);

bool wlr_input_router_focus_is_none(const struct wlr_input_router_focus *focus);
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

// TODO: this is only required by the focus layer, remove?
void wlr_input_router_at(struct wlr_input_router *router, double x, double y,
		struct wlr_input_router_focus *focus, double *local_x, double *local_y);

bool wlr_input_router_get_surface_position(struct wlr_input_router *router,
		struct wlr_surface *surface, double *x, double *y);

bool wlr_input_router_register_handler_interface(
		const struct wlr_input_router_handler_interface *iface,
		int32_t priority, struct wlr_input_router_handler_priority_list *priority_list);

void wlr_input_router_handler_init(struct wlr_input_router_handler *handler,
		struct wlr_input_router_handler *head,
		const struct wlr_input_router_handler_interface *impl,
		const struct wlr_input_router_handler_priority_list *priority_list);

void wlr_input_router_handler_finish(struct wlr_input_router_handler *handler);

uint32_t wlr_input_router_keyboard_notify_focus(
		struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_focus_event *event);

void wlr_input_router_keyboard_notify_device(
		struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_device_event *event);

uint32_t wlr_input_router_keyboard_notify_key(
		struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_key_event *event);

void wlr_input_router_keyboard_notify_modifiers(
		struct wlr_input_router_keyboard *keyboard,
		const struct wlr_input_router_keyboard_modifiers_event *event);

bool wlr_input_router_keyboard_register_interface(
		const struct wlr_input_router_keyboard_interface *iface, int32_t priority);

void wlr_input_router_keyboard_init(
		struct wlr_input_router_keyboard *handler, struct wlr_input_router *router,
		const struct wlr_input_router_keyboard_interface *impl);

void wlr_input_router_keyboard_finish(struct wlr_input_router_keyboard *handler);

uint32_t wlr_input_router_pointer_notify_position(
		struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_position_event *event);

uint32_t wlr_input_router_pointer_notify_button(
		struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_button_event *event);

void wlr_input_router_pointer_notify_axis(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_axis_event *event);

void wlr_input_router_pointer_notify_frame(struct wlr_input_router_pointer *pointer,
		const struct wlr_input_router_pointer_frame_event *event);

uint32_t wlr_input_router_pointer_refresh_position(struct wlr_input_router_pointer *pointer);

uint32_t wlr_input_router_pointer_clear_focus(struct wlr_input_router_pointer *pointer);

bool wlr_input_router_pointer_register_interface(
		const struct wlr_input_router_pointer_interface *iface, int32_t priority);

void wlr_input_router_pointer_init(struct wlr_input_router_pointer *pointer,
		struct wlr_input_router *router, const struct wlr_input_router_pointer_interface *impl);
void wlr_input_router_pointer_finish(struct wlr_input_router_pointer *pointer);

void wlr_input_router_touch_notify_position(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_position_event *event);

uint32_t wlr_input_router_touch_notify_down(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_down_event *event);

uint32_t wlr_input_router_touch_notify_up(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_up_event *event);

void wlr_input_router_touch_notify_cancel(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_cancel_event *event);

void wlr_input_router_touch_notify_frame(struct wlr_input_router_touch *touch,
		const struct wlr_input_router_touch_frame_event *event);

bool wlr_input_router_touch_register_interface(
		const struct wlr_input_router_touch_interface *iface, int32_t priority);

void wlr_input_router_touch_init(struct wlr_input_router_touch *touch,
		struct wlr_input_router *router, const struct wlr_input_router_touch_interface *impl);
void wlr_input_router_touch_finish(struct wlr_input_router_touch *touch);

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
