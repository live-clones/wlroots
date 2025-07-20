#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <types/wlr_foreign_toplevel.h>
#include <wlr/types/wlr_ext_foreign_toplevel_request_v1.h>
#include "ext-foreign-toplevel-request-v1-protocol.h"

#define FOREIGN_TOPLEVEL_REQUEST_V1_VERSION 1

struct ext_foreign_toplevel_request_v1_interface foreign_toplevel_request_v1_impl;
static struct wlr_ext_foreign_toplevel_request_v1 *
		foreign_toplevel_request_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_foreign_toplevel_request_v1_interface, &foreign_toplevel_request_v1_impl));
	return wl_resource_get_user_data(resource);
}

static void foreign_toplevel_request_v1_destroy(struct wl_client *client, struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_foreign_toplevel_request_v1_interface, &foreign_toplevel_request_v1_impl));
	wl_resource_destroy(resource);
}

static void foreign_toplevel_request_v1_resource_destroy(struct wl_resource *resource) {
	struct wlr_ext_foreign_toplevel_request_v1 *request =
		foreign_toplevel_request_v1_from_resource(resource);
	wl_signal_emit_mutable(&request->events.destroy, NULL);
	free(request);
}

struct ext_foreign_toplevel_request_v1_interface foreign_toplevel_request_v1_impl = {
	.destroy = foreign_toplevel_request_v1_destroy,
};

static const struct ext_foreign_toplevel_request_manager_v1_interface foreign_toplevel_request_manager_v1_impl;
static struct wlr_ext_foreign_toplevel_request_manager_v1 *
		foreign_toplevel_request_manager_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_foreign_toplevel_request_manager_v1_interface,
		&foreign_toplevel_request_manager_v1_impl));
	return wl_resource_get_user_data(resource);
}

static void foreign_toplevel_request_manager_request(
		struct wl_client *client, struct wl_resource *resource, uint32_t request) {
	struct wlr_ext_foreign_toplevel_request_manager_v1 *manager
		= foreign_toplevel_request_manager_v1_from_resource(resource);
	struct wlr_ext_foreign_toplevel_request_v1 *wlr_request = calloc(1, sizeof(*wlr_request));
	if (!wlr_request) {
		wl_resource_post_no_memory(resource);
		return;
	}

	wlr_request->manager = resource;
	wlr_request->resource = wl_resource_create(client,
		&ext_foreign_toplevel_request_v1_interface,
		ext_foreign_toplevel_request_v1_interface.version, request);
	if (!wlr_request->resource) {
		wl_resource_post_no_memory(resource);
		free(wlr_request);
		return;
	}

	wl_signal_init(&wlr_request->events.destroy);

	wl_resource_set_implementation(wlr_request->resource, &foreign_toplevel_request_v1_impl,
		wlr_request, foreign_toplevel_request_v1_resource_destroy);

	wl_signal_emit_mutable(&manager->events.request, wlr_request);
}

static void foreign_toplevel_request_manager_destroy(
		struct wl_client *client, struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_foreign_toplevel_request_manager_v1_interface,
		&foreign_toplevel_request_manager_v1_impl));

	wl_resource_destroy(resource);
}

static const struct ext_foreign_toplevel_request_manager_v1_interface foreign_toplevel_request_manager_v1_impl = {
	.request = foreign_toplevel_request_manager_request,
	.destroy = foreign_toplevel_request_manager_destroy
};

static void foreign_toplevel_request_manager_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void foreign_toplevel_request_manager_bind(
		struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wlr_ext_foreign_toplevel_request_manager_v1 *manager = data;
	struct wl_resource *resource = wl_resource_create(client,
			&ext_foreign_toplevel_request_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &foreign_toplevel_request_manager_v1_impl,
			manager, foreign_toplevel_request_manager_resource_destroy);
	wl_list_insert(&manager->resources, wl_resource_get_link(resource));
}

static void manager_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_ext_foreign_toplevel_request_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_signal_emit_mutable(&manager->events.destroy, NULL);

	assert(wl_list_empty(&manager->events.destroy.listener_list));

	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_ext_foreign_toplevel_request_manager_v1 *
		wlr_ext_foreign_toplevel_request_manager_v1_create(struct wl_display *display, uint32_t version) {
	assert(version <= FOREIGN_TOPLEVEL_REQUEST_V1_VERSION);

	struct wlr_ext_foreign_toplevel_request_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		return NULL;
	}

	manager->global = wl_global_create(display,
		&ext_foreign_toplevel_request_manager_v1_interface,
		version, manager, foreign_toplevel_request_manager_bind);
	if (!manager->global) {
		free(manager);
		return NULL;
	}

	wl_list_init(&manager->resources);

	wl_signal_init(&manager->events.request);
	wl_signal_init(&manager->events.destroy);

	manager->display_destroy.notify = manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}

struct wl_resource *create_toplevel_resource_for_resource(
		struct wlr_ext_foreign_toplevel_handle_v1 *toplevel, struct wl_resource *list_resource);

void wlr_ext_foreign_toplevel_request_v1_send_toplevel(
		struct wlr_ext_foreign_toplevel_request_v1 *request, struct wlr_ext_foreign_toplevel_handle_v1 *toplevel) {
	struct wl_resource *resource = foreign_toplevel_create_resource_for_client(
			toplevel, wl_resource_get_client(request->manager));
	ext_foreign_toplevel_request_v1_send_toplevel(request->resource, resource);
	foreign_toplevel_send_details_to_resource(toplevel, resource);
}

void wlr_ext_foreign_toplevel_request_v1_cancel(struct wlr_ext_foreign_toplevel_request_v1 *request) {
	ext_foreign_toplevel_request_v1_send_cancelled(request->resource);
}

struct ext_foreign_toplevel_request_source_v1_interface foreign_toplevel_request_source_v1_impl;
static void foreign_toplevel_request_source_destroy(
		struct wl_client *client, struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_foreign_toplevel_request_source_v1_interface,
		&foreign_toplevel_request_source_v1_impl));

	wl_resource_destroy(resource);
}

struct ext_foreign_toplevel_request_source_v1_interface foreign_toplevel_request_source_v1_impl = {
	.destroy = foreign_toplevel_request_source_destroy
};

static void source_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_ext_foreign_toplevel_request_source_v1 *source =
		wl_container_of(listener, source, display_destroy);
	wl_signal_emit_mutable(&source->events.destroy, NULL);

	assert(wl_list_empty(&source->events.destroy.listener_list));

	wl_list_remove(&source->display_destroy.link);
	wl_global_destroy(source->global);
	free(source);
}

static void foreign_toplevel_request_source_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void foreign_toplevel_request_source_bind(
		struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wlr_ext_foreign_toplevel_request_source_v1 *source = data;
	struct wl_resource *resource = wl_resource_create(client,
		&ext_foreign_toplevel_request_source_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &foreign_toplevel_request_source_v1_impl,
			source, foreign_toplevel_request_source_resource_destroy);
	wl_list_insert(&source->resources, wl_resource_get_link(resource));
}

struct wlr_ext_foreign_toplevel_request_source_v1 *
		wlr_ext_foreign_toplevel_request_source_v1_create(struct wl_display *display, uint32_t version) {
	assert(version <= FOREIGN_TOPLEVEL_REQUEST_V1_VERSION);

	struct wlr_ext_foreign_toplevel_request_source_v1 *source = calloc(1, sizeof(*source));
	if (!source) {
		return NULL;
	}

	source->global = wl_global_create(display,
		&ext_foreign_toplevel_request_source_v1_interface,
		version, source, foreign_toplevel_request_source_bind);
	if (!source->global) {
		free(source);
		return NULL;
	}

	wl_list_init(&source->resources);

	wl_signal_init(&source->events.toplevel);
	wl_signal_init(&source->events.cancel);
	wl_signal_init(&source->events.destroy);

	source->display_destroy.notify = source_handle_display_destroy;
	wl_display_add_destroy_listener(display, &source->display_destroy);

	return source;
}

struct ext_foreign_toplevel_request_pending_v1_interface foreign_toplevel_request_pending_v1_impl;
static struct wlr_ext_foreign_toplevel_request_pending_v1 *
		foreign_toplevel_request_pending_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_foreign_toplevel_request_pending_v1_interface, &foreign_toplevel_request_pending_v1_impl));
	return wl_resource_get_user_data(resource);
}

static void foreign_toplevel_request_pending_v1_resource_destroy(struct wl_resource *resource) {
	free(foreign_toplevel_request_pending_v1_from_resource(resource));
}

static void foreign_toplevel_request_pending_toplevel(
		struct wl_client *client, struct wl_resource *resource, struct wl_resource *toplevel) {
	struct wlr_ext_foreign_toplevel_request_pending_v1 *response =
		foreign_toplevel_request_pending_v1_from_resource(resource);
	response->handle = wlr_ext_foreign_toplevel_handle_v1_from_resource(toplevel);

	wl_signal_emit_mutable(&response->source->events.toplevel, response);
	wl_resource_destroy(resource);
}

static void foreign_toplevel_request_pending_cancel(
		struct wl_client *client, struct wl_resource *resource) {
	struct wlr_ext_foreign_toplevel_request_pending_v1 *response =
		foreign_toplevel_request_pending_v1_from_resource(resource);

	wl_signal_emit_mutable(&response->source->events.cancel, response);
	wl_resource_destroy(resource);
}

struct ext_foreign_toplevel_request_pending_v1_interface foreign_toplevel_request_pending_v1_impl = {
	.toplevel = foreign_toplevel_request_pending_toplevel,
	.cancel = foreign_toplevel_request_pending_cancel
};

#include <stdio.h>

void wlr_ext_foreign_toplevel_request_source_v1_request(
		struct wlr_ext_foreign_toplevel_request_source_v1 *source, struct wlr_ext_foreign_toplevel_request_v1 *request) {
	struct wlr_ext_foreign_toplevel_request_pending_v1 *response = calloc(1, sizeof(*response));
	struct wl_resource *source_resource = wl_container_of(source->resources.next, source_resource, link);
	if (!response) {
		wl_client_post_no_memory(wl_resource_get_client(request->resource));
		return;
	}

	response->source = source;
	response->request = request;
	response->resource = wl_resource_create(wl_resource_get_client(source_resource),
		&ext_foreign_toplevel_request_pending_v1_interface,
		ext_foreign_toplevel_request_pending_v1_interface.version, 0);
	if (!response->resource) {
		wl_client_post_no_memory(wl_resource_get_client(request->resource));
		free(response);
		return;
	}

	wl_resource_set_implementation(response->resource, &foreign_toplevel_request_pending_v1_impl,
		response, foreign_toplevel_request_pending_v1_resource_destroy);

	ext_foreign_toplevel_request_source_v1_send_request(source_resource, response->resource);

	return;
}
