#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>
#include "color-representation-v1-protocol.h"

#define WP_COLOR_REPRESENTATION_VERSION 1

static enum wlr_color_encoding color_encoding_to_wlr(int wp_val) {
	switch(wp_val) {
		case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601:
			return WLR_COLOR_ENCODING_BT601;
		case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709:
			return WLR_COLOR_ENCODING_BT709;
		case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020:
			return WLR_COLOR_ENCODING_BT2020;
		default:
			return WLR_COLOR_ENCODING_NONE;
	}
}

static enum wlr_color_range color_range_to_wlr(int wp_val) {
	switch(wp_val) {
		case WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED:
			return  WLR_COLOR_RANGE_LIMITED;
		case WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL:
			return WLR_COLOR_RANGE_FULL;
		default:
			return WLR_COLOR_RANGE_NONE;
	}
}

static enum wlr_color_chroma_location chroma_location_to_wlr(int wp_val) {
	switch(wp_val) {
		case WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_0:
			return WLR_COLOR_CHROMA_LOCATION_TYPE0;
		case WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_1:
			return WLR_COLOR_CHROMA_LOCATION_TYPE1;
		case WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_2:
			return WLR_COLOR_CHROMA_LOCATION_TYPE2;
		case WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_3:
			return WLR_COLOR_CHROMA_LOCATION_TYPE3;
		default:
			return WLR_COLOR_CHROMA_LOCATION_NONE;
	}
}

struct wlr_color_representation_v1 {
	struct wl_resource *resource;
	struct wlr_surface *surface;

	// Associate the wlr_color_representation_v1 with a wlr_surface
	struct wlr_addon addon;

	struct wlr_surface_synced synced;
	struct wlr_color_representation_v1_state pending, current;
};

static const struct wp_color_representation_surface_v1_interface color_repr_impl;

static struct wlr_color_representation_v1 *color_repr_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_color_representation_surface_v1_interface,
		&color_repr_impl));
	return wl_resource_get_user_data(resource);
}

static void color_repr_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	// Actual destroying is done by the resource-destroy handler
	wl_resource_destroy(resource);
}

static void color_repr_handle_set_alpha_mode(struct wl_client *client,
		struct wl_resource *resource, uint32_t alpha_mode) {
	struct wlr_color_representation_v1 *color_repr =
		color_repr_from_resource(resource);
	if (color_repr == NULL) {
		wl_resource_post_error(resource, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT,
			"Associated surface has been destroyed, object is inert");
		return;
	}

	if (alpha_mode != WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_ALPHA_MODE,
			"Unsupported alpha mode");
		return;
	}

	// Don't currently handle alpha modes.
}

static void color_repr_handle_set_coefficients_and_range(struct wl_client *client,
		struct wl_resource *resource, uint32_t coefficients,
		uint32_t range) {
	struct wlr_color_representation_v1 *color_repr =
		color_repr_from_resource(resource);
	if (color_repr == NULL) {
		wl_resource_post_error(resource, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT,
			"Associated surface has been destroyed, object is inert");
		return;
	}

	enum wlr_color_encoding wlr_encoding =
		color_encoding_to_wlr(coefficients);
	if (wlr_encoding == WLR_COLOR_ENCODING_NONE) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_COEFFICIENTS,
			"Unsupported coefficients");
		return;
	}

	enum wlr_color_range wlr_range =
		color_range_to_wlr(range);
	if (wlr_range == WLR_COLOR_RANGE_NONE) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_COEFFICIENTS,
			"Unsupported range");
		return;
	}

	// Ignore the message unless both coefficients and range are valid. The
	// protocol isn't totally clear what should happen if only one is valid.

	color_repr->pending.coefficients = wlr_encoding;
	color_repr->pending.range = wlr_range;
}

static void color_repr_handle_set_chroma_location(struct wl_client *client,
		struct wl_resource *resource, uint32_t chroma_location) {
	struct wlr_color_representation_v1 *color_repr =
		color_repr_from_resource(resource);
	if (color_repr == NULL) {
		wl_resource_post_error(resource, WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT,
			"Associated surface has been destroyed, object is inert");
		return;
	}

	enum wlr_color_chroma_location wlr_loc =
		chroma_location_to_wlr(chroma_location);

	if (wlr_loc == WLR_COLOR_CHROMA_LOCATION_NONE) {
		// In this protocol there's no concept of supported chroma locations
		// from a client point-of-view. The compositor should just ignore any
		// chroma locations it doesn't know what to do with. Indicated by
		// WLR_COLOR_CHROMA_LOCATION_NONE.
		wlr_log(WLR_DEBUG, "Warning: Unrecognised/unsupported chroma location");
	}

	color_repr->pending.chroma_location = wlr_loc;
}

static const struct wp_color_representation_surface_v1_interface color_repr_impl = {
	.destroy = color_repr_handle_destroy,
	.set_alpha_mode = color_repr_handle_set_alpha_mode,
	.set_coefficients_and_range = color_repr_handle_set_coefficients_and_range,
	.set_chroma_location = color_repr_handle_set_chroma_location,
};

static void color_repr_destroy(struct wlr_color_representation_v1 *color_repr) {
	if (color_repr == NULL) {
		return;
	}
	wlr_surface_synced_finish(&color_repr->synced);
	wlr_addon_finish(&color_repr->addon);
	wl_resource_set_user_data(color_repr->resource, NULL);
	free(color_repr);
}

static void color_repr_addon_destroy(struct wlr_addon *addon) {
	struct wlr_color_representation_v1 *color_repr =
		wl_container_of(addon, color_repr, addon);
	color_repr_destroy(color_repr);
}

static const struct wlr_addon_interface surface_addon_impl = {
	.name = "wlr_color_representation_v1",
	.destroy = color_repr_addon_destroy,
};

static void color_repr_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_color_representation_v1 *color_repr =
		color_repr_from_resource(resource);
	color_repr_destroy(color_repr);
}

static void color_repr_manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	// Actual destroying is done by the resource-destroy handler
	wl_resource_destroy(resource);
}

static void surface_synced_init_state(void *_state) {
	struct wlr_color_representation_v1_state *state = _state;
	state->coefficients = WLR_COLOR_ENCODING_NONE;
	state->range = WLR_COLOR_RANGE_NONE;
	state->chroma_location = WLR_COLOR_CHROMA_LOCATION_NONE;
}

static const struct wlr_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(struct wlr_color_representation_v1_state),
	.init_state = surface_synced_init_state,
};

static struct wlr_color_representation_v1 *color_repr_from_surface(
		struct wlr_surface *surface) {
	struct wlr_addon *addon = wlr_addon_find(&surface->addons, NULL, &surface_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct wlr_color_representation_v1 *color_repr = wl_container_of(addon, color_repr, addon);
	return color_repr;
}

static void color_repr_manager_handle_get_surface(struct wl_client *client,
		struct wl_resource *manager_resource,
		uint32_t color_repr_id,
		struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

	// Check if there's already a color-representation attached to
	// this surface
	if (color_repr_from_surface(surface) != NULL) {
		wl_resource_post_error(manager_resource,
			WP_COLOR_REPRESENTATION_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"wp_color_representation_surface_v1 already exists for this surface");
		return;
	}

	struct wlr_color_representation_v1 *color_repr = calloc(1, sizeof(*color_repr));
	if (!color_repr) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	if (!wlr_surface_synced_init(&color_repr->synced, surface,
			&surface_synced_impl, &color_repr->pending, &color_repr->current)) {
		free(color_repr);
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	uint32_t version = wl_resource_get_version(manager_resource);
	color_repr->resource = wl_resource_create(client,
		&wp_color_representation_surface_v1_interface, version, color_repr_id);
	if (color_repr->resource == NULL) {
		wlr_surface_synced_finish(&color_repr->synced);
		free(color_repr);
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(color_repr->resource,
		&color_repr_impl, color_repr, color_repr_handle_resource_destroy);

	wlr_addon_init(&color_repr->addon, &surface->addons, NULL, &surface_addon_impl);
}

static const struct wp_color_representation_manager_v1_interface color_repr_manager_impl = {
	.destroy = color_repr_manager_handle_destroy,
	.get_surface = color_repr_manager_handle_get_surface,
};

static void send_supported(struct wlr_color_representation_manager_v1 *manager,
		struct wl_resource *resource) {
	// Not all renders or DRM scanout devices will support all of these, and
	// we can't know in advance if a surface will go through the renderer or
	// direct scanout.  EGL doesn't even tell us if the hints will have any
	// effect or are totally ignored.  So, just advertise a standard set of
	// capabilities and accept that clients' requests might get silently
	// ignored.  This is roughly the intersection of what's supported by
	// DRM atomic and EGL hints.

	// DRM pixel_blend_mode exposes pre-multiplied (assuming electrical) and
	// "coverage" (i.e. straight).
	// Our renderers only support pre-multiplied (not straight) alpha, so
	// for now only support this.
	wp_color_representation_manager_v1_send_supported_alpha_mode(resource,
		WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL);

	wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
		resource, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601,
		WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
	wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
		resource, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601,
		WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED);
	wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
		resource, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709,
		WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
	wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
		resource, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709,
		WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED);
	wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
		resource, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020,
		WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
	wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
		resource, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020,
		WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED);

	wp_color_representation_manager_v1_send_done(resource);
}

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_color_representation_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&wp_color_representation_manager_v1_interface,
		version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &color_repr_manager_impl, manager, NULL);

	send_supported(manager, resource);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_color_representation_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);

	wl_signal_emit_mutable(&manager->events.destroy, manager);

	assert(wl_list_empty(&manager->events.destroy.listener_list));

	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_color_representation_manager_v1 *wlr_color_representation_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= WP_COLOR_REPRESENTATION_VERSION);

	struct wlr_color_representation_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display,
		&wp_color_representation_manager_v1_interface,
		version, manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}

const struct wlr_color_representation_v1_state *wlr_color_representation_v1_get_surface_state(
		struct wlr_surface *surface) {
	struct wlr_color_representation_v1 *color_repr = color_repr_from_surface(surface);
	if (color_repr == NULL) {
		return NULL;
	}
	return &color_repr->current;
}
