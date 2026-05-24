#ifndef BACKEND_SESSION_UDEV_H
#define BACKEND_SESSION_UDEV_H

#include "backend/session/session.h"

struct wlr_udev_device_manager {
	struct wlr_device_manager base;

	struct udev *udev;
	struct udev_monitor *mon;
	struct wl_event_source *udev_event;
};

struct wlr_device_manager *wlr_udev_device_manager_create(struct wlr_session *session);
struct wlr_udev_device_manager *wlr_udev_device_manager_try_from_base(struct wlr_device_manager *base);

#endif
