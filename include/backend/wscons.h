#ifndef BACKEND_WSCONS_H
#define BACKEND_WSCONS_H

#include <wayland-server-core.h>
#include <wlr/backend/interface.h>
#include <wlr/backend/wscons.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/backend/session.h>

struct wlr_wscons_backend {
	struct wlr_backend backend;

	struct wlr_device *mouse_dev;
	struct wlr_device *kbd_dev;

	struct wlr_keyboard kbd;
	struct wlr_pointer mouse;

	unsigned kbd_type;
	int kqueue_fd;

	struct wlr_session *session;

	struct wl_event_source *input_event;
	struct wl_listener session_destroy;
};

extern const struct wlr_keyboard_impl wscons_keyboard_impl;
extern const struct wlr_pointer_impl wscons_pointer_impl;

#endif
