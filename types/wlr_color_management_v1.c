#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/addon.h>

#define COLOR_MANAGEMENT_V1_VERSION 1

struct wlr_color_management_output_v1 {
	struct wl_resource *resource;
	struct wlr_output *output;
	struct wl_list link;

	struct wl_listener output_destroy;
};

struct wlr_color_management_surface_v1 {
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wlr_color_manager_v1 *manager;

	struct wlr_addon addon;
};

struct wlr_color_management_feedback_surface_v1 {
	struct wl_resource *resource;
	struct wlr_surface *surface;

	struct wl_listener surface_destroy;
};

struct wlr_color_manager_v1_primaries {
	struct {
		float x, y;
	} red, green, blue, white;
};

static void resource_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void primaries_from_named(struct wlr_color_manager_v1_primaries *out,
		enum wp_color_manager_v1_primaries primaries) {
	// See H.273 ColourPrimaries
	switch (primaries) {
	case WP_COLOR_MANAGER_V1_PRIMARIES_SRGB: // code point 1
		*out = (struct wlr_color_manager_v1_primaries){
			.red = { 0.640, 0.330 },
			.green = { 0.300, 0.600 },
			.blue = { 0.150, 0.060 },
			.white = { 0.3127, 0.3290 },
		};
		break;
	default:
		abort(); // TODO: more primaries
	}
}

static void default_tf_luminance(enum wp_color_manager_v1_transfer_function tf,
		float *min, float *max, float *reference) {
	switch (tf) {
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
		*min = 0;
		*max = 10000;
		*reference = 203;
		break;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG:
		*min = 0.005;
		*max = 1000;
		*reference = 203;
		break;
	default:
		*min = 0.02;
		*max = 80;
		*reference = 80;
		break;
	}
}

static int32_t encode_cie1931_coord(float value) {
	return round(value * 1000 * 1000);
}

static void image_desc_handle_get_information(struct wl_client *client,
		struct wl_resource *image_desc_resource, uint32_t id) {
	uint32_t version = wl_resource_get_version(image_desc_resource);
	struct wl_resource *resource = wl_resource_create(client,
		&wp_image_description_info_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	enum wp_color_manager_v1_primaries primaries_named =
		WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
	enum wp_color_manager_v1_transfer_function transfer_function =
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB;

	struct wlr_color_manager_v1_primaries primaries;
	primaries_from_named(&primaries, primaries_named);

	float min_luminance, max_luminance, reference_luminance;
	default_tf_luminance(transfer_function, &min_luminance, &max_luminance, &reference_luminance);

	wp_image_description_info_v1_send_primaries_named(resource, primaries_named);
	wp_image_description_info_v1_send_primaries(resource,
		encode_cie1931_coord(primaries.red.x), encode_cie1931_coord(primaries.red.y),
		encode_cie1931_coord(primaries.green.x), encode_cie1931_coord(primaries.green.y),
		encode_cie1931_coord(primaries.blue.x), encode_cie1931_coord(primaries.blue.y),
		encode_cie1931_coord(primaries.white.x), encode_cie1931_coord(primaries.white.y));
	wp_image_description_info_v1_send_tf_named(resource, transfer_function);
    wp_image_description_info_v1_send_luminances(resource,
		round(min_luminance * 10000), round(max_luminance),
		round(reference_luminance));
	// TODO: target_primaries, target_luminance, target_max_cll, target_max_fall
	wp_image_description_info_v1_send_done(resource);
	wl_resource_destroy(resource);
}

static const struct wp_image_description_v1_interface image_desc_impl = {
	.destroy = resource_handle_destroy,
	.get_information = image_desc_handle_get_information,
};

static void image_desc_create_ready(struct wl_resource *parent_resource, uint32_t id) {
	struct wl_client *client = wl_resource_get_client(parent_resource);
	uint32_t version = wl_resource_get_version(parent_resource);
	struct wl_resource *resource = wl_resource_create(client,
		&wp_image_description_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &image_desc_impl, NULL, NULL);

	wp_image_description_v1_send_ready(resource, 0);
}

static const struct wp_color_management_output_v1_interface cm_output_impl;

static struct wlr_color_management_output_v1 *cm_output_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_color_management_output_v1_interface, &cm_output_impl));
	return wl_resource_get_user_data(resource);
}

static void cm_output_handle_get_image_description(struct wl_client *client,
		struct wl_resource *cm_output_resource, uint32_t id) {
	image_desc_create_ready(cm_output_resource, id);
}

static const struct wp_color_management_output_v1_interface cm_output_impl = {
	.destroy = resource_handle_destroy,
	.get_image_description = cm_output_handle_get_image_description,
};

static void cm_output_destroy(struct wlr_color_management_output_v1 *cm_output) {
	if (cm_output == NULL) {
		return;
	}
	wl_resource_set_user_data(cm_output->resource, NULL); // make inert
	wl_list_remove(&cm_output->output_destroy.link);
	wl_list_remove(&cm_output->link);
	free(cm_output);
}

static void cm_output_handle_output_destroy(struct wl_listener *listener, void *data) {
	struct wlr_color_management_output_v1 *cm_output = wl_container_of(listener, cm_output, output_destroy);
	cm_output_destroy(cm_output);
}

static void cm_output_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_color_management_output_v1 *cm_output = cm_output_from_resource(resource);
	cm_output_destroy(cm_output);
}

static void cm_surface_destroy(struct wlr_color_management_surface_v1 *cm_surface) {
	if (cm_surface == NULL) {
		return;
	}
	wl_resource_set_user_data(cm_surface->resource, NULL); // make inert
	wlr_addon_finish(&cm_surface->addon);
	free(cm_surface);
}

static void cm_surface_handle_addon_destroy(struct wlr_addon *addon) {
	struct wlr_color_management_surface_v1 *cm_surface = wl_container_of(addon, cm_surface, addon);
	cm_surface_destroy(cm_surface);
}

static const struct wlr_addon_interface cm_surface_addon_impl = {
	.name = "wlr_color_management_surface_v1",
	.destroy = cm_surface_handle_addon_destroy,
};

static const struct wp_color_management_surface_v1_interface cm_surface_impl;

static struct wlr_color_management_surface_v1 *cm_surface_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_color_management_surface_v1_interface, &cm_surface_impl));
	return wl_resource_get_user_data(resource);
}

static void cm_surface_handle_set_image_description(struct wl_client *client,
		struct wl_resource *cm_surface_resource,
		struct wl_resource *image_desc_resource, uint32_t render_intent) {
	struct wlr_color_management_surface_v1 *cm_surface = cm_surface_from_resource(cm_surface_resource);
	if (cm_surface == NULL) {
		wl_resource_post_error(cm_surface_resource,
			WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT,
			"set_image_description cannot be sent on an inert object");
		return;
	}

	bool found = false;
	for (size_t i = 0; i < cm_surface->manager->render_intents_len; i++) {
		if (cm_surface->manager->render_intents[i] == render_intent) {
			found = true;
			break;
		}
	}
	if (!found) {
		wl_resource_post_error(cm_surface_resource,
			WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT,
			"invalid render intent");
		return;
	}

	// TODO
}

static void cm_surface_handle_unset_image_description(struct wl_client *client,
		struct wl_resource *cm_surface_resource) {
	struct wlr_color_management_surface_v1 *cm_surface = cm_surface_from_resource(cm_surface_resource);
	if (cm_surface == NULL) {
		wl_resource_post_error(cm_surface_resource,
			WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT,
			"set_image_description cannot be sent on an inert object");
		return;
	}

	// TODO
}

static const struct wp_color_management_surface_v1_interface cm_surface_impl = {
	.destroy = resource_handle_destroy,
	.set_image_description = cm_surface_handle_set_image_description,
	.unset_image_description = cm_surface_handle_unset_image_description,
};

static void cm_surface_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_color_management_surface_v1 *cm_surface = cm_surface_from_resource(resource);
	cm_surface_destroy(cm_surface);
}

static const struct wp_color_management_feedback_surface_v1_interface feedback_surface_impl;

static struct wlr_color_management_feedback_surface_v1 *feedback_surface_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_color_management_feedback_surface_v1_interface, &feedback_surface_impl));
	return wl_resource_get_user_data(resource);
}

static void feedback_surface_handle_get_preferred(struct wl_client *client,
		struct wl_resource *feedback_surface_resource, uint32_t id) {
	image_desc_create_ready(feedback_surface_resource, id);
}

static const struct wp_color_management_feedback_surface_v1_interface feedback_surface_impl = {
	.destroy = resource_handle_destroy,
	.get_preferred = feedback_surface_handle_get_preferred,
};

static void feedback_surface_destroy(struct wlr_color_management_feedback_surface_v1 *feedback_surface) {
	if (feedback_surface == NULL) {
		return;
	}
	wl_resource_set_user_data(feedback_surface->resource, NULL); // make inert
	wl_list_remove(&feedback_surface->surface_destroy.link);
	free(feedback_surface);
}

static void feedback_surface_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_color_management_feedback_surface_v1 *feedback_surface =
		feedback_surface_from_resource(resource);
	feedback_surface_destroy(feedback_surface);
}

static void feedback_surface_handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct wlr_color_management_feedback_surface_v1 *feedback_surface =
		wl_container_of(listener, feedback_surface, surface_destroy);
	feedback_surface_destroy(feedback_surface);
}

static const struct wp_color_manager_v1_interface manager_impl;

static struct wlr_color_manager_v1 *manager_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_color_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

static void manager_handle_get_output(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *output_resource) {
	struct wlr_color_manager_v1 *manager = manager_from_resource(manager_resource);
	struct wlr_output *output = wlr_output_from_resource(output_resource);

	struct wlr_color_management_output_v1 *cm_output = calloc(1, sizeof(*cm_output));
	if (cm_output == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	uint32_t version = wl_resource_get_version(manager_resource);
	cm_output->resource = wl_resource_create(client,
		&wp_color_management_output_v1_interface, version, id);
	if (!cm_output->resource) {
		wl_client_post_no_memory(client);
		free(cm_output);
		return;
	}
	wl_resource_set_implementation(cm_output->resource, &cm_output_impl,
		cm_output, cm_output_handle_resource_destroy);

	cm_output->output_destroy.notify = cm_output_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &cm_output->output_destroy);

	wl_list_insert(&manager->outputs, &cm_output->link);
}

static void manager_handle_get_surface(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct wlr_color_manager_v1 *manager = manager_from_resource(manager_resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

	if (wlr_addon_find(&surface->addons, NULL, &cm_surface_addon_impl) != NULL) {
		wl_resource_post_error(manager_resource,
			WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"wp_color_management_surface_v1 already constructed for this surface");
		return;
	}

	struct wlr_color_management_surface_v1 *cm_surface = calloc(1, sizeof(*cm_surface));
	if (cm_surface == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	uint32_t version = wl_resource_get_version(manager_resource);
	cm_surface->resource = wl_resource_create(client,
		&wp_color_management_surface_v1_interface, version, id);
	if (!cm_surface->resource) {
		wl_client_post_no_memory(client);
		free(cm_surface);
		return;
	}
	wl_resource_set_implementation(cm_surface->resource, &cm_surface_impl, cm_surface, cm_surface_handle_resource_destroy);

	cm_surface->manager = manager;
	cm_surface->surface = surface;

	wlr_addon_init(&cm_surface->addon, &surface->addons, NULL, &cm_surface_addon_impl);
}

static void manager_handle_get_feedback_surface(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

	struct wlr_color_management_feedback_surface_v1 *feedback_surface =
		calloc(1, sizeof(*feedback_surface));
	if (feedback_surface == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	uint32_t version = wl_resource_get_version(manager_resource);
	feedback_surface->resource = wl_resource_create(client,
		&wp_color_management_feedback_surface_v1_interface, version, id);
	if (!feedback_surface->resource) {
		wl_client_post_no_memory(client);
		free(feedback_surface);
		return;
	}
	wl_resource_set_implementation(feedback_surface->resource, &feedback_surface_impl,
		feedback_surface, feedback_surface_handle_resource_destroy);

	feedback_surface->surface = surface;

	feedback_surface->surface_destroy.notify = feedback_surface_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &feedback_surface->surface_destroy);
}

static void manager_handle_new_icc_creator(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id) {
	wl_resource_post_error(manager_resource,
		WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
		"new_icc_creator is not supported");
}

static void manager_handle_new_parametric_creator(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id) {
	wl_resource_post_error(manager_resource,
		WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
		"new_parametric_creator is not supported");
}

static void manager_handle_get_windows_scrgb(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id) {
	wl_resource_post_error(manager_resource,
		WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
		"get_windows_scrgb is not supported");
}

static const struct wp_color_manager_v1_interface manager_impl = {
	.destroy = resource_handle_destroy,
	.get_output = manager_handle_get_output,
	.get_surface = manager_handle_get_surface,
	.get_feedback_surface = manager_handle_get_feedback_surface,
	.new_icc_creator = manager_handle_new_icc_creator,
	.new_parametric_creator = manager_handle_new_parametric_creator,
	.get_windows_scrgb = manager_handle_get_windows_scrgb,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_color_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&wp_color_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);

	const bool features[] = {
		[WP_COLOR_MANAGER_V1_FEATURE_ICC_V2_V4] = manager->features.icc_v2_v4,
		[WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC] = manager->features.parametric,
		[WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES] = manager->features.set_primaries,
		[WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER] = manager->features.set_tf_power,
		[WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES] = manager->features.set_mastering_display_primaries,
		[WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME] = manager->features.extended_target_volume,
		[WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB] = manager->features.windows_scrgb,
	};

	for (uint32_t i = 0; i < sizeof(features) / sizeof(features[0]); i++) {
		if (features[i]) {
			wp_color_manager_v1_send_supported_feature(resource, i);
		}
	}
	for (size_t i = 0; i < manager->render_intents_len; i++) {
		wp_color_manager_v1_send_supported_intent(resource,
			manager->render_intents[i]);
	}
	for (size_t i = 0; i < manager->transfer_functions_len; i++) {
		wp_color_manager_v1_send_supported_tf_named(resource,
			manager->transfer_functions[i]);
	}
	for (size_t i = 0; i < manager->primaries_len; i++) {
		wp_color_manager_v1_send_supported_primaries_named(resource,
			manager->primaries[i]);
	}

	wp_color_manager_v1_send_done(resource);
}

static void manager_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_color_manager_v1 *manager = wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager->render_intents);
	free(manager->transfer_functions);
	free(manager->primaries);
	free(manager);
}

static bool memdup(void *out, const void *src, size_t size) {
	void *dst = malloc(size);
	if (dst == NULL) {
		return false;
	}
	memcpy(dst, src, size);
	void **dst_ptr = out;
	*dst_ptr = dst;
	return true;
}

struct wlr_color_manager_v1 *wlr_color_manager_v1_create(struct wl_display *display,
		uint32_t version, const struct wlr_color_manager_v1_options *options) {
	assert(version <= COLOR_MANAGEMENT_V1_VERSION);

	bool has_perceptual_render_intent = false;
	for (size_t i = 0; i < options->render_intents_len; i++) {
		if (options->render_intents[i] == WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL) {
			has_perceptual_render_intent = true;
		}
	}
	assert(has_perceptual_render_intent);

	// TODO: add support for all of these features
	assert(!options->features.icc_v2_v4);
	assert(!options->features.parametric);
	assert(!options->features.set_primaries);
	assert(!options->features.set_tf_power);
	assert(!options->features.set_luminances);
	assert(!options->features.set_mastering_display_primaries);
	assert(!options->features.extended_target_volume);
	assert(!options->features.windows_scrgb);

	struct wlr_color_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->features = options->features;

	bool ok =
		memdup(&manager->render_intents, options->render_intents, sizeof(options->render_intents[0]) * options->render_intents_len) &&
		memdup(&manager->transfer_functions, options->transfer_functions, sizeof(options->transfer_functions[0]) * options->transfer_functions_len) &&
		memdup(&manager->primaries, options->primaries, sizeof(options->primaries[0]) * options->primaries_len);
	if (!ok) {
		goto err_options;
	}

	manager->global = wl_global_create(display, &wp_color_manager_v1_interface,
		version, manager, manager_bind);
	if (manager->global == NULL) {
		goto err_options;
	}

	manager->display_destroy.notify = manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;

err_options:
	free(manager->render_intents);
	free(manager->transfer_functions);
	free(manager->primaries);
	free(manager);
	return NULL;
}
