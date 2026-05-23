#ifndef BACKEND_SESSION_SESSION_H
#define BACKEND_SESSION_SESSION_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <xf86drm.h>

// for some reason libdrm does not define MAX3, but has defs that depend on it
#define MAX3(a, b, c) ((a) > (b) && (a) > (c) ? (a) : \
	(b) > (c) ? (b) : (c))

struct drm_event_monitor;

enum drm_event_action {
	DRM_EVENT_ACTION_NONE,
	DRM_EVENT_ACTION_ADD,
	DRM_EVENT_ACTION_CHANGE,
	DRM_EVENT_ACTION_REMOVE,
};

struct drm_event_content {
	char devnode[DRM_NODE_NAME_MAX];
	dev_t devnum;
	enum drm_event_action type;
	int16_t conn_id;
	int16_t prop_id;
	bool has_lease;
};

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

struct drm_event_monitor *monitor_drm_events(int *fd);
void drm_event_monitor_free(struct drm_event_monitor *ctx);

int receive_drm_device(struct drm_event_monitor *ctx,
	struct drm_event_content *ev);

const char *get_device_seat(struct drm_event_monitor *ctx, const char *devnode);
bool is_drm_device_primary(struct drm_event_monitor *ctx, const char *devnode);

#endif
