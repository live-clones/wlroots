#define _BSD_SOURCE
#include <sys/ioctl.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/event.h>
#include <wlr/util/log.h>
#include <wlr/backend/interface.h>
#include <wlr/backend/session.h>
#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsksymdef.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>

#include "backend/wscons.h"
#include "util/time.h"

#include "atKeynames.h"
#include "bsd_KbdMap.h"

static struct wlr_wscons_backend *get_wscons_backend_from_backend(
		struct wlr_backend *wlr_backend) {
	assert(wlr_backend_is_wscons(wlr_backend));
	struct wlr_wscons_backend *backend = wl_container_of(wlr_backend, backend, backend);
	return backend;
}

static int ws_to_xkb(unsigned type, int key) {
	switch (type) {
	case WSKBD_TYPE_PC_XT:
	case WSKBD_TYPE_PC_AT:
		return wsXtMap[key];
	case WSKBD_TYPE_USB:
		return wsUsbMap[key];
	default:
		wlr_log(WLR_INFO, "Unknown wskbd type %d", type);
		return key;
	}
}

static int wsmouse_to_evdev(int button) {
	// The right and middle mouse buttons must be swapped
	switch (button) {
	case 1: // Middle
		return 0x112;
	case 2: // Right
		return 0x111;
	default:
		return button + 0x110;
	}
}

static void notify_key(struct wlr_keyboard *wlr_kbd, uint32_t msec,
		uint32_t state, uint32_t keycode) {
	struct wlr_keyboard_key_event ev;
	ev.update_state = true;
	ev.time_msec = msec;
	ev.state = state;
	ev.keycode = keycode;

	wlr_keyboard_notify_key(wlr_kbd, &ev);
}

static void notify_button(struct wlr_pointer *wlr_ptr, uint32_t msec,
		uint32_t state, uint32_t button) {
	struct wlr_pointer_button_event ev;
	ev.pointer = wlr_ptr;
	ev.time_msec = msec;
	ev.button = button;
	ev.state = state;

	wlr_pointer_notify_button(wlr_ptr, &ev);
	wl_signal_emit_mutable(&wlr_ptr->events.frame, wlr_ptr);
}

static void notify_motion(struct wlr_pointer *wlr_ptr, uint32_t msec,
		double x, double y) {
	struct wlr_pointer_motion_event ev;
	ev.pointer = wlr_ptr;
	ev.time_msec = msec;
	ev.delta_x = x;
	ev.delta_y = y;
	ev.unaccel_dx = x;
	ev.unaccel_dy = y;

	wl_signal_emit_mutable(&wlr_ptr->events.motion, &ev);
	wl_signal_emit_mutable(&wlr_ptr->events.frame, wlr_ptr);
}

static void notify_axis(struct wlr_pointer *wlr_ptr, uint32_t msec,
		uint32_t axis, double delta) {
	struct wlr_pointer_axis_event ev;
	ev.pointer = wlr_ptr;
	ev.time_msec = msec;
	ev.source = WL_POINTER_AXIS_SOURCE_WHEEL;
	ev.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL;
	ev.delta_discrete = 0; // XXX
	ev.orientation = axis;
	ev.delta = delta;

	wl_signal_emit_mutable(&wlr_ptr->events.axis, &ev);
	wl_signal_emit_mutable(&wlr_ptr->events.frame, wlr_ptr);
}

static void notify_motion_abs(struct wlr_pointer *wlr_ptr, uint32_t msec,
		double x, double y) {
	struct wlr_pointer_motion_absolute_event ev;
	ev.pointer = wlr_ptr;
	ev.time_msec = msec;
	ev.x = x;
	ev.y = y;

	wl_signal_emit_mutable(&wlr_ptr->events.motion_absolute, &ev);
	wl_signal_emit_mutable(&wlr_ptr->events.frame, wlr_ptr);
}

static void handle_event(struct wlr_wscons_backend *backend, struct wscons_event *ws_ev) {
	struct wlr_keyboard *wlr_kbd = &backend->kbd;
	struct wlr_pointer *wlr_ptr = &backend->mouse;

	uint32_t msec = timespec_to_msec(&ws_ev->time);
	// TODO: make sure all types are handled
	switch (ws_ev->type) {
	case WSCONS_EVENT_KEY_UP:
		notify_key(wlr_kbd, msec, WL_KEYBOARD_KEY_STATE_RELEASED,
			ws_to_xkb(backend->kbd_type, ws_ev->value));
		break;
	case WSCONS_EVENT_KEY_DOWN:
		notify_key(wlr_kbd, msec, WL_KEYBOARD_KEY_STATE_PRESSED,
			ws_to_xkb(backend->kbd_type, ws_ev->value));
		break;
	case WSCONS_EVENT_MOUSE_UP:
		notify_button(wlr_ptr, msec, WL_POINTER_BUTTON_STATE_RELEASED,
			wsmouse_to_evdev(ws_ev->value));
		break;
	case WSCONS_EVENT_MOUSE_DOWN:
		notify_button(wlr_ptr, msec, WL_POINTER_BUTTON_STATE_PRESSED,
			wsmouse_to_evdev(ws_ev->value));
		break;
	case WSCONS_EVENT_MOUSE_DELTA_X:
		notify_motion(wlr_ptr, msec, ws_ev->value, 0);
		break;
	case WSCONS_EVENT_MOUSE_DELTA_Y:
		notify_motion(wlr_ptr, msec, 0, -ws_ev->value);
		break;
	case WSCONS_EVENT_MOUSE_DELTA_Z:
		notify_axis(wlr_ptr, msec, WL_POINTER_AXIS_VERTICAL_SCROLL,
			ws_ev->value * 10);
		break;
	case WSCONS_EVENT_MOUSE_DELTA_W:
		notify_axis(wlr_ptr, msec, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
			ws_ev->value * 10);
		break;
	case WSCONS_EVENT_MOUSE_ABSOLUTE_X:
		notify_motion_abs(wlr_ptr, msec, ws_ev->value, 0);
		break;
	case WSCONS_EVENT_MOUSE_ABSOLUTE_Y:
		notify_motion_abs(wlr_ptr, msec, 0, -ws_ev->value);
		break;
	case WSCONS_EVENT_ALL_KEYS_UP:
	case WSCONS_EVENT_WSMOUSED_ON:
	case WSCONS_EVENT_WSMOUSED_OFF:
		break;
	default:
		wlr_log(WLR_DEBUG, "Unknown wscons event type: %x", ws_ev->type);
		break;
	}
}

static int handle_wscons_readable(int fd, uint32_t mask, void *_backend) {
	struct wlr_wscons_backend *backend = _backend;

	while (backend->session->active) {
		struct kevent kq_ev;
		static const struct timespec dontblock = {0};
		if (kevent(fd, NULL, 0, &kq_ev, 1, &dontblock) <= 0) {
			return 0;
		}
		assert(kq_ev.filter == EVFILT_READ);
		int in_fd = kq_ev.ident;

		struct wscons_event ws_ev;
		if (read(in_fd, &ws_ev, sizeof(ws_ev)) <= 0) {
			continue;
		}
		handle_event(backend, &ws_ev);
	}
	return 0;
}

static void keyboard_set_leds(struct wlr_keyboard *wlr_kb, uint32_t leds) {
	// TODO: use WSKBDIO_SETLEDS
}

const struct wlr_keyboard_impl wscons_keyboard_impl = {
	.name = "wscons-keyboard",
	.led_update = keyboard_set_leds
};

const struct wlr_pointer_impl wscons_pointer_impl = {
	.name = "wscons-pointer",
};

static int init_wscons(struct wlr_wscons_backend *backend) {
	struct wlr_device *mouse_dev = wlr_session_open_file(backend->session, "/dev/wsmouse");
	struct wlr_device *kbd_dev = wlr_session_open_file(backend->session, "/dev/wskbd");
	if (!mouse_dev || !kbd_dev) {
		wlr_log(WLR_ERROR, "Failed to open input devices");
		return -1;
	}

	if (ioctl(kbd_dev->fd, WSKBDIO_GTYPE, &backend->kbd_type) == -1) {
		// TODO: free
		wlr_log(WLR_ERROR, "Failed to get keyboard type");
		return -1;
	}

	int kfd = kqueue1(O_CLOEXEC);
	if (kfd == -1) {
		// TODO: free
		wlr_log(WLR_ERROR, "Failed to create kqueue fd");
		return -1;
	}
	struct kevent evs[2];
	EV_SET(&evs[0], mouse_dev->fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	EV_SET(&evs[1], kbd_dev->fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	if (kevent(kfd, evs, sizeof(evs) / sizeof(evs[0]), NULL, 0, NULL) == -1) {
		// TODO: free
		wlr_log(WLR_ERROR, "Failed to setup kqueue");
		return -1;
	}

	backend->mouse_dev = mouse_dev;
	backend->kbd_dev = kbd_dev;

	struct wlr_keyboard *wlr_kbd = &backend->kbd;
	wlr_keyboard_init(wlr_kbd, &wscons_keyboard_impl, "wscons-keyboard");
	wl_signal_emit_mutable(&backend->backend.events.new_input, &wlr_kbd->base);

	struct wlr_pointer *wlr_ptr = &backend->mouse;
	wlr_pointer_init(wlr_ptr, &wscons_pointer_impl, "wscons-pointer");
	wl_signal_emit_mutable(&backend->backend.events.new_input, &wlr_ptr->base);

	return kfd;
}

static bool backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_wscons_backend *backend =
		get_wscons_backend_from_backend(wlr_backend);
	wlr_log(WLR_DEBUG, "Starting wscons backend");

	int fd = init_wscons(backend);
	if (fd < 0) {
		wlr_log(WLR_ERROR, "Failed to initialize wscons backend");
		return false;
	}
	backend->kqueue_fd = fd;

	if (backend->input_event) {
		wl_event_source_remove(backend->input_event);
	}
	backend->input_event = wl_event_loop_add_fd(backend->session->event_loop, fd,
			WL_EVENT_READABLE, handle_wscons_readable, backend);
	if (!backend->input_event) {
		wlr_log(WLR_ERROR, "Failed to create input event on event loop");
		return false;
	}
	wlr_log(WLR_DEBUG, "wscons successfully initialized");
	return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
	if (!wlr_backend) {
		return;
	}
	struct wlr_wscons_backend *backend =
		get_wscons_backend_from_backend(wlr_backend);

	wlr_backend_finish(wlr_backend);

	wl_list_remove(&backend->session_destroy.link);

	if (backend->input_event) {
		wl_event_source_remove(backend->input_event);
	}

	close(backend->kqueue_fd);

	if (backend->mouse_dev) {
		wlr_session_close_file(backend->session, backend->mouse_dev);
	}
	if (backend->kbd_dev) {
		wlr_session_close_file(backend->session, backend->kbd_dev);
	}

	free(backend);
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
};

bool wlr_backend_is_wscons(const struct wlr_backend *b) {
	return b->impl == &backend_impl;
}

static void handle_session_destroy(struct wl_listener *listener, void *data) {
	struct wlr_wscons_backend *backend =
		wl_container_of(listener, backend, session_destroy);
	backend_destroy(&backend->backend);
}

struct wlr_backend *wlr_wscons_backend_create(struct wlr_session *session) {
	struct wlr_wscons_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		wlr_log(WLR_ERROR, "Allocation failed: %s", strerror(errno));
		return NULL;
	}
	wlr_backend_init(&backend->backend, &backend_impl);

	backend->session = session;
	backend->kqueue_fd = -1;

	backend->session_destroy.notify = handle_session_destroy;
	wl_signal_add(&session->events.destroy, &backend->session_destroy);

	return &backend->backend;
}
