#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_fifo_v1.h>
#include <wlr/util/log.h>
#include "fifo-v1-protocol.h"

#define FIFO_MANAGER_VERSION 1

struct fifo_commit {
	struct wl_list link; // wlr_fifo_v1.fifo_commits
	bool set_barrier_pending;
	uint32_t seq;
};

static bool commit_on_valid_buffer(const struct wlr_surface * const surface) {
	if (!surface->buffer || (surface->pending.committed & WLR_SURFACE_STATE_BUFFER &&
			surface->pending.buffer == NULL)) {
		return false;
	}

	return true;
}

static void fifo_handle_commit(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, surface_commit);
	struct wlr_surface *surface = fifo->surface;

	if (!surface->buffer) {
		return;
	}

	/*
	 * If the commit that has just been applied has a .set_barrier request, we set the
	 * fifo barrier on the surface.
	 *
	 * A fifo_barrier event is sent, and the listener is responsible for clearing the
	 * fifo barrier for the surface by calling wlr_fifo_v1_signal_barrier(). */
	if (!fifo->fifo_barrier && fifo->set_barrier) {
		fifo->set_barrier = false;

		fifo->fifo_barrier = true; // set the fifo barrier on the surface
		fifo->barrier_commit_seq = fifo->surface->current.seq;
		wl_signal_emit_mutable(&fifo->fifo_manager->events.barrier_set, fifo);
	}
}

static void fifo_handle_client_commit(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, surface_client_commit);

	if (!commit_on_valid_buffer(fifo->surface)) {
		return;
	}

	if (fifo->wait_barrier) {
		fifo->wait_barrier = false;

		/*
		 * When a .commit request with a .set_barrier has been applied, the fifo barrier is set on
		 * the surface. This barrier allows further commits to wait on it through the .wait_barrier
		 * request. If the barrier is not set, .wait_barrier is effectively a no-op. */
		if (fifo->fifo_barrier) {
			struct fifo_commit *commit = calloc(1, sizeof(*commit));
			if (!commit) {
				wl_client_post_no_memory(wl_resource_get_client(fifo->resource));
				return;
			}

			/*
			 * If the upcomming commit also has a .set_barrier request, we set 'set_barrier_pending'
			 * on the locked commit so that the request is not lost. */
			if (fifo->set_barrier) {
				fifo->set_barrier = false;
				commit->set_barrier_pending = true;
			}

			commit->seq = wlr_surface_lock_pending(fifo->surface);
			wl_list_insert(fifo->commits.prev, &commit->link);
		}
	}
}

static const struct wp_fifo_v1_interface fifo_implementation;
static struct wlr_fifo_v1 *wlr_fifo_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_fifo_v1_interface,
		&fifo_implementation));
	return wl_resource_get_user_data(resource);
}

static void fifo_handle_wait_barrier(struct wl_client *client,
		struct wl_resource *resource) {
	struct wlr_fifo_v1 *fifo =
		wlr_fifo_v1_from_resource(resource);
	fifo->wait_barrier = true;
}

static void fifo_handle_set_barrier(struct wl_client *client,
		struct wl_resource *resource) {
	struct wlr_fifo_v1 *fifo =
		wlr_fifo_v1_from_resource(resource);
	fifo->set_barrier = true;
}

static void fifo_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_fifo_v1 *fifo = wlr_fifo_v1_from_resource(resource);
	wlr_addon_finish(&fifo->addon);
	wl_list_remove(&fifo->surface_client_commit.link);
	wl_list_remove(&fifo->surface_commit.link);
	free(fifo);
}

static void fifo_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void surface_fifo_addon_handle_destroy(struct wlr_addon *addon) {
	struct wlr_fifo_v1 *fifo = wl_container_of(addon, fifo, addon);
	wl_resource_destroy(fifo->resource);
}

static const struct wlr_addon_interface surface_fifo_addon_impl = {
	.name = "wp_fifo_v1",
	.destroy = surface_fifo_addon_handle_destroy,
};

static const struct wp_fifo_v1_interface fifo_implementation = {
	.destroy = fifo_handle_destroy,
	.set_barrier = fifo_handle_set_barrier,
	.wait_barrier = fifo_handle_wait_barrier
};

static struct wlr_fifo_v1 *fifo_create(struct wl_client *client, uint32_t version, uint32_t id,
		struct wlr_surface *surface) {
	struct wlr_fifo_v1 *fifo = calloc(1, sizeof(*fifo));
	if (!fifo) {
		goto err_alloc;
	}
	fifo->surface = surface;
	wl_list_init(&fifo->commits);

	fifo->resource = wl_resource_create(client, &wp_fifo_v1_interface, version, id);
	if (fifo->resource == NULL) {
		goto err_alloc;
	}
	wl_resource_set_implementation(fifo->resource, &fifo_implementation, fifo,
		fifo_handle_resource_destroy);

	fifo->surface_client_commit.notify = fifo_handle_client_commit;
	wl_signal_add(&surface->events.client_commit, &fifo->surface_client_commit);
	fifo->surface_commit.notify = fifo_handle_commit;
	wl_signal_add(&surface->events.commit, &fifo->surface_commit);

	wlr_log(WLR_DEBUG, "New wlr_fifo_v1 %p (res %p)", fifo, fifo->resource);

	return fifo;

err_alloc:
	free(fifo);
	return NULL;
}

static const struct wp_fifo_manager_v1_interface fifo_manager_impl;
static struct wlr_fifo_manager_v1 *wlr_fifo_manager_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_fifo_manager_v1_interface,
		&fifo_manager_impl));
	return wl_resource_get_user_data(resource);
}

static void fifo_manager_handle_get_fifo(struct wl_client *wl_client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (wlr_addon_find(&surface->addons, NULL, &surface_fifo_addon_impl) != NULL) {
		wl_resource_post_error(resource,
			WP_FIFO_MANAGER_V1_ERROR_FIFO_MANAGER_ALREADY_EXISTS,
			"A wp_fifo_v1 object already exists for this surface");
		return;
	}

	struct wlr_fifo_v1 *fifo =
		fifo_create(wl_client, wl_resource_get_version(resource), id, surface);
	if (!fifo) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wlr_addon_init(&fifo->addon, &surface->addons, NULL, &surface_fifo_addon_impl);

	struct wlr_fifo_manager_v1 *fifo_manager =
		wlr_fifo_manager_v1_from_resource(resource);

	fifo->fifo_manager = fifo_manager;
}

static void fifo_manager_handle_destroy(struct wl_client *wl_client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_fifo_manager_v1_interface fifo_manager_impl = {
	.get_fifo = fifo_manager_handle_get_fifo,
	.destroy = fifo_manager_handle_destroy,
};

static void fifo_manager_bind(struct wl_client *wl_client, void *data, uint32_t version,
		uint32_t id) {
	struct wlr_fifo_manager_v1 *fifo_manager = data;
	struct wl_resource *resource =
		wl_resource_create(wl_client, &wp_fifo_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &fifo_manager_impl, fifo_manager, NULL);
}

static void fifo_manager_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_fifo_manager_v1 *fifo_manager =
		wl_container_of(listener, fifo_manager, display_destroy);
	wl_signal_emit_mutable(&fifo_manager->events.destroy, fifo_manager);
	wl_list_remove(&fifo_manager->display_destroy.link);
	wl_global_destroy(fifo_manager->global);
	free(fifo_manager);
}

struct wlr_fifo_manager_v1 *wlr_fifo_manager_v1_create(struct wl_display *display, uint32_t version) {
	assert(version <= FIFO_MANAGER_VERSION);

	struct wlr_fifo_manager_v1 *fifo_manager = calloc(1, sizeof(*fifo_manager));
	if (!fifo_manager) {
		return NULL;
	}

	fifo_manager->global = wl_global_create(display, &wp_fifo_manager_v1_interface,
		version, fifo_manager, fifo_manager_bind);
	if (!fifo_manager->global) {
		free(fifo_manager);
		return NULL;
	}

	wl_signal_init(&fifo_manager->events.destroy);
	wl_signal_init(&fifo_manager->events.barrier_set);

	fifo_manager->display_destroy.notify = fifo_manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &fifo_manager->display_destroy);

	return fifo_manager;
}

void wlr_fifo_v1_signal_barrier(struct wlr_surface *surface) {
	struct wlr_addon *addon = wlr_addon_find(&surface->addons,
		NULL, &surface_fifo_addon_impl);
	assert(addon);
	struct wlr_fifo_v1 *fifo = wl_container_of(addon, fifo, addon);

	fifo->fifo_barrier = false; // clear the 'fifo_barrier' condition on the surface

	/*
	 * Unlock all the commits that where waiting on the fifo barrier, in order of commit.
	 * If any of the commits has a pending .set_barrier request, we set
	 * 'fifo->set_barrier' so that we can set again the fifo barrier when the commit is
	 * unlocked and applied, and we break from the loop so that further commits wait on
	 * the new fifo barrier. */
	struct fifo_commit *commit, *tmp;
	bool barrier = false;
	wl_list_for_each_safe(commit, tmp, &fifo->commits, link) {
		wl_list_remove(&commit->link);

		if (commit->set_barrier_pending) {
			barrier = fifo->set_barrier = true;
		}
		wlr_surface_unlock_cached(fifo->surface, commit->seq);

		free(commit);
		if (barrier) {
			break;
		}
	}
}
