#include <assert.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wlr/types/wlr_commit_timing_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include "commit-timing-v1-protocol.h"
#include "util/time.h"

#define TIMING_MANAGER_VERSION 1

static void commit_destroy(struct wlr_commit_timer_v1_commit *commit) {
	wl_event_source_remove(commit->unlock_source);
	// the remove from the list must happen before unlocking the commit, since the commit might end
	// up calling wlr_commit_timer_v1_set_output(), which traverses this list.
	wl_list_remove(&commit->link);
	wlr_surface_unlock_cached(commit->timer->surface, commit->pending_seq);
	free(commit);
}

static int handle_commit_timerfd(int fd, uint32_t mask, void *data) {
	struct wlr_commit_timer_v1_commit *commit = data;
	commit_destroy(commit);
	close(fd);
	return 0;
}

static void timer_handle_output_present(struct wl_listener *listener, void *data) {
	struct wlr_commit_timer_v1 *timer =
		wl_container_of(listener, timer, output_present);
	struct wlr_output_event_present *event = data;

	// we constantly check if the refresh rate has changed, to reset accordingly
	if (timer->state.refresh != event->output->refresh) {
		wlr_commit_timer_v1_set_output(timer, event->output);
	}

	// we need to have just one presentation time so that, together with the refresh rate, we
	// can know the refresh cycle offset for future presentations.
	if (event->presented && !timer->state.base_present_nsec) {
		timer->state.base_present_nsec = timespec_to_nsec(&event->when);
	}
}

static bool target_is_in_past(uint64_t target_nsec) {
	return target_nsec < (uint64_t)get_current_time_nsec();
}

static int mhz_to_nsec(int mhz) {
	assert(mhz != 0);
	return 1000000000000LL / mhz;
}

static uint64_t timer_get_target_nsec(const struct wlr_commit_timer_v1 *timer) {
	struct wlr_output *output = timer->output;
	uint64_t target_nsec = timer->state.timestamp_nsec;

	// ignore the request if the output is not stable yet, or the timestamp is invalid
	if (!output || !timer->state.base_present_nsec || !target_nsec ||
			target_is_in_past(target_nsec)) {
		target_nsec = 0;
		goto out;
	}
	// if output has no refresh rate, use requested timestamp as is
	if (!output->refresh) {
		goto out;
	}

	uint64_t refresh_nsec = mhz_to_nsec(output->refresh);
	uint64_t cycle_phase_nsec = timer->state.base_present_nsec % refresh_nsec;

	uint64_t round_to_nearest_refresh_nsec = target_nsec;
	round_to_nearest_refresh_nsec -= cycle_phase_nsec;
	round_to_nearest_refresh_nsec += refresh_nsec/2;
	round_to_nearest_refresh_nsec -= (round_to_nearest_refresh_nsec % refresh_nsec);
	round_to_nearest_refresh_nsec += cycle_phase_nsec;

	target_nsec = round_to_nearest_refresh_nsec;

	if (timer->state.refresh) {
		// Subtract 1 refresh cycle to the target time.
		// This guarantees that the surface commit is unlocked before the
		// compositor receives the .frame event for the refresh cycle we want to target.
		target_nsec -= mhz_to_nsec(timer->state.refresh);
	}

	target_nsec -= 500000; // subtract a 500us slop so that we don't miss the target

	// check if adjusted target time is in the past
	if (target_is_in_past(target_nsec)) {
		target_nsec = 0;
	}
out:
	return target_nsec;
}

static void timer_handle_client_commit(struct wl_listener *listener, void *data) {
	struct wlr_commit_timer_v1 *timer =
		wl_container_of(listener, timer, client_commit);
	struct wlr_commit_timer_v1_commit *commit = NULL;

	uint64_t target_nsec = timer_get_target_nsec(timer);
	timer->state.timestamp_nsec = 0; // reset the timestamp
	// if the target time is invalid, or if it's too close to the current time (<1ms),
	// don't bother.
	if (!target_nsec || target_nsec - get_current_time_nsec() < 1000000) {
		goto out;
	}

	commit = calloc(1, sizeof(*commit));
	if (!commit) {
		wl_client_post_no_memory(wl_resource_get_client(timer->resource));
		goto out;
	}
	commit->timer = timer;

	int timerfd_unlock_commit_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
	struct itimerspec itspec = { .it_interval = 0 };
	timespec_from_nsec(&itspec.it_value, target_nsec);
	timerfd_settime(timerfd_unlock_commit_fd, TFD_TIMER_ABSTIME, &itspec, NULL);
	commit->unlock_source =
		wl_event_loop_add_fd(wl_display_get_event_loop(timer->wl_display),
			timerfd_unlock_commit_fd, WL_EVENT_READABLE, handle_commit_timerfd, commit);
	if (!commit->unlock_source) {
		wl_client_post_no_memory(wl_resource_get_client(timer->resource));
		close(timerfd_unlock_commit_fd);
		goto out;
	}

	commit->pending_seq = wlr_surface_lock_pending(timer->surface);

	wl_list_insert(&timer->commits, &commit->link);

	return;
out:
	free(commit);
}

static const struct wp_commit_timer_v1_interface timer_impl;

static struct wlr_commit_timer_v1 *wlr_commit_timer_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_commit_timer_v1_interface,
		&timer_impl));
	return wl_resource_get_user_data(resource);
}

static void timer_handle_set_timestamp(struct wl_client *client,
		struct wl_resource *resource, uint32_t tv_sec_hi, uint32_t tv_sec_lo,
		uint32_t tv_nsec) {
	struct wlr_commit_timer_v1 *timer = wlr_commit_timer_v1_from_resource(resource);

	if (timer->state.timestamp_nsec) {
		wl_resource_post_error(resource,
			WP_COMMIT_TIMER_V1_ERROR_TIMESTAMP_EXISTS,
			"surface already has a timestamp");
		return;
	}

	uint64_t tv_sec = (uint64_t)tv_sec_hi<<32 | tv_sec_lo;
	// check for overflow
	if (tv_nsec >= NSEC_PER_SEC ||
			tv_sec > (UINT64_MAX - tv_nsec) / NSEC_PER_SEC) {
		wl_resource_post_error(resource,
			WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP,
			"invalid timestamp");
		return;
	}

	timer->state.timestamp_nsec = tv_sec * NSEC_PER_SEC + tv_nsec;
}

static void timer_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_commit_timer_v1_interface timer_impl = {
	.destroy = timer_handle_destroy,
	.set_timestamp = timer_handle_set_timestamp
};

static void surface_addon_destroy(struct wlr_addon *addon) {
    struct wlr_commit_timer_v1 *timer = wl_container_of(addon, timer, addon);
    wl_resource_destroy(timer->resource);
}

static const struct wlr_addon_interface surface_addon_impl = {
    .name = "wp_commit_timer_v1",
    .destroy = surface_addon_destroy,
};

static void timer_reset(struct wlr_commit_timer_v1 *timer) {
	struct wlr_commit_timer_v1_commit *commit, *tmp_co;
	wl_list_for_each_safe(commit, tmp_co, &timer->commits, link) {
		commit_destroy(commit);
	}
	if (timer->output) {
		timer->output_present.notify = NULL;
		wl_list_remove(&timer->output_present.link);
		timer->output_destroy.notify = NULL;
		wl_list_remove(&timer->output_destroy.link);
		timer->output = NULL;
	}
	memset(&timer->state, 0, sizeof(timer->state));
}

static void timer_handle_output_destroy(struct wl_listener *listener, void *data) {
	struct wlr_commit_timer_v1 *timer =
		wl_container_of(listener, timer, output_destroy);
	timer_reset(timer);
}

static void timer_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_commit_timer_v1 *timer = wlr_commit_timer_v1_from_resource(resource);
	timer_reset(timer);
	wlr_addon_finish(&timer->addon);
	wl_list_remove(&timer->client_commit.link);
	wl_signal_emit_mutable(&timer->events.destroy, timer);
	free(timer);
}

static struct wlr_commit_timer_v1 *commit_timer_create(struct wl_client *wl_client, uint32_t version,
		uint32_t id, struct wlr_surface *surface) {
	struct wlr_commit_timer_v1 *timer = calloc(1, sizeof(*timer));
	if (timer == NULL) {
		goto err_alloc;
	}

	timer->resource = wl_resource_create(wl_client, &wp_commit_timer_v1_interface, version, id);
	if (timer->resource == NULL) {
		goto err_alloc;
	}
	wl_resource_set_implementation(timer->resource, &timer_impl, timer,
		timer_handle_resource_destroy);

	wl_list_init(&timer->commits);

	/* we will use the wl_display to add a timer to the wl_event_loop */
	timer->wl_display = wl_client_get_display(wl_client);

	timer->surface = surface;
	timer->client_commit.notify = timer_handle_client_commit;
	wl_signal_add(&timer->surface->events.client_commit, &timer->client_commit);

	wlr_log(WLR_DEBUG, "New wlr_commit_timer_v1 %p (res %p)", timer, timer->resource);

	return timer;

err_alloc:
	free(timer);
	return NULL;
}

static const struct wp_commit_timing_manager_v1_interface timing_manager_impl;
static struct wlr_commit_timing_manager_v1 *wlr_commit_timing_manager_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_commit_timing_manager_v1_interface,
		&timing_manager_impl));
	return wl_resource_get_user_data(resource);
}

static void timing_manager_handle_get_timer(struct wl_client *wl_client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (wlr_addon_find(&surface->addons, NULL, &surface_addon_impl) != NULL) {
		wl_resource_post_error(resource,
			WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS,
			"A wp_commit_timer_v1 object already exists for this surface");
		return;
	}

	struct wlr_commit_timer_v1 *timer =
		commit_timer_create(wl_client, wl_resource_get_version(resource), id, surface);
	if (!timer) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wlr_addon_init(&timer->addon, &surface->addons, NULL, &surface_addon_impl);
	wl_signal_init(&timer->events.destroy);

	struct wlr_commit_timing_manager_v1 *manager =
		wlr_commit_timing_manager_v1_from_resource(resource);

	timer->timing_manager = manager;

	// it is possible that at this time we have no outputs assigned to the surface yet
	struct wlr_surface_output *surface_output = NULL;
	if (!wl_list_empty(&surface->current_outputs)) {
		wl_list_for_each(surface_output, &surface->current_outputs, link) {
			break;
		}
	}
	wlr_commit_timer_v1_set_output(timer, surface_output ? surface_output->output : NULL);

	wl_signal_emit_mutable(&timer->timing_manager->events.new_timer,
		&(struct wlr_commit_timing_manager_v1_new_timer_event){.timer = timer});
}

static void timing_manager_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_commit_timing_manager_v1_interface timing_manager_impl = {
	.get_timer = timing_manager_handle_get_timer,
	.destroy = timing_manager_handle_destroy,
};

static void timing_manager_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wp_commit_timing_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct wlr_commit_timing_manager_v1 *manager = data;
	wl_resource_set_implementation(resource, &timing_manager_impl, manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_commit_timing_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_signal_emit_mutable(&manager->events.destroy, manager);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_commit_timing_manager_v1 *wlr_commit_timing_manager_v1_create(struct wl_display *display,
		uint32_t version) {
	assert(version <= TIMING_MANAGER_VERSION);

	struct wlr_commit_timing_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		goto err_out;
	}

	manager->global = wl_global_create(display, &wp_commit_timing_manager_v1_interface,
		version, manager, timing_manager_bind);
	if (!manager->global) {
		goto err_out;
	}

	wl_signal_init(&manager->events.new_timer);
	wl_signal_init(&manager->events.destroy);

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;

err_out:
	free(manager);
	return NULL;
}

void wlr_commit_timer_v1_set_output(struct wlr_commit_timer_v1 *timer,
		struct wlr_output *output) {
	timer_reset(timer);

	if (!output) {
		return;
	}

	timer->output = output;
	// we make a copy of the refresh rate so that we can check for whenever it changes
	timer->state.refresh = output->refresh;

	timer->output_present.notify = timer_handle_output_present;
	wl_signal_add(&output->events.present, &timer->output_present);
	timer->output_destroy.notify = timer_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &timer->output_destroy);
}
