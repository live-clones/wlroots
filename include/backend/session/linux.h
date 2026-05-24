#ifndef BACKEND_SESSION_LINUX_H
#define BACKEND_SESSION_LINUX_H

#include "backend/session/session.h"

struct wlr_linux_device_manager {
	struct wlr_device_manager base;

	int netlink_fd;
	struct wl_event_source *netlink_source;
};

struct wlr_device_manager *wlr_linux_device_manager_create(struct wlr_session *session);

#endif
