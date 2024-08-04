#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/render/interface.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <xf86drm.h>

#include <wlr/config.h>

#if WLR_HAS_GLES2_RENDERER
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#endif

#if WLR_HAS_VULKAN_RENDERER
#include <wlr/render/vulkan.h>
#endif // WLR_HAS_VULKAN_RENDERER

#include "backend/backend.h"
#include "render/pixel_format.h"
#include "render/wlr_renderer.h"
#include "util/env.h"

void wlr_renderer_init(struct wlr_renderer *renderer,
		const struct wlr_renderer_impl *impl, uint32_t render_buffer_caps) {
	assert(impl->begin_buffer_pass);
	assert(impl->get_texture_formats);
	assert(render_buffer_caps != 0);

	*renderer = (struct wlr_renderer){
		.impl = impl,
		.render_buffer_caps = render_buffer_caps,
	};

	wl_signal_init(&renderer->events.destroy);
	wl_signal_init(&renderer->events.lost);
}

void wlr_renderer_destroy(struct wlr_renderer *r) {
	if (!r) {
		return;
	}

	wl_signal_emit_mutable(&r->events.destroy, r);

	if (r->impl && r->impl->destroy) {
		r->impl->destroy(r);
	} else {
		free(r);
	}
}

const struct wlr_drm_format_set *wlr_renderer_get_texture_formats(
		struct wlr_renderer *r, uint32_t buffer_caps) {
	return r->impl->get_texture_formats(r, buffer_caps);
}

const struct wlr_drm_format_set *wlr_renderer_get_render_formats(
		struct wlr_renderer *r) {
	if (!r->impl->get_render_formats) {
		return NULL;
	}
	return r->impl->get_render_formats(r);
}

bool wlr_renderer_init_wl_shm(struct wlr_renderer *r,
		struct wl_display *wl_display) {
	return wlr_shm_create_with_renderer(wl_display, 2, r) != NULL;
}

bool wlr_renderer_init_wl_display(struct wlr_renderer *r,
		struct wl_display *wl_display) {
	if (!wlr_renderer_init_wl_shm(r, wl_display)) {
		return false;
	}

	if (wlr_renderer_get_texture_formats(r, WLR_BUFFER_CAP_DMABUF) != NULL &&
			r->drm_dev_id != NULL &&
			wlr_linux_dmabuf_v1_create_with_renderer(wl_display, 4, r) == NULL) {
		return false;
	}

	return true;
}

static bool pick_drm_render_node(dev_t *dev_id) {
	uint32_t flags = 0;
	int devices_len = drmGetDevices2(flags, NULL, 0);
	if (devices_len < 0) {
		wlr_log(WLR_ERROR, "drmGetDevices2 failed: %s", strerror(-devices_len));
		return false;
	}
	drmDevice **devices = calloc(devices_len, sizeof(*devices));
	if (devices == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return false;
	}
	devices_len = drmGetDevices2(flags, devices, devices_len);
	if (devices_len < 0) {
		free(devices);
		wlr_log(WLR_ERROR, "drmGetDevices2 failed: %s", strerror(-devices_len));
		return false;
	}

	bool ok = false;
	for (int i = 0; i < devices_len; i++) {
		drmDevice *dev = devices[i];
		if (dev->available_nodes & (1 << DRM_NODE_RENDER)) {
			const char *name = dev->nodes[DRM_NODE_RENDER];
			wlr_log(WLR_DEBUG, "Picking DRM render node '%s'", name);
			struct stat st;
			if (stat(name, &st) != 0) {
				wlr_log_errno(WLR_ERROR, "stat() failed for %s", name);
				goto out;
			}
			*dev_id = st.st_rdev;
			break;
		}
	}
	if (!ok) {
		wlr_log(WLR_ERROR, "Failed to find any DRM render node");
	}

out:
	for (int i = 0; i < devices_len; i++) {
		drmFreeDevice(&devices[i]);
	}
	free(devices);

	return ok;
}

static bool dev_id_from_fd(int fd, dev_t *dev_id, bool *has_dev_id) {
	struct stat st;
	if (fstat(fd, &st) != 0) {
		wlr_log_errno(WLR_ERROR, "fstat() failed");
		return false;
	}
	*dev_id = st.st_rdev;
	*has_dev_id = true;
	return true;
}

static bool get_preferred_drm_dev_id(struct wlr_backend *backend, int drm_fd,
		dev_t *dev_id, bool *has_dev_id) {
	if (*has_dev_id) {
		return true;
	}

	// If the caller passed in a DRM FD, use that
	if (drm_fd >= 0) {
		return dev_id_from_fd(drm_fd, dev_id, has_dev_id);
	}

	// Allow the user to override the render node
	const char *render_name = getenv("WLR_RENDER_DRM_DEVICE");
	if (render_name != NULL) {
		wlr_log(WLR_INFO,
			"Opening DRM render node '%s' from WLR_RENDER_DRM_DEVICE",
			render_name);
		int drm_fd = open(render_name, O_RDWR | O_CLOEXEC);
		if (drm_fd < 0) {
			wlr_log_errno(WLR_ERROR, "Failed to open '%s'", render_name);
			return false;
		}
		if (drmGetNodeTypeFromFd(drm_fd) != DRM_NODE_RENDER) {
			wlr_log(WLR_ERROR, "'%s' is not a DRM render node", render_name);
			close(drm_fd);
			return false;
		}
		bool ok = dev_id_from_fd(drm_fd, dev_id, has_dev_id);
		close(drm_fd);
		return ok;
	}

	// Prefer the backend's DRM node, if any
	int backend_drm_fd = wlr_backend_get_drm_fd(backend);
	if (backend_drm_fd >= 0) {
		return dev_id_from_fd(backend_drm_fd, dev_id, has_dev_id);
	}

	// If the backend hasn't picked a DRM FD, but accepts DMA-BUFs, pick an
	// arbitrary render node
	uint32_t backend_caps = backend_get_buffer_caps(backend);
	if (backend_caps & WLR_BUFFER_CAP_DMABUF) {
		if (!pick_drm_render_node(dev_id)) {
			return false;
		}
		*has_dev_id = true;
		return true;
	}

	return false;
}

static void log_creation_failure(bool is_auto, const char *msg) {
	wlr_log(is_auto ? WLR_DEBUG : WLR_ERROR, "%s%s", msg, is_auto ? ". Skipping!" : "");
}

static bool has_render_node(struct wlr_backend *backend) {
	if (!backend) {
		return false;
	}

	int backend_drm_fd = wlr_backend_get_drm_fd(backend);
	if (backend_drm_fd < 0) {
		return false;
	}

	char *render_node = drmGetRenderDeviceNameFromFd(backend_drm_fd);
	bool has_render_node = render_node != NULL;
	free(render_node);

	return has_render_node;
}

static struct wlr_renderer *renderer_autocreate(struct wlr_backend *backend, int drm_fd) {
	const char *renderer_options[] = {
		"auto",
		"gles2",
		"vulkan",
		"pixman",
		NULL
	};

	const char *renderer_name = renderer_options[env_parse_switch("WLR_RENDERER", renderer_options)];
	bool is_auto = strcmp(renderer_name, "auto") == 0;
	struct wlr_renderer *renderer = NULL;

	dev_t drm_dev_id;
	bool has_drm_dev_id = false;
	(void)drm_dev_id;
	(void)has_drm_dev_id;
	(void)get_preferred_drm_dev_id;

	if (is_auto || strcmp(renderer_name, "gles2") == 0) {
		if (!get_preferred_drm_dev_id(backend, drm_fd, &drm_dev_id, &has_drm_dev_id)) {
			log_creation_failure(is_auto, "Cannot create GLES2 renderer: no DRM device available");
		} else {
#if WLR_HAS_GLES2_RENDERER
			renderer = wlr_gles2_renderer_create_with_drm_dev_id(drm_dev_id);
#else
			wlr_log(WLR_ERROR, "Cannot create GLES renderer: disabled at compile-time");
#endif
			if (renderer) {
				goto out;
			} else {
				log_creation_failure(is_auto, "Failed to create a GLES2 renderer");
			}
		}
	}

	if (strcmp(renderer_name, "vulkan") == 0) {
		if (!get_preferred_drm_dev_id(backend, drm_fd, &drm_dev_id, &has_drm_dev_id)) {
			log_creation_failure(is_auto, "Cannot create Vulkan renderer: no DRM device available");
		} else {
#if WLR_HAS_VULKAN_RENDERER
			renderer = wlr_vk_renderer_create_with_drm_dev_id(drm_dev_id);
#else
			wlr_log(WLR_ERROR, "Cannot create Vulkan renderer: disabled at compile-time");
#endif
			if (renderer) {
				goto out;
			} else {
				log_creation_failure(is_auto, "Failed to create a Vulkan renderer");
			}
		}
	}

	if ((is_auto && !has_render_node(backend)) || strcmp(renderer_name, "pixman") == 0) {
		renderer = wlr_pixman_renderer_create();
		if (renderer) {
			goto out;
		} else {
			log_creation_failure(is_auto, "Failed to create a pixman renderer");
		}
	}

out:
	if (renderer == NULL) {
		wlr_log(WLR_ERROR, "Could not initialize renderer");
	}
	return renderer;
}

struct wlr_renderer *renderer_autocreate_with_drm_fd(int drm_fd) {
	assert(drm_fd >= 0);

	return renderer_autocreate(NULL, drm_fd);
}

struct wlr_renderer *wlr_renderer_autocreate(struct wlr_backend *backend) {
	return renderer_autocreate(backend, -1);
}

struct wlr_render_pass *wlr_renderer_begin_buffer_pass(struct wlr_renderer *renderer,
		struct wlr_buffer *buffer, const struct wlr_buffer_pass_options *options) {
	struct wlr_buffer_pass_options default_options = {0};
	if (!options) {
		options = &default_options;
	}

	return renderer->impl->begin_buffer_pass(renderer, buffer, options);
}

struct wlr_render_timer *wlr_render_timer_create(struct wlr_renderer *renderer) {
	if (!renderer->impl->render_timer_create) {
		return NULL;
	}
	return renderer->impl->render_timer_create(renderer);
}

int wlr_render_timer_get_duration_ns(struct wlr_render_timer *timer) {
	if (!timer->impl->get_duration_ns) {
		return -1;
	}
	return timer->impl->get_duration_ns(timer);
}

void wlr_render_timer_destroy(struct wlr_render_timer *timer) {
	if (!timer->impl->destroy) {
		return;
	}
	timer->impl->destroy(timer);
}
