#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_xdg_toplevel_drag_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_data_device.h>

#include "types/wlr_data_device.h"
#include "xdg-toplevel-drag-v1-protocol.h"

#define TOPLEVEL_DRAG_MANAGER_V1_VERSION 1

static const struct xdg_toplevel_drag_v1_interface drag_impl;

static struct wlr_xdg_toplevel_drag_v1 *drag_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_toplevel_drag_v1_interface,
		&drag_impl));
	return wl_resource_get_user_data(resource);
}

static const struct xdg_toplevel_drag_manager_v1_interface manager_impl;

static struct wlr_xdg_toplevel_drag_manager_v1 *manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&xdg_toplevel_drag_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

static void resource_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void drag_detach_toplevel(struct wlr_xdg_toplevel_drag_v1 *drag) {
	if (drag->toplevel == NULL) {
		return;
	}
	wl_list_remove(&drag->toplevel_unmap.link);
	wl_list_init(&drag->toplevel_unmap.link);
	wl_list_remove(&drag->toplevel_surface_destroy.link);
	wl_list_init(&drag->toplevel_surface_destroy.link);
	drag->toplevel = NULL;
}

static void handle_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_drag_v1 *drag =
		wl_container_of(listener, drag, toplevel_unmap);
	drag_detach_toplevel(drag);
}

static void handle_toplevel_surface_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_drag_v1 *drag =
		wl_container_of(listener, drag, toplevel_surface_destroy);
	drag_detach_toplevel(drag);
}

static void drag_handle_attach(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *toplevel_resource,
		int32_t x_offset, int32_t y_offset) {
	struct wlr_xdg_toplevel_drag_v1 *drag = drag_from_resource(resource);
	if (drag == NULL) {
		return;
	}

	struct wlr_xdg_toplevel *toplevel =
		wlr_xdg_toplevel_from_resource(toplevel_resource);

	// Check if a valid toplevel is already attached
	if (drag->toplevel != NULL && drag->toplevel->base->surface->mapped) {
		wl_resource_post_error(resource,
			XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED,
			"a mapped toplevel is already attached");
		return;
	}

	drag_detach_toplevel(drag);

	drag->toplevel = toplevel;
	drag->x_offset = x_offset;
	drag->y_offset = y_offset;

	drag->toplevel_unmap.notify = handle_toplevel_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &drag->toplevel_unmap);

	drag->toplevel_surface_destroy.notify = handle_toplevel_surface_destroy;
	wl_signal_add(&toplevel->base->surface->events.destroy, &drag->toplevel_surface_destroy);
}

static void drag_destroy(struct wlr_xdg_toplevel_drag_v1 *drag) {
	if (drag == NULL) {
		return;
	}

	wl_signal_emit_mutable(&drag->events.destroy, NULL);

	assert(wl_list_empty(&drag->events.destroy.listener_list));

	drag_detach_toplevel(drag);

	wl_list_remove(&drag->link);
	wl_list_remove(&drag->data_source_destroy.link);

	wl_resource_set_user_data(drag->resource, NULL);
	free(drag);
}

static void drag_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	struct wlr_xdg_toplevel_drag_v1 *drag = drag_from_resource(resource);
	if (drag == NULL) {
		wl_resource_destroy(resource);
		return;
	}

	// Per protocol: destroy must only be called after dnd_drop_performed or
	// cancelled events. Check if drag started but hasn't ended yet.
	if (drag->started && !drag->ended) {
		wl_resource_post_error(resource,
			XDG_TOPLEVEL_DRAG_V1_ERROR_ONGOING_DRAG,
			"xdg_toplevel_drag destroyed while drag is ongoing");
		return;
	}

	wl_resource_destroy(resource);
}

static void handle_drag_resource_destroy(struct wl_resource *resource) {
	struct wlr_xdg_toplevel_drag_v1 *drag = drag_from_resource(resource);
	drag_destroy(drag);
}

static const struct xdg_toplevel_drag_v1_interface drag_impl = {
	.destroy = drag_handle_destroy,
	.attach = drag_handle_attach,
};

static void handle_data_source_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_xdg_toplevel_drag_v1 *drag =
		wl_container_of(listener, drag, data_source_destroy);
	drag->data_source = NULL;
	drag_destroy(drag);
}

static void manager_handle_get_xdg_toplevel_drag(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *source_resource) {
	struct wlr_xdg_toplevel_drag_manager_v1 *manager =
		manager_from_resource(manager_resource);

	struct wlr_client_data_source *client_source =
		client_data_source_from_resource(source_resource);
	struct wlr_data_source *source = &client_source->source;

	// Check if source already has a toplevel drag
	struct wlr_xdg_toplevel_drag_v1 *existing;
	wl_list_for_each(existing, &manager->drags, link) {
		if (existing->data_source == source) {
			wl_resource_post_error(manager_resource,
				XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE,
				"data_source already has an xdg_toplevel_drag_v1");
			return;
		}
	}

	struct wlr_xdg_toplevel_drag_v1 *drag = calloc(1, sizeof(*drag));
	if (drag == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	drag->resource = wl_resource_create(client, &xdg_toplevel_drag_v1_interface,
		wl_resource_get_version(manager_resource), id);
	if (drag->resource == NULL) {
		free(drag);
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(drag->resource, &drag_impl, drag,
		handle_drag_resource_destroy);

	drag->manager = manager;
	drag->data_source = source;

	wl_list_insert(&manager->drags, &drag->link);
	wl_list_init(&drag->toplevel_unmap.link);
	wl_list_init(&drag->toplevel_surface_destroy.link);

	drag->data_source_destroy.notify = handle_data_source_destroy;
	wl_signal_add(&source->events.destroy, &drag->data_source_destroy);

	wl_signal_init(&drag->events.destroy);

	wl_signal_emit_mutable(&manager->events.new_toplevel_drag, drag);
}

static const struct xdg_toplevel_drag_manager_v1_interface manager_impl = {
	.destroy = resource_handle_destroy,
	.get_xdg_toplevel_drag = manager_handle_get_xdg_toplevel_drag,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_xdg_toplevel_drag_manager_v1 *manager = data;
	struct wl_resource *resource = wl_resource_create(client,
		&xdg_toplevel_drag_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_drag_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);

	wl_signal_emit_mutable(&manager->events.destroy, NULL);

	assert(wl_list_empty(&manager->events.destroy.listener_list));
	assert(wl_list_empty(&manager->events.new_toplevel_drag.listener_list));

	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_xdg_toplevel_drag_v1 *wlr_xdg_toplevel_drag_v1_from_wlr_data_source(
		struct wlr_xdg_toplevel_drag_manager_v1 *manager,
		struct wlr_data_source *source) {
	if (manager == NULL || source == NULL) {
		return NULL;
	}

	struct wlr_xdg_toplevel_drag_v1 *drag;
	wl_list_for_each(drag, &manager->drags, link) {
		if (drag->data_source == source) {
			return drag;
		}
	}
	return NULL;
}

struct wlr_xdg_toplevel_drag_v1 *wlr_xdg_toplevel_drag_v1_from_wlr_xdg_toplevel(
		struct wlr_xdg_toplevel_drag_manager_v1 *manager,
		struct wlr_xdg_toplevel *toplevel) {
	if (manager == NULL || toplevel == NULL) {
		return NULL;
	}

	struct wlr_xdg_toplevel_drag_v1 *drag;
	wl_list_for_each(drag, &manager->drags, link) {
		if (drag->toplevel == toplevel) {
			return drag;
		}
	}
	return NULL;
}

struct wlr_xdg_toplevel_drag_manager_v1 *wlr_xdg_toplevel_drag_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= TOPLEVEL_DRAG_MANAGER_V1_VERSION);

	struct wlr_xdg_toplevel_drag_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display,
		&xdg_toplevel_drag_manager_v1_interface, version, manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	wl_list_init(&manager->drags);

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	wl_signal_init(&manager->events.destroy);
	wl_signal_init(&manager->events.new_toplevel_drag);

	return manager;
}

void wlr_xdg_toplevel_drag_v1_start(struct wlr_xdg_toplevel_drag_v1 *drag) {
	drag->started = true;
}

void wlr_xdg_toplevel_drag_v1_finish(struct wlr_xdg_toplevel_drag_v1 *drag) {
	drag->ended = true;
}
