#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend.h>
#include <wlr/backend/multi.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_manager.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/util/log.h>

static void output_manager_backend_finish(
		struct wlr_output_manager_backend *backend) {
	wlr_allocator_destroy(backend->allocator);
	wlr_renderer_destroy(backend->renderer);
	wl_list_remove(&backend->backend_destroy.link);
	wl_list_remove(&backend->renderer_lost.link);
	wl_list_remove(&backend->link);
}

static void output_manager_handle_backend_destroy(
		struct wl_listener *listener, void *data) {
	struct wlr_output_manager_backend *backend =
		wl_container_of(listener, backend, backend_destroy);

 	output_manager_backend_finish(backend);

	if (backend == &backend->manager->primary) {
		*backend = (struct wlr_output_manager_backend){0};
	} else {
		free(backend);
	}
}

static void output_manager_handle_renderer_lost(
		struct wl_listener *listener, void *data) {
	struct wlr_output_manager_backend *backend =
		wl_container_of(listener, backend, renderer_lost);

	wlr_log(WLR_INFO, "Attempting renderer recovery after GPU reset!");

	struct wlr_renderer *renderer = wlr_renderer_autocreate(backend->backend);
	if (!renderer) {
		wlr_log(WLR_ERROR, "Could not create a new renderer after GPU reset");
		return;
	}

	struct wlr_allocator *allocator =
		wlr_allocator_autocreate(backend->backend, renderer);
	if (!allocator) {
		wlr_log(WLR_ERROR, "Could not create a new allocator after GPU reset");
		wlr_renderer_destroy(renderer);
		return;
	}

	wlr_log(WLR_INFO, "Created new renderer and allocator after reset. Attempting to swap...");

	struct wlr_renderer *old_renderer = backend->renderer;
	struct wlr_allocator *old_allocator = backend->allocator;
	backend->renderer = renderer;
	backend->allocator = allocator;

	wl_signal_add(&backend->renderer->events.lost, &backend->renderer_lost);

	// Only destroy the old state once we signal a recovery to avoid the old
	// state being referenced during its destruction.
	wlr_allocator_destroy(old_allocator);
	wlr_renderer_destroy(old_renderer);

	wl_signal_emit_mutable(&backend->events.recovery, NULL);
}

static bool output_manager_backend_init(struct wlr_output_manager *manager,
		struct wlr_output_manager_backend *backend, struct wlr_backend *wlr_backend) {
	backend->renderer = wlr_renderer_autocreate(wlr_backend);
	if (!backend->renderer) {
		return false;
	}

	backend->allocator = wlr_allocator_autocreate(wlr_backend,
		manager->primary.renderer);
	if (!backend->allocator) {
		wlr_renderer_destroy(manager->primary.renderer);
		return false;
	}

	backend->manager = manager;
	backend->backend = wlr_backend;
	backend->locks = 1;

	wl_signal_init(&backend->events.recovery);

	backend->backend_destroy.notify = output_manager_handle_backend_destroy;
	wl_signal_add(&wlr_backend->events.destroy, &backend->backend_destroy);

	backend->renderer_lost.notify = output_manager_handle_renderer_lost;
	wl_signal_add(&backend->renderer->events.lost, &backend->renderer_lost);

	wl_list_insert(&manager->backends, &backend->link);
	return true;
}

struct multi_backend_iterator_data {
	struct wlr_output_manager *manager;
	bool primary;
};

static void multi_backend_iterator(struct wlr_backend *wlr_backend, void *_data) {
	struct multi_backend_iterator_data *data = _data;

	// Use the first device as the primary
	if (data->primary) {
		if (!output_manager_backend_init(data->manager, &data->manager->primary, wlr_backend)) {
			return;
		}
		data->primary = false;
		return;
	}

	struct wlr_output_manager_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		return;
	}

	if (!output_manager_backend_init(data->manager, backend, wlr_backend)) {
		free(backend);
		return;
	}
}

bool wlr_output_manager_init(struct wlr_output_manager *manager,
		struct wlr_backend *backend) {
	*manager = (struct wlr_output_manager){0};
	wl_list_init(&manager->backends);

	struct multi_backend_iterator_data iter_data = {
		.manager = manager,
		.primary = true,
	};

	if (wlr_backend_is_multi(backend)) {
		wlr_multi_for_each_backend(backend, multi_backend_iterator, &iter_data);
	} else {
		multi_backend_iterator(backend, &iter_data);
	}

	return !wl_list_empty(&manager->backends);
}

void wlr_output_manager_finish(struct wlr_output_manager *manager) {
	struct wlr_output_manager_backend *backend;
	wl_list_for_each(backend, &manager->backends, link) {
		output_manager_backend_finish(backend);
	}
}

struct wlr_output_manager_backend *wlr_output_manager_lock_backend(
		struct wlr_output_manager *manager, struct wlr_backend *wlr_backend) {
	assert(!wlr_backend_is_multi(wlr_backend));

	struct wlr_output_manager_backend *backend;
	wl_list_for_each(backend, &manager->backends, link) {
		if (backend->backend == wlr_backend) {
			backend->locks++;
			return backend;
		}
	}

	backend = calloc(1, sizeof(*backend));
	if (!backend) {
		return NULL;
	}

	if (!output_manager_backend_init(manager, backend, wlr_backend)) {
		free(backend);
		return NULL;
	}

	return backend;
}

void wlr_output_manager_unlock_backend(struct wlr_output_manager_backend *backend) {
	assert(backend->locks > 0);
	backend->locks--;

	if (backend->locks != 0) {
		return;
	}

	output_manager_backend_finish(backend);
	free(backend);
}

struct output_manager_output {
	struct wlr_output_manager_backend *backend;
	struct wlr_output *output;
	struct wlr_addon addon;

	// recover from GPU resets
	struct wl_listener backend_recovery;
};

static void manager_output_handle_output_destroy(struct wlr_addon *addon) {
	struct output_manager_output *manager_output =
		wl_container_of(addon, manager_output, addon);
	wlr_addon_finish(&manager_output->addon);
	wlr_output_manager_unlock_backend(manager_output->backend);
	wl_list_remove(&manager_output->backend_recovery.link);
	free(manager_output);
}

static const struct wlr_addon_interface output_addon_impl = {
	.name = "wlr_output_manager_output",
	.destroy = manager_output_handle_output_destroy,
};

static void output_handle_recovery(struct wl_listener *listener, void *data) {
	struct output_manager_output *manager = wl_container_of(listener, manager, backend_recovery);

	// we lost the context, create a new renderer and switch everything out.
	wlr_output_init_render(manager->output, manager->backend->allocator,
		manager->backend->renderer);
}

bool wlr_output_manager_init_output(struct wlr_output_manager *manager,
		struct wlr_output *output) {
	struct output_manager_output *manager_output = calloc(1, sizeof(*manager_output));
	if (!manager_output) {
		return false;
	}

	manager_output->output = output;

	manager_output->backend = wlr_output_manager_lock_backend(
		manager, output->backend);
	if (!manager_output->backend) {
		free(manager_output);
		return false;
	}

	wlr_addon_init(&manager_output->addon, &output->addons, manager, &output_addon_impl);

	manager_output->backend_recovery.notify = output_handle_recovery;
	wl_signal_add(&manager_output->backend->events.recovery, &manager_output->backend_recovery);

	wlr_output_init_render(output, manager_output->backend->allocator,
		manager_output->backend->renderer);

	return true;
}

bool wlr_output_manager_init_wl_shm(struct wlr_output_manager *manager,
		struct wl_display *wl_display) {
	size_t shm_formats_len = 0;
	uint32_t *shm_formats = NULL;

	struct wlr_output_manager_backend *backend;
	wl_list_for_each(backend, &manager->backends, link) {
		const struct wlr_drm_format_set *format_set = wlr_renderer_get_texture_formats(
			backend->renderer, WLR_BUFFER_CAP_DATA_PTR);
		if (format_set == NULL || format_set->len == 0) {
			wlr_log(WLR_ERROR, "Failed to initialize wl_shm: "
				"cannot get renderer formats");
			return NULL;
		}

		if (!shm_formats) {
			shm_formats = malloc(format_set->len * sizeof(uint32_t));
			if (!shm_formats) {
				wlr_log(WLR_INFO, "Cannot allocate a format set");
				return false;
			}

			for (size_t i = 0; i < format_set->len; i++) {
				shm_formats[i] = format_set->formats[i].format;
			}

			shm_formats_len = format_set->len;
			continue;
		}

		// interset the format lists - null out any formats from the shm_formats
		// list when the current renderer doesn't have the format as well.
		for (size_t i = 0; i < shm_formats_len; i++) {
			if (shm_formats[i] == 0) {
				continue;
			}

			bool found = false;
			for (size_t j = 0; j < format_set->len; j++) {
				if (format_set->formats[j].format == shm_formats[i]) {
					found = true;
					break;
				}
			}

			if (!found) {
				shm_formats[i] = 0;
			}
		}
	}

	// clear out all null formats from the format list
	size_t j = 0;
	for (size_t i = 0; i < shm_formats_len; i++) {
		if (shm_formats[i] != 0) {
			shm_formats[j++] = shm_formats[i];
		}
	}
	shm_formats_len = j;

	bool ok = wlr_shm_create(wl_display, 1, shm_formats, shm_formats_len);
	free(shm_formats);
	return ok;
}

bool wlr_output_manager_init_wl_display(struct wlr_output_manager *manager,
		struct wl_display *wl_display) {
	if (!wlr_output_manager_init_wl_shm(manager, wl_display)) {
		return false;
	}

	struct wlr_renderer *r = manager->primary.renderer;
	if (wlr_renderer_get_texture_formats(r, WLR_BUFFER_CAP_DMABUF) != NULL) {
		if (wlr_renderer_get_drm_fd(r) >= 0) {
			if (wlr_drm_create(wl_display, r) == NULL) {
				return false;
			}
		} else {
			wlr_log(WLR_INFO, "Cannot get renderer DRM FD, disabling wl_drm");
		}

		if (wlr_linux_dmabuf_v1_create_with_renderer(wl_display, 4, r) == NULL) {
			return false;
		}
	}

	return true;
}
