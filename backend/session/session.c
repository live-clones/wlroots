#include <assert.h>
#include <libseat.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/session.h>
#include <wlr/config.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "backend/session/session.h"
#include "config.h"
#include "util/time.h"

#if HAVE_UDEV
#include "backend/session/udev.h"
#else
#include "backend/session/linux.h"
#endif

#define WAIT_GPU_TIMEOUT 10000 // ms

struct wlr_device *session_find_device_by_devid(struct wlr_session *session, dev_t devid) {
	struct wlr_device *dev;
	wl_list_for_each(dev, &session->devices, link) {
		if (dev->dev == devid) {
			return dev;
		}
	}
	return NULL;
}

void wlr_device_manager_init(struct wlr_device_manager *manager,
		const struct wlr_device_manager_impl *impl, struct wlr_session *session) {
	assert(impl && impl->destroy && impl->find_drm_cards);
	*manager = (struct wlr_device_manager){
		.session = session,
		.impl = impl,
	};
}

void wlr_device_manager_finish(struct wlr_device_manager *manager) {
	// No-op
}

static void device_manager_destroy(struct wlr_device_manager *manager) {
	manager->impl->destroy(manager);
}

static ssize_t device_manager_find_drm_cards(struct wlr_device_manager *manager,
		size_t ret_cap, struct wlr_device **ret) {
	return manager->impl->find_drm_cards(manager, ret_cap, ret);
}

static void handle_enable_seat(struct libseat *seat, void *data) {
	struct wlr_session *session = data;
	session->active = true;
	wl_signal_emit_mutable(&session->events.active, NULL);
}

static void handle_disable_seat(struct libseat *seat, void *data) {
	struct wlr_session *session = data;
	session->active = false;
	wl_signal_emit_mutable(&session->events.active, NULL);
	libseat_disable_seat(session->seat_handle);
}

static int libseat_event(int fd, uint32_t mask, void *data) {
	struct wlr_session *session = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		if (mask & WL_EVENT_ERROR) {
			wlr_log(WLR_ERROR, "Failed to wait for libseat event");
		} else {
			wlr_log(WLR_INFO, "Failed to wait for libseat event");
		}
		wlr_session_destroy(session);
		return 0;
	}
	if (libseat_dispatch(session->seat_handle, 0) == -1) {
		wlr_log_errno(WLR_ERROR, "Failed to dispatch libseat");
		wlr_session_destroy(session);
	}
	return 1;
}

static struct libseat_seat_listener seat_listener = {
	.enable_seat = handle_enable_seat,
	.disable_seat = handle_disable_seat,
};

static enum wlr_log_importance libseat_log_level_to_wlr(
		enum libseat_log_level level) {
	switch (level) {
	case LIBSEAT_LOG_LEVEL_ERROR:
		return WLR_ERROR;
	case LIBSEAT_LOG_LEVEL_INFO:
		return WLR_INFO;
	default:
		return WLR_DEBUG;
	}
}

static void log_libseat(enum libseat_log_level level,
		const char *fmt, va_list args) {
	enum wlr_log_importance importance = libseat_log_level_to_wlr(level);

	static char wlr_fmt[1024];
	snprintf(wlr_fmt, sizeof(wlr_fmt), "[libseat] %s", fmt);

	_wlr_vlog(importance, wlr_fmt, args);
}

static int libseat_session_init(struct wlr_session *session,
		struct wl_event_loop *event_loop) {
	libseat_set_log_handler(log_libseat);
	libseat_set_log_level(LIBSEAT_LOG_LEVEL_INFO);

	// libseat will take care of updating the logind state if necessary
	setenv("XDG_SESSION_TYPE", "wayland", 1);

	session->seat_handle = libseat_open_seat(&seat_listener, session);
	if (session->seat_handle == NULL) {
		wlr_log_errno(WLR_ERROR, "Unable to create seat");
		return -1;
	}

	const char *seat_name = libseat_seat_name(session->seat_handle);
	if (seat_name == NULL) {
		wlr_log_errno(WLR_ERROR, "Unable to get seat info");
		goto error;
	}
	snprintf(session->seat, sizeof(session->seat), "%s", seat_name);

	session->libseat_event = wl_event_loop_add_fd(event_loop, libseat_get_fd(session->seat_handle),
		WL_EVENT_READABLE, libseat_event, session);
	if (session->libseat_event == NULL) {
		wlr_log(WLR_ERROR, "Failed to create libseat event source");
		goto error;
	}

	// We may have received enable_seat immediately after the open_seat result,
	// so, dispatch once without timeout to speed up activation.
	if (libseat_dispatch(session->seat_handle, 0) == -1) {
		wlr_log_errno(WLR_ERROR, "libseat dispatch failed");
		goto error_dispatch;
	}

	wlr_log(WLR_INFO, "Successfully loaded libseat session");
	return 0;

error_dispatch:
	wl_event_source_remove(session->libseat_event);
	session->libseat_event = NULL;
error:
	libseat_close_seat(session->seat_handle);
	session->seat_handle = NULL;
	return -1;
}

static void libseat_session_finish(struct wlr_session *session) {
	libseat_close_seat(session->seat_handle);
	wl_event_source_remove(session->libseat_event);
	session->seat_handle = NULL;
	session->libseat_event = NULL;
}

static void handle_event_loop_destroy(struct wl_listener *listener, void *data) {
	struct wlr_session *session =
		wl_container_of(listener, session, event_loop_destroy);
	wlr_session_destroy(session);
}

struct wlr_session *wlr_session_create(struct wl_event_loop *event_loop) {
	struct wlr_session *session = calloc(1, sizeof(*session));
	if (!session) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	session->event_loop = event_loop;
	wl_signal_init(&session->events.active);
	wl_signal_init(&session->events.add_drm_card);
	wl_signal_init(&session->events.destroy);
	wl_list_init(&session->devices);

	if (libseat_session_init(session, event_loop) == -1) {
		wlr_log(WLR_ERROR, "Failed to load session backend");
		goto error_open;
	}

#if HAVE_UDEV
	session->device_manager = wlr_udev_device_manager_create(session);
#else
	session->device_manager = wlr_linux_device_manager_create(session);
#endif
	if (session->device_manager == NULL) {
		wlr_log(WLR_ERROR, "Failed to create device manager");
		goto error_session;
	}

	session->event_loop_destroy.notify = handle_event_loop_destroy;
	wl_event_loop_add_destroy_listener(event_loop, &session->event_loop_destroy);

	return session;

error_session:
	libseat_session_finish(session);
error_open:
	free(session);
	return NULL;
}

void wlr_session_destroy(struct wlr_session *session) {
	if (!session) {
		return;
	}

	wl_signal_emit_mutable(&session->events.destroy, session);

	assert(wl_list_empty(&session->events.active.listener_list));
	assert(wl_list_empty(&session->events.add_drm_card.listener_list));
	assert(wl_list_empty(&session->events.destroy.listener_list));

	wl_list_remove(&session->event_loop_destroy.link);

	device_manager_destroy(session->device_manager);

	struct wlr_device *dev, *tmp_dev;
	wl_list_for_each_safe(dev, tmp_dev, &session->devices, link) {
		wlr_session_close_file(session, dev);
	}

	libseat_session_finish(session);
	free(session);
}

struct wlr_device *wlr_session_open_file(struct wlr_session *session,
		const char *path) {
	int fd;
	int device_id = libseat_open_device(session->seat_handle, path, &fd);
	if (device_id == -1) {
		wlr_log_errno(WLR_ERROR, "Failed to open device: '%s'", path);
		return NULL;
	}

	struct wlr_device *dev = malloc(sizeof(*dev));
	if (!dev) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		goto error;
	}

	struct stat st;
	if (fstat(fd, &st) < 0) {
		wlr_log_errno(WLR_ERROR, "Stat failed");
		goto error;
	}

	dev->fd = fd;
	dev->dev = st.st_rdev;
	dev->device_id = device_id;
	wl_signal_init(&dev->events.change);
	wl_signal_init(&dev->events.remove);
	wl_list_insert(&session->devices, &dev->link);

	return dev;

error:
	libseat_close_device(session->seat_handle, device_id);
	free(dev);
	close(fd);
	return NULL;
}

void wlr_session_close_file(struct wlr_session *session,
		struct wlr_device *dev) {
	if (libseat_close_device(session->seat_handle, dev->device_id) == -1) {
		wlr_log_errno(WLR_ERROR, "Failed to close device %d", dev->device_id);
	}

	assert(wl_list_empty(&dev->events.change.listener_list));
	// TODO: assert that the "remove" listener list is empty as well. Listeners
	// will typically call wlr_session_close_file() in response, and
	// wl_signal_emit_mutable() installs two phantom listeners, so we'd count
	// these two.

	close(dev->fd);
	wl_list_remove(&dev->link);
	free(dev);
}

bool wlr_session_change_vt(struct wlr_session *session, unsigned vt) {
	if (!session) {
		return false;
	}
	return libseat_switch_session(session->seat_handle, vt) == 0;
}

/* Tests if 'path' is KMS compatible by trying to open it. Returns the opened
 * device on success. */
struct wlr_device *session_open_if_kms(struct wlr_session *restrict session,
		const char *restrict path) {
	if (!path) {
		return NULL;
	}

	struct wlr_device *dev = wlr_session_open_file(session, path);
	if (!dev) {
		return NULL;
	}

	if (!drmIsKMS(dev->fd)) {
		wlr_log(WLR_DEBUG, "Ignoring '%s': not a KMS device", path);
		wlr_session_close_file(session, dev);
		return NULL;
	}

	return dev;
}

static ssize_t explicit_find_gpus(struct wlr_session *session,
		size_t ret_len, struct wlr_device *ret[static ret_len], const char *str) {
	char *gpus = strdup(str);
	if (!gpus) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return -1;
	}

	size_t i = 0;
	char *save;
	char *ptr = strtok_r(gpus, ":", &save);
	do {
		if (i >= ret_len) {
			break;
		}

		ret[i] = session_open_if_kms(session, ptr);
		if (!ret[i]) {
			wlr_log(WLR_ERROR, "Unable to open %s as KMS device", ptr);
		} else {
			++i;
		}
	} while ((ptr = strtok_r(NULL, ":", &save)));

	free(gpus);
	return i;
}

struct find_gpus_add_handler {
	bool added;
	struct wl_listener listener;
};

static void find_gpus_handle_add(struct wl_listener *listener, void *data) {
	struct find_gpus_add_handler *handler =
		wl_container_of(listener, handler, listener);
	handler->added = true;
}

ssize_t wlr_session_find_gpus(struct wlr_session *session,
		size_t ret_len, struct wlr_device **ret) {
	const char *explicit = getenv("WLR_DRM_DEVICES");
	if (explicit) {
		wlr_log(WLR_INFO, "Opening fixed list of KMS devices from WLR_DRM_DEVICES: %s", explicit);
		return explicit_find_gpus(session, ret_len, ret, explicit);
	}

	ssize_t n = device_manager_find_drm_cards(session->device_manager, ret_len, ret);
	if (n != 0) {
		return n;
	}

	wlr_log(WLR_INFO, "Waiting for a KMS device");

	struct find_gpus_add_handler handler = {0};
	handler.listener.notify = find_gpus_handle_add;
	wl_signal_add(&session->events.add_drm_card, &handler.listener);

	int64_t started_at = get_current_time_msec();
	int64_t timeout = WAIT_GPU_TIMEOUT;
	while (!handler.added) {
		int ret = wl_event_loop_dispatch(session->event_loop, (int)timeout);
		if (ret < 0) {
			wlr_log_errno(WLR_ERROR, "Failed to wait for KMS device: "
				"wl_event_loop_dispatch failed");
			return -1;
		}

		int64_t now = get_current_time_msec();
		if (now >= started_at + WAIT_GPU_TIMEOUT) {
			break;
		}
		timeout = started_at + WAIT_GPU_TIMEOUT - now;
	}

	wl_list_remove(&handler.listener.link);

	return device_manager_find_drm_cards(session->device_manager, ret_len, ret);
}
