#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_fifo_v1.h>
#include <wlr/util/log.h>
#include "fifo-v1-protocol.h"
#include "util/time.h"

#define FIFO_MANAGER_VERSION 1

struct fifo_commit {
	struct wlr_fifo_v1 *fifo;
	struct wl_list link; // wlr_fifo_v1.fifo_commits
	bool barrier_pending;
	uint32_t seq;
};

static void surface_synced_move_state(void *_dst, void *_src) {
	struct wlr_fifo_v1_fifo_state *dst = _dst, *src = _src;
	dst->set_barrier = src->set_barrier;
	dst->wait_barrier = src->wait_barrier;
	src->wait_barrier = NULL;
	src->set_barrier = NULL;
}

static const struct wlr_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(struct wlr_fifo_v1_fifo_state),
	.move_state = surface_synced_move_state,
};

static bool is_surface_buffer_valid(const struct wlr_surface * const surface) {
	if (!surface->buffer || (surface->pending.committed & WLR_SURFACE_STATE_BUFFER &&
			surface->pending.buffer == NULL)) {
		return false;
	}

	return true;
}

static void commit_remove(struct fifo_commit *commit) {
	wl_list_remove(&commit->link);
	assert(commit->seq);
	wlr_surface_unlock_cached(commit->fifo->surface, commit->seq);
	free(commit);
}

static void fifo_signal_barrier(struct wlr_fifo_v1 *fifo) {
	// dequeue and unlock commits until we find one with a .set_barrier request,
	// in which case leave the barrier condition set.
	struct fifo_commit *commit, *tmp;
	bool barrier_pending = false;
	wl_list_for_each_safe(commit, tmp, &fifo->commits, link) {
		barrier_pending = commit->barrier_pending;
		commit_remove(commit);
		if (barrier_pending) {
			break;
		}
	}

	if (!barrier_pending) {
		fifo->state.barrier_set = false;
	}
}

static void fifo_reset(struct wlr_fifo_v1 *fifo) {
	struct fifo_commit *commit, *tmp_co;
	wl_list_for_each_safe(commit, tmp_co, &fifo->commits, link) {
		commit_remove(commit);
	}
	if (fifo->output) {
		fifo->output_present.notify = NULL;
		wl_list_remove(&fifo->output_present.link);
		fifo->output_destroy.notify = NULL;
		wl_list_remove(&fifo->output_destroy.link);
	}
	if (fifo->surface_occluded_source)
		wl_event_source_remove(fifo->surface_occluded_source);
	memset(&fifo->state, 0, sizeof(fifo->state));
}

static void fifo_handle_output_destroy(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, output_destroy);
	fifo_reset(fifo);
	fifo->output = NULL;
}

// If the surface is occluded, and there is no other surface updating the output's contents,
// then we won't receive any output events. For this case, we introduce a timer ticking at
// a heuristically defined value (40hz) so that we can advance the queue.
static int handle_timer(void *data) {
	struct wlr_fifo_v1 *fifo = data;
	int64_t refresh_nsec = 25000000;
	int64_t now_nsec = get_current_time_msec()*1000000;
	if (fifo->state.barrier_set && (now_nsec - (int64_t)fifo->state.last_output_present_nsec > refresh_nsec)) {
		fifo_signal_barrier(fifo);
	}
	wl_event_source_timer_update(fifo->surface_occluded_source, refresh_nsec/1000000);
	return 0;
}

static void fifo_handle_output_present(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, output_present);
	struct wlr_output_event_present *event = data;

	if (!fifo->surface->buffer) {
		return;
	}

	// We use the output.present event to advance the queue.
	if (fifo->state.barrier_set) {
		fifo_signal_barrier(fifo);
	}
	fifo->state.last_output_present_nsec = timespec_to_nsec(&event->when);
}

static void fifo_handle_commit(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, surface_commit);
	// set the barrier condition
	if (fifo->state.current.set_barrier) {
		fifo->state.barrier_set = true;
	}
}

static bool should_queue_commit(struct wlr_fifo_v1 *fifo) {
	return fifo->state.pending.wait_barrier && fifo->state.barrier_set;
}

static void fifo_handle_client_commit(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, surface_client_commit);

	if (!fifo || !fifo->surface || !is_surface_buffer_valid(fifo->surface)) {
		return;
	}

	if (should_queue_commit(fifo)) {
		struct fifo_commit *commit = calloc(1, sizeof(*commit));
		if (!commit) {
			wl_client_post_no_memory(wl_resource_get_client(fifo->resource));
			return;
		}
		commit->fifo = fifo;

		// If the commit, in addition to a .wait_barrier request, has a .set_barrier one,
		// mark it so that we can set again the barrier when dequeing the commit.
		if (fifo->state.pending.set_barrier) {
			commit->barrier_pending = true;
		}
		commit->seq = wlr_surface_lock_pending(fifo->surface);
		wl_list_insert(fifo->commits.prev, &commit->link);
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
	if (!fifo->surface) {
		wl_resource_post_error(resource,
			WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
			"the associated surface no longer exists");
		return;
	}
	fifo->state.pending.wait_barrier = true;
}

static void fifo_handle_set_barrier(struct wl_client *client,
		struct wl_resource *resource) {
	struct wlr_fifo_v1 *fifo =
		wlr_fifo_v1_from_resource(resource);
	if (!fifo->surface) {
		wl_resource_post_error(resource,
			WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
			"the associated surface no longer exists");
		return;
	}
	fifo->state.pending.set_barrier = true;
}

static void fifo_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_fifo_v1 *fifo = wlr_fifo_v1_from_resource(resource);
	wlr_surface_synced_finish(&fifo->synced);
	wl_signal_emit_mutable(&fifo->events.destroy, fifo);
	fifo_reset(fifo);
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

static const struct wp_fifo_manager_v1_interface fifo_manager_impl;
static struct wlr_fifo_manager_v1 *wlr_fifo_manager_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_fifo_manager_v1_interface,
		&fifo_manager_impl));
	return wl_resource_get_user_data(resource);
}

static void fifo_manager_handle_get_fifo(struct wl_client *wl_client, struct wl_resource *manager_resource,
		uint32_t id, struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (wlr_addon_find(&surface->addons, NULL, &surface_fifo_addon_impl) != NULL) {
		wl_resource_post_error(manager_resource,
			WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
			"A wp_fifo_v1 object already exists for this surface");
		return;
	}

	struct wlr_fifo_v1 *fifo = calloc(1, sizeof(*fifo));
	if (!fifo) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	fifo->surface = surface;
	struct wlr_fifo_manager_v1 *fifo_manager =
		wlr_fifo_manager_v1_from_resource(manager_resource);
	fifo->manager = fifo_manager;
	fifo_manager->display = wl_client_get_display(wl_client);

	if (!wlr_surface_synced_init(&fifo->synced, surface,
			&surface_synced_impl, &fifo->state.pending, &fifo->state.current)) {
		free(fifo);
		wl_client_post_no_memory(wl_client);
		return;
	}

	fifo->resource = wl_resource_create(wl_client, &wp_fifo_v1_interface,
		wl_resource_get_version(manager_resource), id);
	if (fifo->resource == NULL) {
		wlr_surface_synced_finish(&fifo->synced);
		free(fifo);
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(fifo->resource, &fifo_implementation, fifo,
		fifo_handle_resource_destroy);

	wl_list_init(&fifo->commits);
	wl_signal_init(&fifo->events.destroy);

	fifo->surface_client_commit.notify = fifo_handle_client_commit;
	wl_signal_add(&surface->events.client_commit, &fifo->surface_client_commit);
	fifo->surface_commit.notify = fifo_handle_commit;
	wl_signal_add(&surface->events.commit, &fifo->surface_commit);

	wlr_addon_init(&fifo->addon, &surface->addons, NULL, &surface_fifo_addon_impl);

	// It is possible that at this time we have no outputs assigned to the surface yet.
	struct wlr_surface_output *surface_output = NULL;
	if (!wl_list_empty(&surface->current_outputs)) {
		wl_list_for_each(surface_output, &surface->current_outputs, link) {
			break;
		}
	}
	if (!wlr_fifo_v1_set_output(fifo, surface_output ? surface_output->output : NULL)) {
		wl_resource_destroy(fifo->resource);
		return;
	}

	wl_signal_emit_mutable(&fifo->manager->events.new_fifo,
		&(struct wlr_fifo_manager_v1_new_fifo_event){.fifo = fifo});
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
	wl_signal_init(&fifo_manager->events.new_fifo);

	fifo_manager->display_destroy.notify = fifo_manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &fifo_manager->display_destroy);

	return fifo_manager;
}

bool wlr_fifo_v1_set_output(struct wlr_fifo_v1 *fifo, struct wlr_output *output) {
	if (fifo->output) {
		fifo_reset(fifo);
	}

	if (output) {
		// If the surface is occluded, and there is no other surface updating the output's contents,
		// then we won't receive any output events. For this case, we introduce a timer ticking at
		// a heuristically defined value (40hz) so that we can advance the queue.
		fifo->surface_occluded_source =
			wl_event_loop_add_timer(wl_display_get_event_loop(fifo->manager->display),
				handle_timer, fifo);
		if (!fifo->surface_occluded_source) {
			return false;
		}
		int refresh_msec = 25;
		wl_event_source_timer_update(fifo->surface_occluded_source, refresh_msec);

		fifo->output = output;
		fifo->output_present.notify = fifo_handle_output_present;
		wl_signal_add(&fifo->output->events.present, &fifo->output_present);
		fifo->output_destroy.notify = fifo_handle_output_destroy;
		wl_signal_add(&fifo->output->events.destroy, &fifo->output_destroy);
	}

	return true;
}
