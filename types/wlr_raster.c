#include <assert.h>
#include <pixman.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_raster.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/util/addon.h>
#include "types/wlr_buffer.h"

static void raster_handle_buffer_release(struct wl_listener *listener, void *data) {
	struct wlr_raster *raster = wl_container_of(listener, raster, buffer_release);
	raster->buffer = NULL;
	wl_list_remove(&raster->buffer_release.link);
	wl_list_init(&raster->buffer_release.link);
}

struct wlr_raster *wlr_raster_create(struct wlr_buffer *buffer,
		const struct wlr_raster_create_options *options) {
	struct wlr_raster *raster = calloc(1, sizeof(*raster));
	if (!raster) {
		return NULL;
	}

	wl_signal_init(&raster->events.destroy);

	assert(buffer);
	raster->opaque = buffer_is_opaque(buffer);
	raster->width = buffer->width;
	raster->height = buffer->height;
	raster->buffer = buffer;

	raster->n_locks = 1;

	raster->buffer_release.notify = raster_handle_buffer_release;
	wl_signal_add(&raster->buffer->events.release, &raster->buffer_release);

	if (options && options->wait_timeline) {
		raster->wait_timeline = wlr_drm_syncobj_timeline_ref(options->wait_timeline);
		raster->wait_point = options->wait_point;
	}

	return raster;
}

static void raster_consider_destroy(struct wlr_raster *raster) {
	if (raster->n_locks > 0) {
		return;
	}

	wl_signal_emit_mutable(&raster->events.destroy, NULL);

	if (raster->texture) {
		wl_list_remove(&raster->renderer_destroy.link);
		wlr_texture_destroy(raster->texture);
	}

	wl_list_remove(&raster->buffer_release.link);
	wlr_drm_syncobj_timeline_unref(raster->wait_timeline);
	free(raster);
}

struct wlr_raster *wlr_raster_lock(struct wlr_raster *raster) {
	raster->n_locks++;
	return raster;
}

void wlr_raster_unlock(struct wlr_raster *raster) {
	if (!raster) {
		return;
	}

	assert(raster->n_locks > 0);

	raster->n_locks--;
	raster_consider_destroy(raster);
}

static void raster_detach(struct wlr_raster *raster, struct wlr_texture *texture) {
	assert(texture);
	assert(raster->texture == texture);

	wl_list_remove(&raster->renderer_destroy.link);
	raster->texture = NULL;
}

static void handle_renderer_destroy(struct wl_listener *listener, void *data) {
	struct wlr_raster *raster = wl_container_of(listener, raster, renderer_destroy);
	raster_detach(raster, raster->texture);
}

static void raster_attach(struct wlr_raster *raster, struct wlr_texture *texture) {
	assert(texture->width == raster->width && texture->height == raster->height);
	assert(!raster->texture);

	raster->renderer_destroy.notify = handle_renderer_destroy;
	wl_signal_add(&texture->renderer->events.destroy, &raster->renderer_destroy);

	raster->texture = texture;
}

struct wlr_texture *wlr_raster_obtain_texture(struct wlr_raster *raster,
		struct wlr_renderer *renderer) {
	if (raster->texture) {
		assert(raster->texture->renderer == renderer);
		return raster->texture;
	}

	assert(raster->buffer);

	struct wlr_client_buffer *client_buffer =
		wlr_client_buffer_get(raster->buffer);
	if (client_buffer != NULL) {
		return client_buffer->texture;
	}

	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, raster->buffer);
	if (texture) {
		raster_attach(raster, texture);
	}

	return texture;
}

struct raster_update_state {
	struct wlr_buffer *buffer;
	pixman_region32_t damage;

	struct wlr_raster *new_raster;
	struct wlr_raster *old_raster;

	struct wl_listener old_raster_destroy;
	struct wl_listener new_raster_destroy;
	struct wl_listener buffer_release;
};

static void destroy_raster_update_state(struct raster_update_state *state) {
	wl_list_remove(&state->old_raster_destroy.link);
	wl_list_remove(&state->new_raster_destroy.link);
	wl_list_remove(&state->buffer_release.link);
	pixman_region32_fini(&state->damage);
	free(state);
}

static void raster_update_handle_new_raster_destroy(struct wl_listener *listener, void *data) {
	struct raster_update_state *state = wl_container_of(listener, state, new_raster_destroy);
	destroy_raster_update_state(state);
}

static void raster_update_handle_old_raster_destroy(struct wl_listener *listener, void *data) {
	struct raster_update_state *state = wl_container_of(listener, state, old_raster_destroy);

	// if the new raster already has a texture, there's nothing we can do to help.
	if (state->new_raster->texture) {
		assert(state->new_raster->texture->renderer == state->old_raster->texture->renderer);
		destroy_raster_update_state(state);
		return;
	}

	struct wlr_texture *texture = state->old_raster->texture;
	if (!texture) {
		destroy_raster_update_state(state);
		return;
	}

	if (wlr_texture_update_from_buffer(texture, state->buffer, &state->damage)) {
		raster_detach(state->old_raster, texture);
		raster_attach(state->new_raster, texture);
	}

	destroy_raster_update_state(state);
}

static void raster_update_handle_buffer_release(struct wl_listener *listener, void *data) {
	struct raster_update_state *state = wl_container_of(listener, state, buffer_release);
	destroy_raster_update_state(state);
}

static struct wlr_raster *raster_update(struct wlr_raster *raster,
		struct wlr_buffer *buffer, const pixman_region32_t *damage,
		const struct wlr_raster_create_options *options) {
	struct raster_update_state *state = calloc(1, sizeof(*state));
	if (!state) {
		return NULL;
	}

	struct wlr_raster *new_raster = wlr_raster_create(buffer, options);
	if (!new_raster) {
		free(state);
		return NULL;
	}

	state->old_raster_destroy.notify = raster_update_handle_old_raster_destroy;
	wl_signal_add(&raster->events.destroy, &state->old_raster_destroy);
	state->new_raster_destroy.notify = raster_update_handle_new_raster_destroy;
	wl_signal_add(&new_raster->events.destroy, &state->new_raster_destroy);
	state->buffer_release.notify = raster_update_handle_buffer_release;
	wl_signal_add(&buffer->events.release, &state->buffer_release);

	state->new_raster = new_raster;
	state->old_raster = raster;
	state->buffer = buffer;

	pixman_region32_init(&state->damage);
	pixman_region32_copy(&state->damage, damage);

	return new_raster;
}

struct surface_raster {
	struct wlr_raster *raster;
	struct wlr_surface *surface;

	struct wlr_addon addon;

	struct wl_listener buffer_prerelease;

	bool locking_buffer;
};

static void surface_raster_drop_raster(struct surface_raster *surface_raster) {
	if (surface_raster->locking_buffer) {
		wlr_buffer_unlock(surface_raster->raster->buffer);
		surface_raster->locking_buffer = false;
	}

	wlr_raster_unlock(surface_raster->raster);
	surface_raster->raster = NULL;
}

static void surface_raster_destroy(struct surface_raster *surface_raster) {
	surface_raster_drop_raster(surface_raster);

	wl_list_remove(&surface_raster->buffer_prerelease.link);
	wlr_addon_finish(&surface_raster->addon);
	free(surface_raster);
}

static void surface_raster_handle_addon_destroy(struct wlr_addon *addon) {
	struct surface_raster *surface_raster = wl_container_of(addon, surface_raster, addon);
	surface_raster_destroy(surface_raster);
}

static void surface_raster_handle_buffer_prerelease(struct wl_listener *listener, void *data) {
	struct surface_raster *surface_raster =
		wl_container_of(listener, surface_raster, buffer_prerelease);
	struct wlr_raster *raster = surface_raster->raster;

	struct wlr_surface_output *output;
	wl_list_for_each(output, &surface_raster->surface->current_outputs, link) {
		wlr_raster_obtain_texture(raster, output->output->renderer);
	}

	// if there was a failed texture upload, keep on locking the buffer
	if (!raster->texture) {
		wlr_buffer_lock(raster->buffer);
		surface_raster->locking_buffer = true;
	}

	wl_list_remove(&surface_raster->buffer_prerelease.link);
	wl_list_init(&surface_raster->buffer_prerelease.link);
}

const struct wlr_addon_interface surface_raster_addon_impl = {
	.name = "wlr_raster_surface",
	.destroy = surface_raster_handle_addon_destroy,
};

static struct surface_raster *get_surface_raster(struct wlr_surface *surface) {
	struct wlr_addon *addon = wlr_addon_find(&surface->addons, NULL,
		&surface_raster_addon_impl);
	if (!addon) {
		return NULL;
	}

	struct surface_raster *surface_raster = wl_container_of(addon, surface_raster, addon);
	return surface_raster;
}

// Because wlr_raster doesn't lock the buffer itself, we need something extra
// to keep client buffer locked when operating in legacy mode.
struct client_buffer_compat {
	struct wlr_client_buffer *buffer;
	struct wl_listener destroy;
};

static void client_buffer_compat_raster_destroy(struct wl_listener *listener, void *data) {
	struct client_buffer_compat *compat = wl_container_of(listener, compat, destroy);

	wlr_buffer_unlock(&compat->buffer->base);
	wl_list_remove(&compat->destroy.link);
	free(compat);
}

struct wlr_raster *wlr_raster_from_surface(struct wlr_surface *surface) {
	struct wlr_linux_drm_syncobj_surface_v1_state *syncobj_surface_state =
		wlr_linux_drm_syncobj_v1_get_surface_state(surface);

	struct wlr_raster_create_options options = {0};
	if (syncobj_surface_state) {
		options.wait_timeline = syncobj_surface_state->acquire_timeline;
		options.wait_point = syncobj_surface_state->acquire_point;
	}

	if (surface->compositor->renderer) {
		// use legacy wlr_client_buffer
		if (!surface->buffer) {
			return NULL;
		}

		struct client_buffer_compat *compat = calloc(1, sizeof(*compat));
		if (!compat) {
			return NULL;
		}

		struct wlr_raster *raster = wlr_raster_create(&surface->buffer->base, &options);
		if (!raster) {
			free(compat);
			return NULL;
		}

		compat->destroy.notify = client_buffer_compat_raster_destroy;
		wl_signal_add(&raster->events.destroy, &compat->destroy);

		compat->buffer = surface->buffer;
		wlr_buffer_lock(&surface->buffer->base);

		return raster;
	}

	struct surface_raster *surface_raster = get_surface_raster(surface);
	if (!surface_raster) {
		surface_raster = calloc(1, sizeof(*surface_raster));
		if (!surface_raster) {
			return NULL;
		}

		surface_raster->surface = surface;

		wlr_addon_init(&surface_raster->addon, &surface->addons, NULL,
			&surface_raster_addon_impl);

		surface_raster->buffer_prerelease.notify = surface_raster_handle_buffer_prerelease;
		wl_list_init(&surface_raster->buffer_prerelease.link);
	}

	if (!surface->current.buffer) {
		// surface is mapped but it hasn't committed a new buffer. We need to keep
		// using the old one
		if (wlr_surface_has_buffer(surface)) {
			if (surface_raster->raster) {
				return wlr_raster_lock(surface_raster->raster);
			} else {
				return NULL;
			}
		}

		wl_list_remove(&surface_raster->buffer_prerelease.link);
		wl_list_init(&surface_raster->buffer_prerelease.link);

		surface_raster_drop_raster(surface_raster);

		return NULL;
	}

	struct wlr_raster *raster;
	if (surface_raster->raster) {
		// make sure we haven't already seen this buffer
		if (surface_raster->raster->buffer == surface->current.buffer) {
			return wlr_raster_lock(surface_raster->raster);
		}

		raster = raster_update(surface_raster->raster,
			surface->current.buffer, &surface->buffer_damage, &options);
	} else {
		raster = wlr_raster_create(surface->current.buffer, &options);
	}

	if (!raster) {
		return NULL;
	}

	surface_raster_drop_raster(surface_raster);
	surface_raster->raster = wlr_raster_lock(raster);

	wl_list_remove(&surface_raster->buffer_prerelease.link);
	wl_signal_add(&surface->current.buffer->events.prerelease, &surface_raster->buffer_prerelease);

	wlr_surface_consume(surface);
	return raster;
}
