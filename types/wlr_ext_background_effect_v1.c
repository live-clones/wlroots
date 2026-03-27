#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_ext_background_effect_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>
#include "ext-background-effect-v1-protocol.h"

#define BACKGROUND_EFFECT_VERSION 1

struct wlr_ext_background_effect_manager_client {
	struct wl_resource *resource;
	struct wlr_ext_background_effect_manager_v1 *manager;
	struct wl_list link; // wlr_ext_background_effect_manager_v1.resources
};

static const struct ext_background_effect_surface_v1_interface surface_impl;
static const struct ext_background_effect_manager_v1_interface manager_impl;

static struct wlr_ext_background_effect_manager_client *manager_client_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_background_effect_manager_v1_interface, &manager_impl));

	return wl_resource_get_user_data(resource);
}

static struct wlr_ext_background_effect_surface_v1 *surface_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_background_effect_surface_v1_interface, &surface_impl));

	return wl_resource_get_user_data(resource);
}

static void surface_handle_commit(struct wl_listener *listener, void *data) {
	struct wlr_ext_background_effect_surface_v1 *surface =
		wl_container_of(listener, surface, surface_commit);

	wl_signal_emit_mutable(&surface->events.commit, surface);
}

static void surface_destroy(struct wlr_ext_background_effect_surface_v1 *surface) {
	if (surface == NULL) {
		return;
	}

	wl_signal_emit_mutable(&surface->events.destroy, surface);
	wl_list_remove(&surface->surface_commit.link);
	wlr_surface_synced_finish(&surface->synced);
	wlr_addon_finish(&surface->addon);
	wl_resource_set_user_data(surface->resource, NULL);
	free(surface);
}

static void surface_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_ext_background_effect_surface_v1 *surface = surface_from_resource(resource);
	surface_destroy(surface);
}

static void surface_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void surface_handle_set_blur_region(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *region_resource) {
	struct wlr_ext_background_effect_surface_v1 *surface = surface_from_resource(resource);

	if (surface == NULL) {
		wl_resource_post_error(resource,
			EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
			"The wl_surface object has been destroyed");
		return;
	}

	if (region_resource != NULL) {
		const pixman_region32_t *region = wlr_region_from_resource(region_resource);
		pixman_region32_copy(&surface->pending.blur_region, region);
	} else {
		pixman_region32_clear(&surface->pending.blur_region);
	}
}

static const struct ext_background_effect_surface_v1_interface surface_impl = {
	.destroy = surface_handle_destroy,
	.set_blur_region = surface_handle_set_blur_region,
};

static void surface_synced_init_state(void *_state) {
	struct wlr_ext_background_effect_surface_v1_state *state = _state;
	pixman_region32_init(&state->blur_region);
}

static void surface_synced_finish_state(void *_state) {
	struct wlr_ext_background_effect_surface_v1_state *state = _state;
	pixman_region32_fini(&state->blur_region);
}

static void surface_synced_move_state(void *_dst, void *_src) {
	struct wlr_ext_background_effect_surface_v1_state *dst = _dst;
	struct wlr_ext_background_effect_surface_v1_state *src = _src;

	pixman_region32_copy(&dst->blur_region, &src->blur_region);
}

static const struct wlr_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(struct wlr_ext_background_effect_surface_v1_state),
	.init_state = surface_synced_init_state,
	.finish_state = surface_synced_finish_state,
	.move_state = surface_synced_move_state,
};

static void surface_addon_destroy(struct wlr_addon *addon) {
	struct wlr_ext_background_effect_surface_v1 *surface = wl_container_of(addon, surface, addon);

	surface_destroy(surface);
}

static const struct wlr_addon_interface surface_addon_impl = {
	.name = "ext_background_effect_surface_v1",
	.destroy = surface_addon_destroy,
};

static struct wlr_ext_background_effect_surface_v1 *surface_from_wlr_surface(
		struct wlr_surface *wlr_surface) {
	struct wlr_addon *addon = wlr_addon_find(&wlr_surface->addons, NULL, &surface_addon_impl);
	if (addon == NULL) {
		return NULL;
	}

	struct wlr_ext_background_effect_surface_v1 *surface =
		wl_container_of(addon, surface, addon);

	return surface;
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void manager_handle_get_background_effect(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id, struct wl_resource *surface_resource) {
	struct wlr_surface *wlr_surface = wlr_surface_from_resource(surface_resource);

	if (surface_from_wlr_surface(wlr_surface) != NULL) {
		wl_resource_post_error(manager_resource,
			EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
			"The wl_surface object already has a ext_background_effect_surface_v1 object");
		return;
	}

	struct wlr_ext_background_effect_surface_v1 *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	if (!wlr_surface_synced_init(&surface->synced, wlr_surface, &surface_synced_impl,
			&surface->pending, &surface->current)) {
		free(surface);
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	uint32_t version = wl_resource_get_version(manager_resource);
	surface->resource =
		wl_resource_create(client, &ext_background_effect_surface_v1_interface, version, id);

	if (surface->resource == NULL) {
		wlr_surface_synced_finish(&surface->synced);
		free(surface);
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	wl_resource_set_implementation(surface->resource, &surface_impl, surface,
		surface_handle_resource_destroy);

	wl_signal_init(&surface->events.commit);
	wl_signal_init(&surface->events.destroy);

	surface->surface = wlr_surface;
	wlr_addon_init(&surface->addon, &wlr_surface->addons, NULL, &surface_addon_impl);

	surface->surface_commit.notify = surface_handle_commit;
	wl_signal_add(&wlr_surface->events.commit, &surface->surface_commit);

	struct wlr_ext_background_effect_manager_client *manager_client =
		manager_client_from_resource(manager_resource);
	wl_signal_emit_mutable(&manager_client->manager->events.new_surface, surface);
}

static const struct ext_background_effect_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_background_effect = manager_handle_get_background_effect,
};

static void manager_client_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_ext_background_effect_manager_client *client = manager_client_from_resource(resource);

	wl_list_remove(&client->link);
	free(client);
}

static void manager_bind(struct wl_client *wl_client, void *data, uint32_t version, uint32_t id) {
	struct wlr_ext_background_effect_manager_v1 *manager = data;

	struct wlr_ext_background_effect_manager_client *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	client->resource = wl_resource_create(wl_client, &ext_background_effect_manager_v1_interface,
		version, id);
	if (client->resource == NULL) {
		free(client);
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(client->resource, &manager_impl, client,
		manager_client_handle_resource_destroy);

	client->manager = manager;
	wl_list_insert(&manager->resources, &client->link);

	ext_background_effect_manager_v1_send_capabilities(client->resource, manager->capabilities);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_ext_background_effect_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);

	wl_signal_emit_mutable(&manager->events.destroy, NULL);
	assert(wl_list_empty(&manager->events.destroy.listener_list));

	wl_global_destroy(manager->global);
	wl_list_remove(&manager->display_destroy.link);
	free(manager);
}

struct wlr_ext_background_effect_manager_v1 *wlr_ext_background_effect_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= BACKGROUND_EFFECT_VERSION);

	struct wlr_ext_background_effect_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display, &ext_background_effect_manager_v1_interface, version,
		manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	wl_signal_init(&manager->events.new_surface);
	wl_signal_init(&manager->events.destroy);
	wl_list_init(&manager->resources);

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}

void wlr_ext_background_effect_manager_v1_set_capabilities(
		struct wlr_ext_background_effect_manager_v1 *manager, uint32_t capabilities) {
	manager->capabilities = capabilities;

	struct wlr_ext_background_effect_manager_client *client;
	wl_list_for_each(client, &manager->resources, link) {
		ext_background_effect_manager_v1_send_capabilities(client->resource,
			capabilities);
	}
}

const struct wlr_ext_background_effect_surface_v1_state *
wlr_ext_background_effect_v1_get_surface_state(struct wlr_surface *wlr_surface) {
	struct wlr_ext_background_effect_surface_v1 *surface =
		surface_from_wlr_surface(wlr_surface);

	if (surface == NULL) {
		return NULL;
	}

	return &surface->current;
}
