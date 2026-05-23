#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include "backend/session/session.h"

struct drm_event_monitor {
	int fd;
};

// TODO: use kqueue NOTE_CHANGE mechanism
struct drm_event_monitor *monitor_drm_events(int *fd) {
	struct drm_event_monitor *ctx = calloc(1, sizeof(struct drm_event_monitor));
	if (!ctx) {
		return NULL;
	}
	ctx->fd = open("/dev/null", O_RDONLY);
	if (ctx->fd == -1) {
		free(ctx);
		return NULL;
	}
	*fd = ctx->fd;
	return ctx;
}
void drm_event_monitor_free(struct drm_event_monitor *ctx) {
	close(ctx->fd);
	free(ctx);
}

int receive_drm_device(struct drm_event_monitor *ctx,
		struct drm_event_content *ev) {
	return -1;
}

const char *get_device_seat(struct drm_event_monitor *ctx, const char *devnode) {
	return NULL;
}

bool is_drm_device_primary(struct drm_event_monitor *ctx, const char *devnode) {
	return false;
}
