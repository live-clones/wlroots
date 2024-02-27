#include <assert.h>
#include <drm_fourcc.h>
#include <pixman.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_raster.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>
#include "render/drm_format_set.h"
#include "render/wlr_renderer.h"
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

	wl_list_init(&raster->sources);
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

static void raster_source_destroy(struct wlr_raster_source *source) {
	wl_list_remove(&source->link);
	wl_list_remove(&source->renderer_destroy.link);
	wl_list_remove(&source->allocator_destroy.link);
	free(source);
}

static void raster_consider_destroy(struct wlr_raster *raster) {
	if (raster->n_locks > 0) {
		return;
	}

	wl_signal_emit_mutable(&raster->events.destroy, NULL);

	struct wlr_raster_source *source, *source_tmp;
	wl_list_for_each_safe(source, source_tmp, &raster->sources, link) {
		wlr_texture_destroy(source->texture);
		raster_source_destroy(source);
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
	if (!texture) {
		return;
	}

	struct wlr_raster_source *source;
	wl_list_for_each(source, &raster->sources, link) {
		if (source->texture == texture) {
			raster_source_destroy(source);
			return;
		}
	}

	assert(false);
}

static void handle_renderer_destroy(struct wl_listener *listener, void *data) {
	struct wlr_raster_source *source = wl_container_of(listener, source, renderer_destroy);
	raster_source_destroy(source);
}

static void handle_allocator_destroy(struct wl_listener *listener, void *data) {
	struct wlr_raster_source *source = wl_container_of(listener, source, allocator_destroy);
	source->allocator = NULL;
	wl_list_remove(&source->allocator_destroy.link);
	wl_list_init(&source->allocator_destroy.link);
}

static void raster_attach_with_allocator(struct wlr_raster *raster,
		struct wlr_texture *texture, struct wlr_allocator *allocator) {
	assert(texture->width == raster->width && texture->height == raster->height);

	struct wlr_raster_source *source;
	wl_list_for_each(source, &raster->sources, link) {
		assert(source->texture != texture);
	}

	source = calloc(1, sizeof(*source));
	if (!source) {
		 return;
	}

	source->renderer_destroy.notify = handle_renderer_destroy;
	wl_signal_add(&texture->renderer->events.destroy, &source->renderer_destroy);

	if (allocator) {
		source->allocator_destroy.notify = handle_allocator_destroy;
		wl_signal_add(&allocator->events.destroy, &source->allocator_destroy);
	} else {
		wl_list_init(&source->allocator_destroy.link);
	}

	wl_list_insert(&raster->sources, &source->link);
	source->texture = texture;
	source->allocator = allocator;
}

static void raster_attach(struct wlr_raster *raster, struct wlr_texture *texture) {
	raster_attach_with_allocator(raster, texture, NULL);
}

static struct wlr_texture *wlr_raster_get_texture(struct wlr_raster *raster,
		struct wlr_renderer *renderer) {
	struct wlr_raster_source *source;
	wl_list_for_each(source, &raster->sources, link) {
		if (source->texture->renderer == renderer) {
			return source->texture;
		}
	}

	return NULL;
}

static bool compute_import_buffer_format(struct wlr_raster *raster, struct wlr_drm_format *drm_fmt,
		struct wlr_renderer *dst) {
	const struct wlr_drm_format_set *texture_formats =
		wlr_renderer_get_texture_formats(dst, WLR_BUFFER_CAP_DMABUF);
	if (!texture_formats) {
		wlr_log(WLR_ERROR, "Failed to get texture_formats");
		return NULL;
	}

	// For now, let's only use XRGB
	uint32_t fmt = raster->opaque ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_ARGB8888;
	const struct wlr_drm_format *drm_fmt_inv =
		wlr_drm_format_set_get(texture_formats, fmt);

	if (!wlr_drm_format_copy(drm_fmt, drm_fmt_inv)) {
		return false;
	}

	for (size_t i = 0; i < drm_fmt->len; i++) {
		uint64_t mod = drm_fmt->modifiers[i];
		if (mod != DRM_FORMAT_MOD_INVALID) {
			continue;
		}

		for (size_t j = i + 1; j < drm_fmt->len; j++) {
			drm_fmt->modifiers[j] = drm_fmt->modifiers[j + 1];
		}

		drm_fmt->len--;
		break;
	}

	return true;
}

static struct wlr_buffer *raster_try_blit(struct wlr_raster *raster,
		struct wlr_raster_source *source, struct wlr_renderer *dst) {
	if (!source->allocator) {
		return NULL;
	}

	wlr_log(WLR_DEBUG, "Attempting a multigpu blit through a GPU");

	struct wlr_renderer *src = source->texture->renderer;

	// The src needs to be able to render into this format
	const struct wlr_drm_format_set *render_formats =
		wlr_renderer_get_render_formats(src);
	if (!render_formats) {
		wlr_log(WLR_ERROR, "Failed to get render_formats");
		return NULL;
	}

	struct wlr_drm_format fmt = {0};
	if (!compute_import_buffer_format(raster, &fmt, dst)) {
		wlr_log(WLR_ERROR, "Could not find a common format modifiers for all GPUs");
		return NULL;
	}

	if (wlr_drm_format_intersect(&fmt, &fmt,
			wlr_drm_format_set_get(render_formats, fmt.format))) {
		wlr_drm_format_finish(&fmt);
		return NULL;
	}

	struct wlr_buffer *buffer = wlr_allocator_create_buffer(
		source->allocator, raster->width, raster->height, &fmt);
	wlr_drm_format_finish(&fmt);
	if (!buffer) {
		wlr_log(WLR_ERROR, "Failed to allocate multirenderer blit buffer");
		return NULL;
	}

	struct wlr_drm_syncobj_timeline *timeline = NULL;
	int drm_fd = wlr_renderer_get_drm_fd(src);
	if (src->features.timeline && drm_fd >= 0) {
		timeline = wlr_drm_syncobj_timeline_create(drm_fd);
	}

	const struct wlr_buffer_pass_options pass_options = {
		.signal_timeline = timeline,
		.signal_point = 1,
	};

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(src, buffer, &pass_options);
	if (!pass) {
		wlr_log(WLR_ERROR, "Failed to create a render pass");
		wlr_buffer_drop(buffer);
		return NULL;
	}

	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options) {
		.texture = source->texture,
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		.wait_timeline = timeline,
		.wait_point = 1,
	});

	wlr_drm_syncobj_timeline_unref(timeline);

	if (!wlr_render_pass_submit(pass)) {
		wlr_log(WLR_ERROR, "Failed to renedr to a multigpu blit buffer");
		wlr_buffer_drop(buffer);
		return NULL;
	}

	return buffer;
}

static struct wlr_texture *raster_try_texture_from_blit(struct wlr_raster *raster,
		struct wlr_renderer *renderer) {
	struct wlr_buffer *imported = NULL;

	struct wlr_raster_source *source;
	wl_list_for_each(source, &raster->sources, link) {
		imported = raster_try_blit(raster, source, renderer);
		if (imported) {
			break;
		}
	}

	if (!imported) {
		return NULL;
	}

	wlr_buffer_drop(imported);

	return wlr_texture_from_buffer(renderer, imported);
}

static struct wlr_texture *raster_try_cpu_copy(struct wlr_raster *raster,
		struct wlr_renderer *dst) {
	if (wl_list_empty(&raster->sources)) {
		return NULL;
	}

	wlr_log(WLR_DEBUG, "Performing multigpu blit through the CPU");
	struct wlr_texture *texture = NULL;

	uint32_t format = DRM_FORMAT_ARGB8888;
	uint32_t stride = raster->width * 4;
	void *data = malloc(stride * raster->height);
	if (!data) {
		return NULL;
	}

	struct wlr_raster_source *source;
	wl_list_for_each(source, &raster->sources, link) {
		if (!wlr_texture_read_pixels(source->texture, &(struct wlr_texture_read_pixels_options){
			.format = format,
			.stride = stride,
			.data = data,
		})) {
			wlr_log(WLR_ERROR, "Failed to read pixels");
			continue;
		}

		texture = wlr_texture_from_pixels(dst, format,
			stride, raster->width, raster->height, data);
		if (!texture) {
			wlr_log(WLR_ERROR, "Failed to upload texture from cpu data");
			continue;
		}

		break;
	}

	free(data);
	return texture;
}

struct wlr_texture *wlr_raster_obtain_texture_with_allocator(struct wlr_raster *raster,
		struct wlr_renderer *renderer, struct wlr_allocator *allocator) {
	struct wlr_texture *texture = wlr_raster_get_texture(raster, renderer);
	if (texture) {
		return texture;
	}

	if (raster->buffer) {
		struct wlr_client_buffer *client_buffer =
			wlr_client_buffer_get(raster->buffer);
		if (client_buffer != NULL) {
			return client_buffer->texture;
		}

		// if we have a buffer, try and import that
		texture = wlr_texture_from_buffer(renderer, raster->buffer);
		if (texture) {
			raster_attach_with_allocator(raster, texture, allocator);
			return texture;
		}
	}

	// try to blit using the textures already available to us
	texture = raster_try_texture_from_blit(raster, renderer);
	if (texture) {
		raster_attach_with_allocator(raster, texture, allocator);
		return texture;
	}

	// as a last resort we need to do a copy through the CPU
	texture = raster_try_cpu_copy(raster, renderer);
	if (texture) {
		raster_attach_with_allocator(raster, texture, allocator);
		return texture;
	}

	return NULL;
}

struct wlr_texture *wlr_raster_obtain_texture(struct wlr_raster *raster,
		struct wlr_renderer *renderer) {
	return wlr_raster_create_texture_with_allocator(raster, renderer, NULL);
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
	if (!wl_list_empty(&state->new_raster->sources)) {
		destroy_raster_update_state(state);
		return;
	}

	struct wlr_raster_source *source, *tmp_source;
	wl_list_for_each_safe(source, tmp_source, &state->old_raster->sources, link) {
		struct wlr_texture *texture = source->texture;
		if (wlr_texture_update_from_buffer(texture, state->buffer, &state->damage)) {
			raster_detach(state->old_raster, texture);
			raster_attach(state->new_raster, texture);
		}
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
	if (wl_list_empty(&raster->sources)) {
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

		// before we try to update the old raster, remove obsolete textures
		struct wlr_raster_source *source, *tmp_source;
		wl_list_for_each_safe(source, tmp_source, &surface_raster->raster->sources, link) {
			struct wlr_texture *texture = source->texture;

			bool found = false;
			struct wlr_surface_output *output;
			wl_list_for_each(output, &surface->current_outputs, link) {
				if (output->output->renderer == texture->renderer) {
					found = true;
					break;
				}
			}

			if (!found) {
				raster_detach(surface_raster->raster, texture);
				wlr_texture_destroy(texture);
			}
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
