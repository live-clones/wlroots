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

static bool commit_on_valid_buffer(const struct wlr_surface * const surface) {
	if (!surface->buffer || (surface->pending.committed & WLR_SURFACE_STATE_BUFFER &&
			surface->pending.buffer == NULL)) {
		return false;
	}

	return true;
}

static void fifo_signal_barrier(struct wlr_fifo_v1 *fifo) {
	bool barrier_pending = false;
	struct fifo_commit *commit, *tmp;
	// dequeue and unlock commits until we find one with a .set_barrier request,
	// in which case leave the barrier condition set.
	wl_list_for_each_safe(commit, tmp, &fifo->commits, link) {
		wl_list_remove(&commit->link);

		wlr_surface_unlock_cached(fifo->surface, commit->seq);

		if (commit->barrier_pending) {
			free(commit);
			barrier_pending = true;
			break;
		}
		free(commit);
	}

	if (!barrier_pending) {
		fifo->barrier_set = false;
	}
}

static void fifo_reset(struct wlr_fifo_v1 *fifo) {
	struct fifo_commit *commit, *tmp_co;
	wl_list_for_each_safe(commit, tmp_co, &fifo->commits, link) {
		if (commit->seq) {
			wlr_surface_unlock_cached(fifo->surface, commit->seq);
		}
		wl_list_remove(&commit->link);
		free(commit);
	}
	if (fifo->output) {
		fifo->output_commit.notify = NULL;
		wl_list_remove(&fifo->output_commit.link);
		fifo->output_destroy.notify = NULL;
		wl_list_remove(&fifo->output_destroy.link);
	}
	fifo->barrier_set = false;
	fifo->current = fifo->pending = (struct wlr_fifo_v1_fifo_state){0};
	fifo->last_output_commit_msec = 0;
}

static void fifo_handle_output_destroy(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, output_destroy);
	fifo_reset(fifo);
	fifo->output = NULL;
}

static int handle_timer(void *data) {
	struct wlr_fifo_v1 *fifo = data;
	// set the timer to the frequency of the output, or to 40hz if we have no output
	int64_t refresh_msec = (fifo->output && fifo->output->refresh) ?
		1000000/fifo->output->refresh :
		25;
	int64_t now = get_current_time_msec();
	// this condition means that the surface is in some way occluded, so
	// we advance the queue at the rate of the timer
	if (fifo->barrier_set && now - fifo->last_output_commit_msec > refresh_msec) {
		fifo_signal_barrier(fifo);
	}
	wl_event_source_timer_update(fifo->surface_occluded_timer, refresh_msec);
	return 0;
}

static void fifo_handle_output_commit(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, output_commit);
	if (!fifo->surface->buffer) {
	    return;
	}

	// The output.commit event is sent right after the last output commit has been applied, so any
	// surface commit will apply for the next refresh cycle. In this case we can signal the barrier
	// to advance the queue.
	if (fifo->barrier_set) {
		fifo_signal_barrier(fifo);
	}
	fifo->last_output_commit_msec = get_current_time_msec();
}

static void fifo_handle_commit(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, surface_commit);
	// set the barrier condition
	if (fifo->current.set_barrier) {
		fifo->barrier_set = true;
	}
}

static bool fifo_do_queue_commit(struct wlr_fifo_v1 *fifo) {
	// Queue the commit if it has a .wait_barrier request, and the barrier is currently set.
	return fifo->pending.wait_barrier && fifo->barrier_set;
}

static void fifo_handle_client_commit(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo =
		wl_container_of(listener, fifo, surface_client_commit);

	if (!commit_on_valid_buffer(fifo->surface)) {
		return;
	}

	if (fifo_do_queue_commit(fifo)) {
		struct fifo_commit *commit = calloc(1, sizeof(*commit));
		if (!commit) {
			wl_client_post_no_memory(wl_resource_get_client(fifo->resource));
			return;
		}

		// If the commit, in addition to a .wait_barrier request, has a .set_barrier one,
		// mark it, that we can re-set the barrier when dequeing the commit.
		if (fifo->pending.set_barrier) {
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
	fifo->pending.wait_barrier = true;
}

static void fifo_handle_set_barrier(struct wl_client *client,
		struct wl_resource *resource) {
	struct wlr_fifo_v1 *fifo =
		wlr_fifo_v1_from_resource(resource);
	fifo->pending.set_barrier = true;
}

static void fifo_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_fifo_v1 *fifo = wlr_fifo_v1_from_resource(resource);
	fifo_reset(fifo);
	wlr_addon_finish(&fifo->addon);
	wlr_surface_synced_finish(&fifo->synced);
	wl_list_remove(&fifo->surface_client_commit.link);
	wl_list_remove(&fifo->surface_commit.link);
	wl_signal_emit_mutable(&fifo->events.destroy, fifo);
	if (fifo->surface_occluded_timer) {
		wl_event_source_remove(fifo->surface_occluded_timer);
	}
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

	wl_signal_init(&fifo->events.destroy);

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
			WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
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

	if (!wlr_surface_synced_init(&fifo->synced, surface,
			&surface_synced_impl, &fifo->pending, &fifo->current)) {
		free(fifo);
		wl_client_post_no_memory(wl_client);
		return;
	}

	struct wlr_fifo_manager_v1 *fifo_manager =
		wlr_fifo_manager_v1_from_resource(resource);
	fifo->fifo_manager = fifo_manager;
	fifo_manager->display = wl_client_get_display(wl_client);

	// It is possible that at this time we have no outputs assigned to the surface yet.
	struct wlr_surface_output *surface_output = NULL;
	if (!wl_list_empty(&surface->current_outputs)) {
		wl_list_for_each(surface_output, &surface->current_outputs, link) {
			break;
		}
	}
	wlr_fifo_v1_set_output(fifo, surface_output ? surface_output->output : NULL);

	// we constantly run a timer to advance the queue for the cases where the surface is occluded
	fifo->surface_occluded_timer =
		wl_event_loop_add_timer(wl_display_get_event_loop(fifo_manager->display),
			handle_timer, fifo);
	if (!fifo->surface_occluded_timer) {
		return;
	}
	// set the timer to the frequency of the output, or to 40hz if we have no output
	int64_t refresh_msec = (fifo->output && fifo->output->refresh) ?
		1000000/fifo->output->refresh :
		25;
	wl_event_source_timer_update(fifo->surface_occluded_timer, refresh_msec);

	wl_signal_emit_mutable(&fifo->fifo_manager->events.new_fifo,
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

void wlr_fifo_v1_set_output(struct wlr_fifo_v1 *fifo, struct wlr_output *output) {
	// reset fifo state
	fifo_reset(fifo);

	// handle new output
	if (output) {
		fifo->output = output;
		fifo->output_commit.notify = fifo_handle_output_commit;
		wl_signal_add(&fifo->output->events.commit, &fifo->output_commit);
		fifo->output_destroy.notify = fifo_handle_output_destroy;
		wl_signal_add(&fifo->output->events.destroy, &fifo->output_destroy);
	}
}
