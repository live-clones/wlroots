#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_xdg_session_management_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "xdg-session-management-v1-protocol.h"

#define MANAGER_VERSION 1

static const struct xdg_toplevel_session_v1_interface toplevel_session_impl;
static const struct xdg_session_v1_interface session_impl;
static const struct xdg_session_manager_v1_interface manager_impl;

static struct wlr_xdg_toplevel_session_v1 *toplevel_session_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_toplevel_session_v1_interface, &toplevel_session_impl));
	return wl_resource_get_user_data(resource);
}

static struct wlr_xdg_session_v1 *session_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_session_v1_interface, &session_impl));
	return wl_resource_get_user_data(resource);
}

static struct wlr_xdg_session_manager_v1 *manager_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_session_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

static struct wlr_xdg_toplevel_session_v1 *session_find_toplevel(struct wlr_xdg_session_v1 *session,
	const char *name);

static void toplevel_session_handle_rename(struct wl_client *client,
		struct wl_resource *toplevel_session_resource, const char *name) {
	struct wlr_xdg_toplevel_session_v1 *toplevel_session =
		toplevel_session_from_resource(toplevel_session_resource);
	if (toplevel_session == NULL) {
		return;
	}

	if (strcmp(toplevel_session->name, name) == 0) {
		return;
	}
	if (session_find_toplevel(toplevel_session->session, name) != NULL) {
		wl_resource_post_error(toplevel_session_resource,
			XDG_SESSION_V1_ERROR_NAME_IN_USE,
			"Name already in use by another toplevel in the same session");
		return;
	}

	char *name_copy = strdup(name);
	if (name_copy == NULL) {
		wl_resource_post_no_memory(toplevel_session_resource);
		return;
	}

	free(toplevel_session->name);
	toplevel_session->name = name_copy;

	wl_signal_emit_mutable(&toplevel_session->events.rename, NULL);
}

static void toplevel_session_handle_destroy(struct wl_client *client,
		struct wl_resource *toplevel_session_resource) {
	wl_resource_destroy(toplevel_session_resource);
}

static const struct xdg_toplevel_session_v1_interface toplevel_session_impl = {
	.destroy = toplevel_session_handle_destroy,
	.rename = toplevel_session_handle_rename,
};

uint32_t wlr_xdg_toplevel_session_v1_notify_restored(
		struct wlr_xdg_toplevel_session_v1 *toplevel_session) {
	assert(toplevel_session->restorable);
	toplevel_session->restorable = false;

	xdg_toplevel_session_v1_send_restored(toplevel_session->resource);
	return wlr_xdg_surface_schedule_configure(toplevel_session->toplevel->base);
}

static void toplevel_session_destroy(struct wlr_xdg_toplevel_session_v1 *toplevel_session) {
	if (toplevel_session == NULL) {
		return;
	}

	wl_list_remove(&toplevel_session->toplevel_destroy.link);

	wl_signal_emit_mutable(&toplevel_session->events.destroy, NULL);

	assert(wl_list_empty(&toplevel_session->events.destroy.listener_list));
	assert(wl_list_empty(&toplevel_session->events.rename.listener_list));

	wl_resource_set_user_data(toplevel_session->resource, NULL); // make inert
	wl_list_remove(&toplevel_session->link);
	free(toplevel_session->name);
	free(toplevel_session);
}

static void toplevel_session_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_xdg_toplevel_session_v1 *toplevel_session = toplevel_session_from_resource(resource);
	toplevel_session_destroy(toplevel_session);
}

static void toplevel_session_handle_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_session_v1 *toplevel_session =
		wl_container_of(listener, toplevel_session, toplevel_destroy);
	toplevel_session_destroy(toplevel_session);
}

static void session_handle_remove(struct wl_client *client, struct wl_resource *session_resource) {
	struct wlr_xdg_session_v1 *session = session_from_resource(session_resource);
	wl_signal_emit_mutable(&session->events.remove, NULL);
}

static struct wlr_xdg_toplevel_session_v1 *session_find_toplevel(struct wlr_xdg_session_v1 *session,
		const char *name) {
	struct wlr_xdg_toplevel_session_v1 *toplevel_session;
	wl_list_for_each(toplevel_session, &session->toplevels, link) {
		if (strcmp(toplevel_session->name, name) == 0) {
			return toplevel_session;
		}
	}
	return NULL;
}

static struct wlr_xdg_toplevel_session_v1 *session_create_toplevel(struct wl_resource *session_resource,
		uint32_t id, struct wl_resource *toplevel_resource, const char *name) {
	struct wlr_xdg_session_v1 *session = session_from_resource(session_resource);
	struct wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);

	struct wl_client *client = wl_resource_get_client(session_resource);
	uint32_t version = wl_resource_get_version(session_resource);
	struct wl_resource *resource = wl_resource_create(client,
		&xdg_toplevel_session_v1_interface, version, id);
	if (resource == NULL) {
		wl_resource_post_no_memory(session_resource);
		return NULL;
	}
	wl_resource_set_implementation(resource, &toplevel_session_impl, NULL,
		toplevel_session_handle_resource_destroy);

	if (session == NULL || toplevel == NULL) {
		return NULL;
	}

	if (session_find_toplevel(session, name) != NULL) {
		wl_resource_post_error(session->resource, XDG_SESSION_V1_ERROR_NAME_IN_USE,
			"Name already in use by a toplevel in the same session");
		return NULL;
	}

	// TODO: post already_added error if toplevel was already added to *any* session
	struct wlr_xdg_toplevel_session_v1 *toplevel_session;
	wl_list_for_each(toplevel_session, &session->toplevels, link) {
		if (toplevel_session->toplevel == toplevel) {
			wl_resource_post_error(session->resource, XDG_SESSION_V1_ERROR_ALREADY_ADDED,
				"Toplevel already added to session");
			return NULL;
		}
	}

	toplevel_session = calloc(1, sizeof(*toplevel_session));
	if (toplevel_session == NULL) {
		wl_resource_post_no_memory(session_resource);
		return NULL;
	}

	toplevel_session->resource = resource;
	toplevel_session->session = session;
	toplevel_session->toplevel = toplevel;

	toplevel_session->name = strdup(name);
	if (toplevel_session->name == NULL) {
		wl_resource_post_no_memory(session_resource);
		free(toplevel_session);
		return NULL;
	}

	toplevel_session->toplevel_destroy.notify = toplevel_session_handle_toplevel_destroy;
	wl_signal_add(&toplevel->events.destroy, &toplevel_session->toplevel_destroy);

	wl_resource_set_user_data(resource, toplevel_session);
	wl_list_insert(&session->toplevels, &toplevel_session->link);

	return toplevel_session;
}

static void session_handle_add_toplevel(struct wl_client *client, struct wl_resource *session_resource,
		uint32_t id, struct wl_resource *toplevel_resource, const char *name) {
	struct wlr_xdg_session_v1 *session = session_from_resource(session_resource);
	struct wlr_xdg_toplevel_session_v1 *toplevel_session =
		session_create_toplevel(session_resource, id, toplevel_resource, name);
	if (toplevel_session == NULL) {
		return;
	}

	wl_signal_emit_mutable(&session->events.add_toplevel, toplevel_session);
}

static void session_handle_restore_toplevel(struct wl_client *client, struct wl_resource *session_resource,
		uint32_t id, struct wl_resource *toplevel_resource, const char *name) {
	struct wlr_xdg_session_v1 *session = session_from_resource(session_resource);
	struct wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);
	if (toplevel && toplevel->base->surface->mapped) {
		wl_resource_post_error(session_resource, XDG_SESSION_V1_ERROR_ALREADY_MAPPED,
			"Restored toplevel is already mapped");
		return;
	}

	struct wlr_xdg_toplevel_session_v1 *toplevel_session =
		session_create_toplevel(session_resource, id, toplevel_resource, name);
	if (toplevel_session == NULL) {
		return;
	}

	toplevel_session->restorable = true;

	wl_signal_emit_mutable(&session->events.restore_toplevel, toplevel_session);
}

static void session_handle_remove_toplevel(struct wl_client *client, struct wl_resource *session_resource,
		const char *name) {
	struct wlr_xdg_session_v1 *session = session_from_resource(session_resource);
	if (session == NULL) {
		return;
	}

	struct wlr_xdg_session_v1_remove_toplevel_event event = {
		.name = name,
	};
	wl_signal_emit_mutable(&session->events.remove_toplevel, &event);

	struct wlr_xdg_toplevel_session_v1 *toplevel_session = session_find_toplevel(session, name);
	toplevel_session_destroy(toplevel_session);
}

static void session_handle_destroy(struct wl_client *client, struct wl_resource *session_resource) {
	wl_resource_destroy(session_resource);
}

static const struct xdg_session_v1_interface session_impl = {
	.destroy = session_handle_destroy,
	.remove = session_handle_remove,
	.add_toplevel = session_handle_add_toplevel,
	.restore_toplevel = session_handle_restore_toplevel,
	.remove_toplevel = session_handle_remove_toplevel,
};

static void session_destroy(struct wlr_xdg_session_v1 *session) {
	if (session == NULL) {
		return;
	}

	wl_signal_emit_mutable(&session->events.destroy, NULL);

	assert(wl_list_empty(&session->events.destroy.listener_list));
	assert(wl_list_empty(&session->events.remove.listener_list));
	assert(wl_list_empty(&session->events.add_toplevel.listener_list));
	assert(wl_list_empty(&session->events.restore_toplevel.listener_list));
	assert(wl_list_empty(&session->events.remove_toplevel.listener_list));

	struct wlr_xdg_toplevel_session_v1 *toplevel_session, *toplevel_session_tmp;
	wl_list_for_each_safe(toplevel_session, toplevel_session_tmp, &session->toplevels, link) {
		toplevel_session_destroy(toplevel_session);
	}

	wl_resource_set_user_data(session->resource, NULL); // make inert
	wl_list_remove(&session->link);
	free(session->id);
	free(session);
}

static void session_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_xdg_session_v1 *session = session_from_resource(resource);
	session_destroy(session);
}

void wlr_xdg_session_v1_notify_created(struct wlr_xdg_session_v1 *session, const char *session_id) {
	assert(!session->initialized);

	char *session_id_copy = strdup(session_id);
	if (session_id_copy == NULL) {
		wl_resource_post_no_memory(session->resource);
		return;
	}
	free(session->id);
	session->id = session_id_copy;

	xdg_session_v1_send_created(session->resource, session_id);

	session->initialized = true;
}

void wlr_xdg_session_v1_notify_restored(struct wlr_xdg_session_v1 *session) {
	assert(!session->initialized);
	assert(session->id != NULL);

	xdg_session_v1_send_restored(session->resource);

	session->initialized = true;
}

void wlr_xdg_session_v1_notify_replaced_and_destroy(struct wlr_xdg_session_v1 *session) {
	xdg_session_v1_send_replaced(session->resource);
	session_destroy(session);
}

static void manager_handle_get_session(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id, uint32_t reason,
		const char *session_id) {
	struct wlr_xdg_session_manager_v1 *manager = manager_from_resource(manager_resource);
	uint32_t version = wl_resource_get_version(manager_resource);

	if (!xdg_session_manager_v1_reason_is_valid(reason, version)) {
		wl_resource_post_error(manager_resource, XDG_SESSION_MANAGER_V1_ERROR_INVALID_REASON,
			"Invalid reason");
		return;
	}

	struct wlr_xdg_session_v1 *sess;
	wl_list_for_each(sess, &manager->sessions, link) {
		if (wl_resource_get_client(sess->resource) == client && sess->id != NULL &&
				strcmp(sess->id, session_id) == 0) {
			wl_resource_post_error(manager_resource,
				XDG_SESSION_MANAGER_V1_ERROR_IN_USE,
				"Session ID already in use by another session object");
			return;
		}
	}

	struct wlr_xdg_session_v1 *session = calloc(1, sizeof(*session));
	if (session == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	char *session_id_copy = NULL;
	if (session_id != NULL) {
		session_id_copy = strdup(session_id);
		if (session_id_copy == NULL) {
			wl_client_post_no_memory(client);
			free(session);
			return;
		}
	}

	session->resource = wl_resource_create(client,
		&xdg_session_v1_interface, version, id);
	if (session->resource == NULL) {
		wl_resource_post_no_memory(manager_resource);
		free(session);
		return;
	}
	wl_resource_set_implementation(session->resource, &session_impl, session,
		session_handle_resource_destroy);

	wl_signal_init(&session->events.destroy);
	wl_signal_init(&session->events.remove);
	wl_signal_init(&session->events.add_toplevel);
	wl_signal_init(&session->events.restore_toplevel);
	wl_signal_init(&session->events.remove_toplevel);

	session->reason = reason;
	session->id = session_id_copy;
	wl_list_init(&session->toplevels);

	wl_list_insert(&manager->sessions, &session->link);

	wl_signal_emit_mutable(&manager->events.new_session, session);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *manager_resource) {
	wl_resource_destroy(manager_resource);
}

static const struct xdg_session_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_session = manager_handle_get_session,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wlr_xdg_session_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&xdg_session_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

static void manager_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_session_manager_v1 *manager = wl_container_of(listener, manager, display_destroy);

	wl_signal_emit_mutable(&manager->events.destroy, NULL);

	assert(wl_list_empty(&manager->events.destroy.listener_list));
	assert(wl_list_empty(&manager->events.new_session.listener_list));

	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_xdg_session_manager_v1 *wlr_xdg_session_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= MANAGER_VERSION);

	struct wlr_xdg_session_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display, &xdg_session_manager_v1_interface,
		version, manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	wl_signal_init(&manager->events.destroy);
	wl_signal_init(&manager->events.new_session);

	wl_list_init(&manager->sessions);

	manager->display_destroy.notify = manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
