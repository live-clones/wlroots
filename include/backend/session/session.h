#ifndef BACKEND_SESSION_SESSION_H
#define BACKEND_SESSION_SESSION_H

#include <stdbool.h>
#include <sys/types.h>

struct wl_display;
struct wlr_session;

struct wlr_session *libseat_session_create(struct wl_display *disp);
void libseat_session_destroy(struct wlr_session *base);
int libseat_session_open_device(struct wlr_session *base, const char *path);
void libseat_session_close_device(struct wlr_session *base, int fd);
bool libseat_change_vt(struct wlr_session *base, unsigned vt);

void session_init(struct wlr_session *session);

struct wlr_device *session_open_if_kms(struct wlr_session *restrict session,
	const char *restrict path);
struct wlr_device *session_find_device_by_devid(struct wlr_session *session, dev_t devid);

struct wlr_device_manager {
	struct wlr_session *session;

	const struct wlr_device_manager_impl *impl;
};

struct wlr_device_manager_impl {
	void (*destroy)(struct wlr_device_manager *manager);
	ssize_t (*find_drm_cards)(struct wlr_device_manager *manager,
		size_t ret_cap, struct wlr_device **ret);
};

void wlr_device_manager_init(struct wlr_device_manager *manager,
	const struct wlr_device_manager_impl *impl, struct wlr_session *session);
void wlr_device_manager_finish(struct wlr_device_manager *manager);

#endif
