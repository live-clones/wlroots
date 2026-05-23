#include <stdio.h>
#include <stdlib.h>
#include <libudev.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <wlr/backend/session.h>
#include "backend/session/session.h"

struct drm_event_monitor {
	struct udev *udev;
	struct udev_monitor *mon;
};

struct drm_event_monitor *monitor_drm_events(int *fd) {
	struct drm_event_monitor *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return NULL;
	}
	ctx->udev = udev_new();
	if (!ctx->udev) {
		free(ctx);
		return NULL;
	}
	ctx->mon = udev_monitor_new_from_netlink(ctx->udev, "udev");
	if (!ctx->mon) {
		udev_unref(ctx->udev);
		free(ctx);
		return NULL;
	}
	udev_monitor_filter_add_match_subsystem_devtype(ctx->mon, "drm", NULL);
	udev_monitor_enable_receiving(ctx->mon);
	*fd = udev_monitor_get_fd(ctx->mon);
	return ctx;
}

void drm_event_monitor_free(struct drm_event_monitor *ctx) {
	udev_monitor_unref(ctx->mon);
	udev_unref(ctx->udev);
	free(ctx);
}

static void populate_drm_event(struct drm_event_content *ev,
		struct udev_device *dev) {
	memset(ev, 0, sizeof(*ev));

	const char *devnode = udev_device_get_devnode(dev);
	if (devnode) {
		int len = snprintf(ev->devnode, sizeof(ev->devnode), "%s", devnode);
		assert((size_t)len < sizeof(ev->devnode));
	}
	ev->devnum = udev_device_get_devnum(dev);

	const char *action = udev_device_get_action(dev);
	if (strcmp(action, "add") == 0) {
		ev->type = DRM_EVENT_ACTION_ADD;
	} else if (strcmp(action, "change") == 0) {
		ev->type = DRM_EVENT_ACTION_CHANGE;
	} else if (strcmp(action, "remove") == 0) {
		ev->type = DRM_EVENT_ACTION_REMOVE;
	}

	const char *hotplug = udev_device_get_property_value(dev, "HOTPLUG");
	if (hotplug != NULL && strcmp(hotplug, "1") == 0) {
		ev->has_lease = false;

		const char *connector =
			udev_device_get_property_value(dev, "CONNECTOR");
		if (connector != NULL) {
			ev->conn_id = strtoul(connector, NULL, 10);
		}

		const char *prop =
			udev_device_get_property_value(dev, "PROPERTY");
		if (prop != NULL) {
			ev->prop_id = strtoul(prop, NULL, 10);
		}

		return;
	}

	const char *lease = udev_device_get_property_value(dev, "LEASE");
	if (lease != NULL && strcmp(lease, "1") == 0) {
		ev->has_lease = true;
	}
}

int receive_drm_device(struct drm_event_monitor *ctx,
		struct drm_event_content *ev) {
	struct udev_device *dev = udev_monitor_receive_device(ctx->mon);
	if (!dev) {
		return -1;
	}
	populate_drm_event(ev, dev);
	udev_device_unref(dev);
	return 0;
}

static struct udev_device *get_udev_device_from_devnode(struct udev *udev,
		const char *devnode) {
	const char *sysname = strrchr(devnode, '/');
	if (!sysname) {
		return NULL;
	}
	sysname += 1;
	struct udev_device *dev =
		udev_device_new_from_subsystem_sysname(udev, "drm", sysname);
	if (!dev) {
		return NULL;
	}
	return dev;
}

// XXX: move it to libseat?
const char *get_device_seat(struct drm_event_monitor *ctx, const char *devnode) {
	struct udev_device *dev = get_udev_device_from_devnode(ctx->udev, devnode);
	if (!dev) {
		return NULL;
	}
	const char *seat = udev_device_get_property_value(dev, "ID_SEAT");
	udev_device_unref(dev);
	return seat;
}

// XXX: use sysfs directly?
bool is_drm_device_primary(struct drm_event_monitor *ctx, const char *devnode) {
	struct udev_device *dev = get_udev_device_from_devnode(ctx->udev, devnode);
	if (!dev) {
		return false;
	}
	const char *boot_display = udev_device_get_sysattr_value(dev, "boot_display");
	if (boot_display && strcmp(boot_display, "1") == 0) {
		udev_device_unref(dev);
		return true;
	}
	// This is owned by 'dev', so we don't need to free it
	struct udev_device *pci =
		udev_device_get_parent_with_subsystem_devtype(dev, "pci", NULL);
	if (!pci) {
		udev_device_unref(dev);
		return false;
	}
	const char *id = udev_device_get_sysattr_value(pci, "boot_vga");
	bool primary = id && strcmp(id, "1") == 0;
	udev_device_unref(dev);
	return primary;
}
