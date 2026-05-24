#include <assert.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/session.h>
#include <wlr/util/log.h>
#include <xf86drm.h>

#include "backend/session/linux.h"

// TODO: make configurable
#define NETLINK_GROUPS 0x1

static ssize_t manager_find_drm_cards(struct wlr_device_manager *base,
		size_t ret_cap, struct wlr_device **ret) {
	struct wlr_linux_device_manager *manager = wl_container_of(base, manager, base);
	struct wlr_session *session = manager->base.session;

	uint32_t flags = 0;
	int devices_len = drmGetDevices2(flags, NULL, 0);
	if (devices_len < 0) {
		wlr_log(WLR_ERROR, "drmGetDevices2() failed");
		return -1;
	}
	drmDevice **devices = calloc(devices_len, sizeof(devices[0]));
	if (devices == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return -1;
	}
	devices_len = drmGetDevices2(flags, devices, devices_len);
	if (devices_len < 0) {
		free(devices);
		wlr_log(WLR_ERROR, "drmGetDevices2() failed");
		return -1;
	}

	// TODO: prioritize boot_display/boot_vga
	ssize_t n = 0;
	for (int i = 0; i < devices_len; i++) {
		drmDevice *dev = devices[i];
		if (!(dev->available_nodes & (1 << DRM_NODE_PRIMARY))) {
			continue;
		}

		struct wlr_device *wlr_dev = session_open_if_kms(session, dev->nodes[DRM_NODE_PRIMARY]);
		if (wlr_dev == NULL) {
			continue;
		}

		ret[n] = wlr_dev;
		n++;
	}

	drmFreeDevices(devices, devices_len);
	free(devices);
	return n;
}

static void manager_destroy(struct wlr_device_manager *base) {
	struct wlr_linux_device_manager *manager = wl_container_of(base, manager, base);

	wl_event_source_remove(manager->netlink_source);
	close(manager->netlink_fd);
	free(manager);
}

static const struct wlr_device_manager_impl manager_impl = {
	.destroy = manager_destroy,
	.find_drm_cards = manager_find_drm_cards,
};

static bool parse_ul(unsigned long *out, const char *str) {
	char *end = NULL;
	errno = 0;
	unsigned long v = strtoul(str, &end, 10);
	if (errno != 0 || end == str || end[0] != '\0') {
		return false;
	}
	*out = v;
	return true;
}

static bool parse_devid(dev_t *out, const char *major_str, const char *minor_str) {
	unsigned long major, minor;
	if (!parse_ul(&major, major_str) || !parse_ul(&minor, minor_str)) {
		return false;
	}
	*out = makedev(major, minor);
	return true;
}

static int handle_netlink_event(int fd, uint32_t mask, void *data) {
	struct wlr_linux_device_manager *manager = data;
	struct wlr_session *session = manager->base.session;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		if (mask & WL_EVENT_ERROR) {
			wlr_log(WLR_ERROR, "Failed to wait for netlink event");
		} else {
			wlr_log(WLR_INFO, "Disconnected from netlink socket");
		}
		return 0;
	}

	char buf[8192];
	struct sockaddr_nl addr = {0};
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = sizeof(buf),
	};
	struct msghdr hdr = {
		.msg_name = &addr,
		.msg_namelen = sizeof(addr),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	ssize_t n = recvmsg(fd, &hdr, 0);
	if (n <= 0) {
		wlr_log(WLR_ERROR, "recvmsg() failed");
		return 0;
	}

	if (hdr.msg_flags & MSG_TRUNC) {
		wlr_log(WLR_ERROR, "Received truncated netlink message");
		return 0;
	}

	if (addr.nl_groups == 0x0 || (addr.nl_groups == 0x1 && addr.nl_pid != 0)) {
		wlr_log(WLR_ERROR, "Invalid netlink address");
		return 0;
	}

	assert(buf[n - 1] == '\0');

	size_t offset = 0;
	const char *action = NULL, *subsystem = NULL, *devpath = NULL, *major = NULL, *minor = NULL,
		*hotplug = NULL, *connector = NULL, *property = NULL, *lease = NULL;
	while (offset < (size_t)n) {
		char *line = &buf[offset];
		offset += strlen(line) + 1;
		if (offset == 0) { // summary
			wlr_log(WLR_DEBUG, "Received netlink event: %s", line);
			continue;
		}

		char *eq = strchr(line, '=');
		if (eq == NULL) {
			wlr_log(WLR_ERROR, "Malformed netlink event");
			return 0;
		}
		eq[0] = '\0';
		const char *key = line, *value = &eq[1];

		if (strcmp(key, "ACTION") == 0) {
			action = value;
		} else if (strcmp(key, "SUBSYSTEM") == 0) {
			subsystem = value;
		} else if (strcmp(key, "DEVPATH") == 0) {
			subsystem = value;
		} else if (strcmp(key, "MAJOR") == 0) {
			major = value;
		} else if (strcmp(key, "MINOR") == 0) {
			minor = value;
		} else if (strcmp(key, "HOTPLUG") == 0) {
			hotplug = value;
		} else if (strcmp(key, "CONNECTOR") == 0) {
			connector = value;
		} else if (strcmp(key, "PROPERTY") == 0) {
			property = value;
		} else if (strcmp(key, "LEASE") == 0) {
			lease = value;
		}
	}
	if (subsystem == NULL || strcmp(subsystem, "drm") != 0) {
		return 0;
	}
	if (action == NULL || devpath == NULL || major == NULL || minor == NULL) {
		wlr_log(WLR_ERROR, "Missing required properties in netlink event");
		return 0;
	}

	// TODO: filter card devices

	dev_t devid;
	if (!parse_devid(&devid, major, minor)) {
		return 0;
	}

	char path[1024];
	snprintf(path, sizeof(path), "/dev/char/%s:%s", major, minor);

	struct wlr_device *dev = session_find_device_by_devid(session, devid);
	if (strcmp(action, "add") == 0) {
		if (dev != NULL) {
			wlr_log(WLR_DEBUG, "Skipping duplicate device %s", devpath);
			return 0;
		}

		wlr_log(WLR_DEBUG, "DRM device %s added", devpath);
		struct wlr_session_add_event event = {
			.path = path,
		};
		wl_signal_emit_mutable(&session->events.add_drm_card, &event);
	} else if (strcmp(action, "change") == 0 && dev != NULL) {
		wlr_log(WLR_DEBUG, "DRM device %s changed", devpath);
		struct wlr_device_change_event event = {0};
		if (hotplug != NULL && strcmp(hotplug, "1") == 0) {
			event.type = WLR_DEVICE_HOTPLUG;
			if (connector != NULL) {
				unsigned long id;
				if (!parse_ul(&id, connector)) {
					wlr_log(WLR_ERROR, "Invalid CONNECTOR attribute");
					return 0;
				}
				event.hotplug.connector_id = id;
			}
			if (property != NULL) {
				unsigned long id;
				if (!parse_ul(&id, property)) {
					wlr_log(WLR_ERROR, "Invalid PROPERTY attribute");
					return 0;
				}
				event.hotplug.prop_id = id;
			}
		} else if (lease != NULL && strcmp(lease, "1") == 0) {
			event.type = WLR_DEVICE_LEASE;
		} else {
			return 0;
		}
		wl_signal_emit_mutable(&dev->events.change, &event);
	} else if (strcmp(action, "remove") == 0 && dev != NULL) {
		wlr_log(WLR_DEBUG, "DRM device %s removed", devpath);
		wl_signal_emit_mutable(&dev->events.remove, NULL);
	}

	return 1;
}

struct wlr_device_manager *wlr_linux_device_manager_create(struct wlr_session *session) {
	struct wlr_linux_device_manager *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}
	wlr_device_manager_init(&manager->base, &manager_impl, session);

	int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "socket() failed");
		goto error_manager;
	}

	int fd_flags = fcntl(fd, F_GETFL, 0);
	if (fd_flags == -1) {
		wlr_log_errno(WLR_ERROR, "Failed to get FD flags");
		goto error_fd;
	}
	if (fcntl(fd, F_SETFL, fd_flags | O_NONBLOCK | O_CLOEXEC) == -1) {
		wlr_log_errno(WLR_ERROR, "Failed to set FD flags");
		goto error_fd;
	}

	struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_groups = NETLINK_GROUPS };
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		wlr_log_errno(WLR_ERROR, "bind() failed");
		goto error_fd;
	}

	// TODO: SO_ATTACH_FILTER for "drm" subsystem matching
	// https://github.com/systemd/systemd/blob/788ef4c43eb2e74a67b90c3059bf407e7e301c81/src/libsystemd/sd-device/device-monitor.c#L807

	manager->netlink_fd = fd;
	manager->netlink_source = wl_event_loop_add_fd(session->event_loop, fd,
		WL_EVENT_READABLE, handle_netlink_event, manager);
	if (manager->netlink_source == NULL) {
		wlr_log(WLR_ERROR, "wl_event_loop_add_fd() failed");
		goto error_fd;
	}

	return &manager->base;

error_fd:
	close(fd);
error_manager:
	free(manager);
	return NULL;
}
