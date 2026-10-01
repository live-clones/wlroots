#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wlr/types/wlr_xdg_cutouts_v1.h>
#include <wlr/util/log.h>
#include "xdg-cutouts-v1-protocol.h"

#define CUTOUTS_MANAGER_VERSION 1

static const struct xdg_cutouts_v1_interface cutouts_impl;

static struct wlr_xdg_cutouts_v1 *cutouts_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_cutouts_v1_interface, &cutouts_impl));
	return wl_resource_get_user_data(resource);
}

static void cutouts_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void cutouts_handle_set_unhandled(struct wl_client *client,
		struct wl_resource *resource,
		struct wl_array *unhandled) {
	struct wlr_xdg_cutouts_v1 *cutouts =
		cutouts_from_resource(resource);
	wl_array_release(&cutouts->pending.unhandled);
	wl_array_init(&cutouts->pending.unhandled);
	wl_array_copy(&cutouts->pending.unhandled, unhandled);
}

static const struct xdg_cutouts_v1_interface cutouts_impl = {
	.destroy = cutouts_handle_destroy,
	.set_unhandled = cutouts_handle_set_unhandled,
};


static void
add_sent_id(struct wlr_xdg_cutouts_v1 *cutouts, uint32_t id)
{
	uint32_t *new_id = wl_array_add (&cutouts->sent_ids, sizeof(uint32_t));
	*new_id = id;
}


static void
array_move(struct wl_array *dst, struct wl_array *src)
{
	assert(dst->size == 0);
	wl_array_copy(dst, src);
	wl_array_release (src);
	wl_array_init(src);
}

static bool
array_equal(struct wl_array *a1, struct wl_array *a2)
{
	if (a1->size != a2->size)
		return false;

	return memcmp(a1, a2, a1->size) == 0;
}


static bool
array_contains(struct wl_array *haystack, uint32_t needle)
{
	uint32_t *id;
	wl_array_for_each(id, haystack) {
		if (needle == *id)
			return true;
	}
	return false;
}

void wlr_xdg_cutouts_v1_send_cutout(struct wlr_xdg_cutouts_v1 *cutouts, const struct wlr_box *box,
	       enum wlr_cutouts_type type, uint32_t id) {
	enum xdg_cutouts_v1_type cutouts_v1_type;

	assert(!wl_list_empty(&cutouts->configure_list));

	switch (type) {
	case WLR_CUTOUTS_TYPE_CUTOUT:
		cutouts_v1_type = XDG_CUTOUTS_V1_TYPE_CUTOUT;
		break;
	case WLR_CUTOUTS_TYPE_NOTCH:
		cutouts_v1_type = XDG_CUTOUTS_V1_TYPE_NOTCH;
		break;
	case WLR_CUTOUTS_TYPE_WATERFALL:
		cutouts_v1_type = XDG_CUTOUTS_V1_TYPE_WATERFALL;
		break;
	default:
		assert(false);
	}

	xdg_cutouts_v1_send_cutout_box (cutouts->resource, box->x, box->y, box->width, box->height,
	       cutouts_v1_type, id);

	add_sent_id (cutouts, id);
}

void wlr_xdg_cutouts_v1_send_corner(struct wlr_xdg_cutouts_v1 *cutouts,
	        enum wlr_edges position, uint32_t radius, uint32_t id) {
	enum xdg_cutouts_v1_corner_position corner;

	assert(!wl_list_empty(&cutouts->configure_list));

	switch ((uint32_t)position) {
	case WLR_EDGE_LEFT | WLR_EDGE_TOP:
		corner = XDG_CUTOUTS_V1_CORNER_POSITION_TOP_LEFT;
		break;
	case WLR_EDGE_RIGHT | WLR_EDGE_TOP:
		corner = XDG_CUTOUTS_V1_CORNER_POSITION_TOP_RIGHT;
		break;
	case WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM:
		corner = XDG_CUTOUTS_V1_CORNER_POSITION_BOTTOM_RIGHT;
		break;
	case WLR_EDGE_LEFT | WLR_EDGE_BOTTOM:
		corner = XDG_CUTOUTS_V1_CORNER_POSITION_BOTTOM_LEFT;
		break;
	default:
		assert(false);
	}

        xdg_cutouts_v1_send_cutout_corner (cutouts->resource, corner, radius, id);

	add_sent_id (cutouts, id);
}

void wlr_xdg_cutouts_v1_send_cutouts_done(struct wlr_xdg_cutouts_v1 *cutouts) {
	// The xdg_surface.configure from the current configure sequence needs to be present and
	// the first element in the list
	assert(!wl_list_empty(&cutouts->configure_list));
	struct wlr_xdg_cutouts_v1_configure *configure =
		wl_container_of((&cutouts->configure_list)->next, configure, link);
	array_move(&configure->valid_ids, &cutouts->sent_ids);
	xdg_cutouts_v1_send_configure (cutouts->resource);
}

static void cutouts_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_xdg_cutouts_v1 *cutouts = cutouts_from_resource(resource);
	wl_signal_emit_mutable(&cutouts->events.destroy, NULL);

	assert(wl_list_empty(&cutouts->events.destroy.listener_list));
	assert(wl_list_empty(&cutouts->events.unhandled_updated.listener_list));
        assert(wl_list_empty(&cutouts->events.send_cutouts.listener_list));

	wl_list_remove(&cutouts->toplevel_destroy.link);
	wl_list_remove(&cutouts->surface_configure.link);
	wl_list_remove(&cutouts->surface_ack_configure.link);
	struct wlr_xdg_cutouts_v1_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &cutouts->configure_list, link) {
		free(configure);
	}
	wl_list_remove(&cutouts->link);

	wl_array_release(&cutouts->sent_ids);
	wl_array_release(&cutouts->pending.unhandled);
	wl_array_release(&cutouts->current.unhandled);

	free(cutouts);
}

static void cutouts_handle_toplevel_destroy(
		struct wl_listener *listener, void *data) {
	struct wlr_xdg_cutouts_v1 *cutouts =
		wl_container_of(listener, cutouts, toplevel_destroy);

        wl_resource_post_error(cutouts->resource,
                XDG_CUTOUTS_MANAGER_V1_ERROR_DEFUNCT_CUTOUTS_OBJECT,
                "xdg_toplevel destroyed before xdg_cutouts");

	wl_resource_destroy(cutouts->resource);
}

static void cutouts_handle_surface_configure(
		struct wl_listener *listener, void *data) {
	struct wlr_xdg_cutouts_v1 *cutouts =
		wl_container_of(listener, cutouts, surface_configure);
	struct wlr_surface_configure *surface_configure = data;

	struct wlr_xdg_cutouts_v1_configure *configure = calloc(1, sizeof(*configure));
	if (configure == NULL) {
		return;
	}
	wl_array_init (&configure->valid_ids);
	configure->surface_configure = surface_configure;
	wl_list_insert(cutouts->configure_list.prev, &configure->link);
	wl_signal_emit_mutable(&cutouts->events.send_cutouts, NULL);
}


static void
cutouts_configure_destroy(struct wlr_xdg_cutouts_v1_configure *configure) {
	wl_list_remove(&configure->link);
	wl_array_release (&configure->valid_ids);
	free(configure);
}


static void cutouts_handle_surface_ack_configure(
		struct wl_listener *listener, void *data) {
	struct wlr_xdg_cutouts_v1 *cutouts =
		wl_container_of(listener, cutouts, surface_ack_configure);
	struct wlr_surface_configure *surface_configure = data;

	// First find the ack'ed configure
	bool found = false;
	struct wlr_xdg_cutouts_v1_configure *configure, *tmp;
	wl_list_for_each(configure, &cutouts->configure_list, link) {
		if (configure->surface_configure == surface_configure) {
			found = true;
			break;
		}
	}
	if (!found) {
		return;
	}

	bool needs_update = !array_equal(&cutouts->current.unhandled, &cutouts->pending.unhandled);
	if (needs_update) {
		uint32_t *id;
		wl_array_for_each (id, &cutouts->pending.unhandled) {
			if (!array_contains(&configure->valid_ids, *id)) {
				wl_resource_post_error(cutouts->resource,
					XDG_CUTOUTS_V1_ERROR_INVALID_ELEMENT_ID,
					"Invalid element id %d", *id);
			}
		}
	}

	// Then remove old configures from the list
	wl_list_for_each_safe(configure, tmp, &cutouts->configure_list, link) {
		if (configure->surface_configure == surface_configure) {
			break;
		}
		cutouts_configure_destroy (configure);
	}

	if (needs_update) {
		wl_array_release(&cutouts->current.unhandled);
		wl_array_init(&cutouts->current.unhandled);
		array_move(&cutouts->current.unhandled, &cutouts->pending.unhandled);
		wl_signal_emit_mutable(&cutouts->events.unhandled_updated, NULL);
	}

	cutouts_configure_destroy (configure);
}

static const struct xdg_cutouts_manager_v1_interface cutouts_manager_impl;

static struct wlr_xdg_cutouts_manager_v1 *
		cutouts_manager_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_cutouts_manager_v1_interface,
		&cutouts_manager_impl));
	return wl_resource_get_user_data(resource);
}

static void cutouts_manager_handle_destroy(
		struct wl_client *client, struct wl_resource *manager_resource) {
	wl_resource_destroy(manager_resource);
}


static void cutouts_manager_handle_get_xdg_cutouts(
		struct wl_client *client, struct wl_resource *manager_resource,
		uint32_t id, struct wl_resource *surface_resource) {
	struct wlr_xdg_cutouts_manager_v1 *manager = cutouts_manager_from_resource(manager_resource);
	struct wlr_surface *surface = wlr_surface_from_resource (surface_resource);
	struct wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_try_from_wlr_surface (surface);
	if (!toplevel) {
		wl_resource_post_error(manager_resource,
			XDG_CUTOUTS_MANAGER_V1_ERROR_INVALID_ROLE,
			"xdg_cutouts_v1 must be a xdg_toplevel");
		return;
	}

	struct wlr_xdg_cutouts_v1 *cutouts = calloc(1, sizeof(*cutouts));
	if (cutouts == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	cutouts->manager = manager;
	cutouts->toplevel = toplevel;

	uint32_t version = wl_resource_get_version(manager_resource);
	cutouts->resource = wl_resource_create(client, &xdg_cutouts_v1_interface, version, id);
	if (cutouts->resource == NULL) {
		free(cutouts);
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(cutouts->resource, &cutouts_impl, cutouts,
		cutouts_handle_resource_destroy);

	wlr_log(WLR_DEBUG, "new xdg_cutouts %p (res %p)", cutouts,
		cutouts->resource);

	wl_list_init(&cutouts->configure_list);
	wl_array_init(&cutouts->pending.unhandled);
	wl_array_init(&cutouts->current.unhandled);
	wl_array_init(&cutouts->sent_ids);

        wl_signal_init(&cutouts->events.send_cutouts);
	wl_signal_init(&cutouts->events.unhandled_updated);
	wl_signal_init(&cutouts->events.destroy);

	wl_signal_add(&toplevel->events.destroy, &cutouts->toplevel_destroy);
	cutouts->toplevel_destroy.notify = cutouts_handle_toplevel_destroy;

	wl_signal_add(&toplevel->base->events.configure, &cutouts->surface_configure);
	cutouts->surface_configure.notify = cutouts_handle_surface_configure;

	wl_signal_add(&toplevel->base->events.ack_configure, &cutouts->surface_ack_configure);
	cutouts->surface_ack_configure.notify = cutouts_handle_surface_ack_configure;

	wl_list_insert(&manager->cutouts, &cutouts->link);
	wl_signal_emit_mutable(&manager->events.new_cutouts, cutouts);

	// Schedule a configure to emit cutouts information
	wlr_xdg_surface_schedule_configure(cutouts->toplevel->base);
}

static const struct xdg_cutouts_manager_v1_interface cutouts_manager_impl = {
	.destroy = cutouts_manager_handle_destroy,
	.get_cutouts = cutouts_manager_handle_get_xdg_cutouts,
};

static void cutouts_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_xdg_cutouts_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&xdg_cutouts_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &cutouts_manager_impl,
		manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_cutouts_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_signal_emit_mutable(&manager->events.destroy, NULL);

	assert(wl_list_empty(&manager->events.new_cutouts.listener_list));
	assert(wl_list_empty(&manager->events.destroy.listener_list));

	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_xdg_cutouts_manager_v1 *
		wlr_xdg_cutouts_manager_v1_create(struct wl_display *display) {
	struct wlr_xdg_cutouts_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}
	manager->global = wl_global_create(display, &xdg_cutouts_manager_v1_interface,
		CUTOUTS_MANAGER_VERSION, manager, cutouts_manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}
	wl_list_init(&manager->cutouts);

	manager->next_id = 1;
	wl_signal_init(&manager->events.new_cutouts);
	wl_signal_init(&manager->events.destroy);

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
