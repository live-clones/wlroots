#include <libudev.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/backend/session.h>
#include <wlr/util/log.h>
#include <xf86drm.h>

#include "backend/session/session.h"
#include "backend/session/udev.h"

static struct udev_enumerate *enumerate_drm_cards(struct udev *udev) {
	struct udev_enumerate *en = udev_enumerate_new(udev);
	if (!en) {
		wlr_log(WLR_ERROR, "udev_enumerate_new failed");
		return NULL;
	}

	udev_enumerate_add_match_subsystem(en, "drm");
	udev_enumerate_add_match_sysname(en, DRM_PRIMARY_MINOR_NAME "[0-9]*");

	if (udev_enumerate_scan_devices(en) != 0) {
		wlr_log(WLR_ERROR, "udev_enumerate_scan_devices failed");
		udev_enumerate_unref(en);
		return NULL;
	}

	return en;
}

static ssize_t manager_find_drm_cards(struct wlr_device_manager *base,
		size_t ret_cap, struct wlr_device **ret) {
	struct wlr_udev_device_manager *manager = wl_container_of(base, manager, base);
	struct wlr_session *session = manager->base.session;

	struct udev_enumerate *en = enumerate_drm_cards(manager->udev);
	if (!en) {
		return -1;
	}

	struct udev_list_entry *entry;
	size_t i = 0;
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
		if (i == ret_cap) {
			break;
		}

		const char *path = udev_list_entry_get_name(entry);
		struct udev_device *dev = udev_device_new_from_syspath(manager->udev, path);
		if (!dev) {
			continue;
		}

		const char *seat = udev_device_get_property_value(dev, "ID_SEAT");
		if (!seat) {
			seat = "seat0";
		}
		if (session->seat[0] && strcmp(session->seat, seat) != 0) {
			udev_device_unref(dev);
			continue;
		}

		bool is_primary = false;
		const char *boot_display = udev_device_get_sysattr_value(dev, "boot_display");
		if (boot_display && strcmp(boot_display, "1") == 0) {
		    is_primary = true;
		} else {
			// This is owned by 'dev', so we don't need to free it
			struct udev_device *pci =
				udev_device_get_parent_with_subsystem_devtype(dev, "pci", NULL);

			if (pci) {
				const char *id = udev_device_get_sysattr_value(pci, "boot_vga");
				if (id && strcmp(id, "1") == 0) {
					is_primary = true;
				}
			}
		}

		struct wlr_device *wlr_dev =
			session_open_if_kms(session, udev_device_get_devnode(dev));
		udev_device_unref(dev);
		if (!wlr_dev) {
			continue;
		}

		ret[i] = wlr_dev;
		if (is_primary) {
			struct wlr_device *tmp = ret[0];
			ret[0] = ret[i];
			ret[i] = tmp;
		}

		++i;
	}

	udev_enumerate_unref(en);

	return i;
}

static void manager_destroy(struct wlr_device_manager *base) {
	struct wlr_udev_device_manager *manager = wl_container_of(base, manager, base);

	wl_event_source_remove(manager->udev_event);
	udev_monitor_unref(manager->mon);
	udev_unref(manager->udev);
	free(manager);
}

static const struct wlr_device_manager_impl manager_impl = {
	.destroy = manager_destroy,
	.find_drm_cards = manager_find_drm_cards,
};

static bool is_drm_card(const char *sysname) {
	const char prefix[] = DRM_PRIMARY_MINOR_NAME;
	if (strncmp(sysname, prefix, strlen(prefix)) != 0) {
		return false;
	}
	for (size_t i = strlen(prefix); sysname[i] != '\0'; i++) {
		if (sysname[i] < '0' || sysname[i] > '9') {
			return false;
		}
	}
	return true;
}

static void read_udev_change_event(struct wlr_device_change_event *event,
		struct udev_device *udev_dev) {
	const char *hotplug = udev_device_get_property_value(udev_dev, "HOTPLUG");
	if (hotplug != NULL && strcmp(hotplug, "1") == 0) {
		event->type = WLR_DEVICE_HOTPLUG;
		struct wlr_device_hotplug_event *hotplug = &event->hotplug;

		const char *connector =
			udev_device_get_property_value(udev_dev, "CONNECTOR");
		if (connector != NULL) {
			hotplug->connector_id = strtoul(connector, NULL, 10);
		}

		const char *prop =
			udev_device_get_property_value(udev_dev, "PROPERTY");
		if (prop != NULL) {
			hotplug->prop_id = strtoul(prop, NULL, 10);
		}

		return;
	}

	const char *lease = udev_device_get_property_value(udev_dev, "LEASE");
	if (lease != NULL && strcmp(lease, "1") == 0) {
		event->type = WLR_DEVICE_LEASE;
		return;
	}
}

static int handle_udev_event(int fd, uint32_t mask, void *data) {
	struct wlr_udev_device_manager *manager = data;
	struct wlr_session *session = manager->base.session;

	struct udev_device *udev_dev = udev_monitor_receive_device(manager->mon);
	if (!udev_dev) {
		return 1;
	}

	const char *sysname = udev_device_get_sysname(udev_dev);
	const char *devnode = udev_device_get_devnode(udev_dev);
	const char *action = udev_device_get_action(udev_dev);
	wlr_log(WLR_DEBUG, "udev event for %s (%s)", sysname, action);

	if (!is_drm_card(sysname) || !action || !devnode) {
		goto out;
	}

	const char *seat = udev_device_get_property_value(udev_dev, "ID_SEAT");
	if (!seat) {
		seat = "seat0";
	}
	if (session->seat[0] != '\0' && strcmp(session->seat, seat) != 0) {
		goto out;
	}

	dev_t devnum = udev_device_get_devnum(udev_dev);
	struct wlr_device *dev = session_find_device_by_devid(session, devnum);
	if (strcmp(action, "add") == 0) {
		if (dev != NULL) {
			wlr_log(WLR_DEBUG, "Skipping duplicate device %s", sysname);
			goto out;
		}

		wlr_log(WLR_DEBUG, "DRM device %s added", sysname);
		struct wlr_session_add_event event = {
			.path = devnode,
		};
		wl_signal_emit_mutable(&session->events.add_drm_card, &event);
	} else if (strcmp(action, "change") == 0 && dev != NULL) {
		wlr_log(WLR_DEBUG, "DRM device %s changed", sysname);
		struct wlr_device_change_event event = {0};
		read_udev_change_event(&event, udev_dev);
		wl_signal_emit_mutable(&dev->events.change, &event);
	} else if (strcmp(action, "remove") == 0 && dev != NULL) {
		wlr_log(WLR_DEBUG, "DRM device %s removed", sysname);
		wl_signal_emit_mutable(&dev->events.remove, NULL);
	}

out:
	udev_device_unref(udev_dev);
	return 1;
}

struct wlr_device_manager *wlr_udev_device_manager_create(struct wlr_session *session) {
	struct wlr_udev_device_manager *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}
	wlr_device_manager_init(&manager->base, &manager_impl, session);

	manager->udev = udev_new();
	if (!manager->udev) {
		wlr_log_errno(WLR_ERROR, "Failed to create udev context");
		goto error_manager;
	}

	manager->mon = udev_monitor_new_from_netlink(manager->udev, "udev");
	if (!manager->mon) {
		wlr_log_errno(WLR_ERROR, "Failed to create udev monitor");
		goto error_udev;
	}

	udev_monitor_filter_add_match_subsystem_devtype(manager->mon, "drm", NULL);
	udev_monitor_enable_receiving(manager->mon);

	int fd = udev_monitor_get_fd(manager->mon);

	manager->udev_event = wl_event_loop_add_fd(session->event_loop, fd,
		WL_EVENT_READABLE, handle_udev_event, manager);
	if (!manager->udev_event) {
		wlr_log_errno(WLR_ERROR, "Failed to create udev event source");
		goto error_mon;
	}

	return &manager->base;

error_mon:
	udev_monitor_unref(manager->mon);
error_udev:
	udev_unref(manager->udev);
error_manager:
	free(manager);
	return NULL;
}

struct wlr_udev_device_manager *wlr_udev_device_manager_try_from_base(struct wlr_device_manager *base) {
	if (base->impl != &manager_impl) {
		return NULL;
	}
	struct wlr_udev_device_manager *manager = wl_container_of(base, manager, base);
	return manager;
}
