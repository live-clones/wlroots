#include <assert.h>
#include <backend/headless.h>
#include <stdlib.h>
#include <time.h>
#include <types/wlr_output.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend/wayland.h>
#include <wlr/interfaces/wlr_frame_scheduler.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_frame_scheduler.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

void wlr_frame_scheduler_schedule_frame(struct wlr_frame_scheduler *scheduler) {
	scheduler->needs_frame = true;
	scheduler->impl->schedule_frame(scheduler);
}

void wlr_frame_scheduler_destroy(struct wlr_frame_scheduler *scheduler) {
	if (scheduler == NULL) {
		return;
	}
	wl_list_remove(&scheduler->backend_needs_frame.link);
	scheduler->impl->destroy(scheduler);
}

void wlr_frame_scheduler_emit_frame(struct wlr_frame_scheduler *scheduler) {
	if (!scheduler->needs_frame) {
		return;
	}
	scheduler->needs_frame = false;
	wl_signal_emit_mutable(&scheduler->events.frame, NULL);
}

static void frame_scheduler_handle_needs_frame(struct wl_listener *listener, void *data) {
	struct wlr_frame_scheduler *scheduler = wl_container_of(listener, scheduler, backend_needs_frame);
	wlr_frame_scheduler_schedule_frame(scheduler);
}

void wlr_frame_scheduler_init(struct wlr_frame_scheduler *scheduler,
		const struct wlr_frame_scheduler_impl *impl, struct wlr_output *output) {
	assert(impl->schedule_frame);
	assert(impl->destroy);

	*scheduler = (struct wlr_frame_scheduler){
		.impl = impl,
		.output = output,
	};
	wl_signal_init(&scheduler->events.frame);
	scheduler->backend_needs_frame.notify = frame_scheduler_handle_needs_frame;
	wl_signal_add(&output->events.needs_frame, &scheduler->backend_needs_frame);
}

// This struct and its methods are a common base for frame schedulers that restart their render loop
// via an idle event source, which fires "soon", instead of using a more complex schedule. Deferring
// the frame to an idle event is a crude way of ensuring that work done after scheduling the frame
// gets picked up by the renderer, rather than rendering happening inside the schedule call and
// missing out on any immediately following updates.
struct idle_frame_scheduler {
	struct wlr_frame_scheduler base;
	struct wl_event_source *idle;
	// Whether the render loop is already awake, i.e. whether frames from idle events should be
	// inhibited.
	bool frame_pending;
};

static void idle_frame_scheduler_emit_frame(struct idle_frame_scheduler *scheduler) {
	scheduler->frame_pending = false;
	wlr_frame_scheduler_emit_frame(&scheduler->base);
}

static void idle_frame_scheduler_handle_idle(void *data) {
	struct idle_frame_scheduler *scheduler = data;
	if (!scheduler->frame_pending) {
		idle_frame_scheduler_emit_frame(scheduler);
	}
	scheduler->idle = NULL;
}

static void idle_frame_scheduler_schedule_frame(struct wlr_frame_scheduler *wlr_scheduler) {
	struct idle_frame_scheduler *scheduler = wl_container_of(wlr_scheduler, scheduler, base);
	if (scheduler->idle != NULL || scheduler->frame_pending) {
		// Either we are already set up to restart the render loop or it is already running.
		return;
	}

	struct wl_event_loop *loop = scheduler->base.output->event_loop;
	scheduler->idle = wl_event_loop_add_idle(loop, idle_frame_scheduler_handle_idle, scheduler);
}

static void idle_frame_scheduler_set_frame_pending(struct idle_frame_scheduler *scheduler) {
	scheduler->frame_pending = true;
	if (scheduler->idle) {
		wl_event_source_remove(scheduler->idle);
		scheduler->idle = NULL;
	}
}

static void idle_frame_scheduler_finish(struct idle_frame_scheduler *scheduler) {
	if (scheduler->idle) {
		wl_event_source_remove(scheduler->idle);
	}
}

// The present scheduler builds on the idle_frame_scheduler's logic for restarting the render loop,
// and drives the render loop using `wlr_output.events.present`.
struct present_scheduler {
	struct idle_frame_scheduler base;
	struct wl_listener commit;
	struct wl_listener present;
};

static void present_scheduler_destroy(struct wlr_frame_scheduler *wlr_scheduler) {
	struct present_scheduler *scheduler = wl_container_of(wlr_scheduler, scheduler, base.base);
	idle_frame_scheduler_finish(&scheduler->base);
	wl_list_remove(&scheduler->commit.link);
	wl_list_remove(&scheduler->present.link);
	free(scheduler);
}

static void present_scheduler_handle_commit(struct wl_listener *listener, void *data) {
	struct present_scheduler *scheduler = wl_container_of(listener, scheduler, commit);
	if (scheduler->base.base.output->enabled) {
		idle_frame_scheduler_set_frame_pending(&scheduler->base);
	}
}

static void present_scheduler_handle_present(struct wl_listener *listener, void *data) {
	struct present_scheduler *scheduler = wl_container_of(listener, scheduler, present);
	struct wlr_output_event_present *present = data;
	if (present->presented) {
		idle_frame_scheduler_emit_frame(&scheduler->base);
	} else {
		scheduler->base.frame_pending = false;
	}
}

static const struct wlr_frame_scheduler_impl present_scheduler_impl = {
	.schedule_frame = idle_frame_scheduler_schedule_frame,
	.destroy = present_scheduler_destroy,
};

struct wlr_frame_scheduler *wlr_present_scheduler_create(struct wlr_output *output) {
	struct present_scheduler *scheduler = calloc(1, sizeof(*scheduler));
	if (!scheduler) {
		return NULL;
	}
	wlr_frame_scheduler_init(&scheduler->base.base, &present_scheduler_impl, output);
	scheduler->commit.notify = present_scheduler_handle_commit;
	wl_signal_add(&output->events.commit, &scheduler->commit);
	scheduler->present.notify = present_scheduler_handle_present;
	wl_signal_add(&output->events.present, &scheduler->present);
	return &scheduler->base.base;
}

// This scheduler builds on idle_frame_scheduler and uses Wayland's frame callbacks for driving the
// render loop.
struct wl_scheduler {
	struct idle_frame_scheduler base;
	struct wl_callback *frame;
	struct wl_listener precommit;
};

static void wl_scheduler_destroy(struct wlr_frame_scheduler *wlr_scheduler) {
	struct wl_scheduler *scheduler = wl_container_of(wlr_scheduler, scheduler, base.base);
	idle_frame_scheduler_finish(&scheduler->base);
	wl_list_remove(&scheduler->precommit.link);
	wl_callback_destroy(scheduler->frame);
	free(scheduler);
}

static void wl_scheduler_handle_frame(void *data, struct wl_callback *cb, uint32_t time) {
	struct wl_scheduler *scheduler = data;
	assert(scheduler->frame == cb);
	idle_frame_scheduler_emit_frame(&scheduler->base);
}

static const struct wl_callback_listener wl_scheduler_frame_listener = {
	.done = wl_scheduler_handle_frame,
};

static void wl_scheduler_handle_precommit(struct wl_listener *listener, void *data) {
	struct wl_scheduler *scheduler = wl_container_of(listener, scheduler, precommit);
	struct wlr_output_event_precommit *precommit = data;
	if (!output_pending_enabled(precommit->output, precommit->state)) {
		return;
	}
	idle_frame_scheduler_set_frame_pending(&scheduler->base);
	if (scheduler->frame != NULL) {
		wl_callback_destroy(scheduler->frame);
	}
	struct wl_surface *surface = wlr_wl_output_get_surface(scheduler->base.base.output);
	scheduler->frame = wl_surface_frame(surface);
	wl_callback_add_listener(scheduler->frame, &wl_scheduler_frame_listener, scheduler);
}

static const struct wlr_frame_scheduler_impl wl_scheduler_impl = {
	.schedule_frame = idle_frame_scheduler_schedule_frame,
	.destroy = wl_scheduler_destroy,
};

static struct wlr_frame_scheduler *wl_scheduler_create(struct wlr_output *output) {
	if (!wlr_output_is_wl(output)) {
		return NULL;
	}

	struct wl_scheduler *scheduler = calloc(1, sizeof(*scheduler));
	if (!scheduler) {
		return NULL;
	}
	wlr_frame_scheduler_init(&scheduler->base.base, &wl_scheduler_impl, output);
	scheduler->precommit.notify = wl_scheduler_handle_precommit;
	wl_signal_add(&output->events.precommit, &scheduler->precommit);

	return &scheduler->base.base;
}

#define DEFAULT_REFRESH (60 * 1000) // 60 Hz in mHz

struct interval_scheduler {
	struct idle_frame_scheduler base;
	struct wl_event_source *frame_timer;
	struct wl_listener commit;

	int32_t frame_delay;
};

static void interval_scheduler_destroy(struct wlr_frame_scheduler *wlr_scheduler) {
	struct interval_scheduler *scheduler = wl_container_of(wlr_scheduler, scheduler, base.base);
	idle_frame_scheduler_finish(&scheduler->base);
	wl_event_source_remove(scheduler->frame_timer);
	wl_list_remove(&scheduler->commit.link);
	free(scheduler);
}

static void interval_scheduler_handle_commit(struct wl_listener *listener, void *data) {
	struct interval_scheduler *scheduler = wl_container_of(listener, scheduler, commit);
	struct wlr_output_event_commit *commit = data;
	struct wlr_output *output = commit->output;
	if (commit->state->committed & WLR_OUTPUT_STATE_MODE) {
		int32_t refresh = output->refresh ? output->refresh : DEFAULT_REFRESH;
		scheduler->frame_delay = 1000 * 1000 / refresh;
	}

	if (output->enabled) {
		assert(scheduler->frame_delay != 0);
		wl_event_source_timer_update(scheduler->frame_timer, scheduler->frame_delay);
		idle_frame_scheduler_set_frame_pending(&scheduler->base);
	}
}

static int interval_scheduler_handle_timer(void *data) {
	struct interval_scheduler *scheduler = data;
	idle_frame_scheduler_emit_frame(&scheduler->base);
	return 0;
}

static const struct wlr_frame_scheduler_impl interval_scheduler_impl = {
	.schedule_frame = idle_frame_scheduler_schedule_frame,
	.destroy = interval_scheduler_destroy,
};

struct wlr_frame_scheduler *wlr_interval_scheduler_create(struct wlr_output *output) {
	if (!wlr_output_is_headless(output)) {
		return NULL;
	}

	struct interval_scheduler *scheduler = calloc(1, sizeof(*scheduler));
	if (!scheduler) {
		return NULL;
	}
	wlr_frame_scheduler_init(&scheduler->base.base, &interval_scheduler_impl, output);

	scheduler->frame_delay = 1000 * 1000 / DEFAULT_REFRESH;

	scheduler->frame_timer = wl_event_loop_add_timer(output->event_loop,
		interval_scheduler_handle_timer, scheduler);

	scheduler->commit.notify = interval_scheduler_handle_commit;
	wl_signal_add(&output->events.commit, &scheduler->commit);

	return &scheduler->base.base;
}

// The predictive scheduler schedules frame signals based on historic render times, aiming to
// start rendering as late as possible while still meeting the next vblank deadline.
struct predictive_scheduler {
	struct wlr_frame_scheduler base;
	struct wl_event_source *idle;
	struct wl_event_source *timer;
	struct wl_listener commit;
	struct wl_listener present;

	bool frame_pending;

	int64_t estimated_frame_time_ns;
	bool has_estimate;

	int64_t pending_pre_render_ns;
	struct wlr_render_timer *pending_render_timer;
	bool has_pending_feedback;
};

static const struct wlr_frame_scheduler_impl predictive_scheduler_impl;

static int64_t compute_margin_ns(int refresh_ns) {
	int64_t margin = refresh_ns / 20;
	if (margin < 1500000) {
		margin = 1500000;
	}
	return margin;
}

static void update_estimate(struct predictive_scheduler *scheduler, int64_t sample_ns) {
	if (!scheduler->has_estimate) {
		scheduler->estimated_frame_time_ns = sample_ns;
		scheduler->has_estimate = true;
		return;
	}

	if (sample_ns > scheduler->estimated_frame_time_ns) {
		// React quickly to spikes
		scheduler->estimated_frame_time_ns =
			(9 * sample_ns + scheduler->estimated_frame_time_ns) / 10;
	} else {
		// Decay slowly
		scheduler->estimated_frame_time_ns =
			(sample_ns + 9 * scheduler->estimated_frame_time_ns) / 10;
	}
}

static void predictive_scheduler_consume_feedback(struct predictive_scheduler *scheduler) {
	if (!scheduler->has_pending_feedback) {
		return;
	}
	scheduler->has_pending_feedback = false;

	int64_t gpu_ns = 0;
	if (scheduler->pending_render_timer != NULL) {
		int duration = wlr_render_timer_get_duration_ns(scheduler->pending_render_timer);
		if (duration > 0) {
			gpu_ns = duration;
		}
	}

	update_estimate(scheduler, scheduler->pending_pre_render_ns + gpu_ns);
}

static void predictive_scheduler_handle_idle(void *data) {
	struct predictive_scheduler *scheduler = data;
	scheduler->idle = NULL;
	scheduler->frame_pending = false;
	wlr_frame_scheduler_emit_frame(&scheduler->base);
}

static int predictive_scheduler_handle_timer(void *data) {
	struct predictive_scheduler *scheduler = data;
	scheduler->frame_pending = false;
	wlr_frame_scheduler_emit_frame(&scheduler->base);
	return 0;
}

static void predictive_scheduler_set_timer(struct predictive_scheduler *scheduler, int ms) {
	if (scheduler->idle != NULL) {
		wl_event_source_remove(scheduler->idle);
		scheduler->idle = NULL;
	}
	wl_event_source_timer_update(scheduler->timer, ms);
}

static void predictive_scheduler_set_idle(struct predictive_scheduler *scheduler) {
	wl_event_source_timer_update(scheduler->timer, 0);
	if (!scheduler->idle) {
		scheduler->idle = wl_event_loop_add_idle(
			scheduler->base.output->event_loop,
			predictive_scheduler_handle_idle, scheduler);
	}
}

static void predictive_scheduler_cancel(struct predictive_scheduler *scheduler) {
	if (scheduler->idle != NULL) {
		wl_event_source_remove(scheduler->idle);
		scheduler->idle = NULL;
	}
	wl_event_source_timer_update(scheduler->timer, 0);
}

static void predictive_scheduler_handle_commit(struct wl_listener *listener, void *data) {
	struct predictive_scheduler *scheduler = wl_container_of(listener, scheduler, commit);
	if (scheduler->base.output->enabled) {
		scheduler->frame_pending = true;
		predictive_scheduler_cancel(scheduler);
	}
}

static void predictive_scheduler_handle_present(struct wl_listener *listener, void *data) {
	struct predictive_scheduler *scheduler = wl_container_of(listener, scheduler, present);
	struct wlr_output_event_present *event = data;

	if (!event->presented) {
		scheduler->frame_pending = false;
		return;
	}

	predictive_scheduler_consume_feedback(scheduler);

	if (!scheduler->has_estimate || event->refresh == 0) {
		predictive_scheduler_set_idle(scheduler);
		return;
	}

	int64_t refresh_ns = event->refresh;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	int64_t present_ns = (int64_t)event->when.tv_sec * 1000000000 + event->when.tv_nsec;
	int64_t now_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
	int64_t next_vblank_ns = present_ns + refresh_ns;

	int64_t margin = compute_margin_ns(refresh_ns);
	int64_t deadline_ns = next_vblank_ns - scheduler->estimated_frame_time_ns - margin;
	int64_t delay_ns = deadline_ns - now_ns;

	if (delay_ns < 1000000) {
		predictive_scheduler_set_idle(scheduler);
	} else {
		int delay_ms = (int)(delay_ns / 1000000);
		predictive_scheduler_set_timer(scheduler, delay_ms);
	}
}

static void predictive_scheduler_schedule_frame(struct wlr_frame_scheduler *wlr_scheduler) {
	struct predictive_scheduler *scheduler =
		wl_container_of(wlr_scheduler, scheduler, base);
	if (scheduler->idle != NULL || scheduler->frame_pending) {
		return;
	}
	predictive_scheduler_set_idle(scheduler);
}

static void predictive_scheduler_destroy(struct wlr_frame_scheduler *wlr_scheduler) {
	struct predictive_scheduler *scheduler =
		wl_container_of(wlr_scheduler, scheduler, base);
	if (scheduler->idle != NULL) {
		wl_event_source_remove(scheduler->idle);
	}
	wl_event_source_remove(scheduler->timer);
	wl_list_remove(&scheduler->commit.link);
	wl_list_remove(&scheduler->present.link);
	free(scheduler);
}

static const struct wlr_frame_scheduler_impl predictive_scheduler_impl = {
	.schedule_frame = predictive_scheduler_schedule_frame,
	.destroy = predictive_scheduler_destroy,
};

struct wlr_frame_scheduler *wlr_predictive_frame_scheduler_create(struct wlr_output *output) {
	struct predictive_scheduler *scheduler = calloc(1, sizeof(*scheduler));
	if (!scheduler) {
		return NULL;
	}
	wlr_frame_scheduler_init(&scheduler->base, &predictive_scheduler_impl, output);

	scheduler->timer = wl_event_loop_add_timer(output->event_loop,
		predictive_scheduler_handle_timer, scheduler);

	scheduler->commit.notify = predictive_scheduler_handle_commit;
	wl_signal_add(&output->events.commit, &scheduler->commit);
	scheduler->present.notify = predictive_scheduler_handle_present;
	wl_signal_add(&output->events.present, &scheduler->present);
	return &scheduler->base;
}

void wlr_frame_scheduler_inform_render(struct wlr_frame_scheduler *scheduler,
		int64_t pre_render_duration_ns, struct wlr_render_timer *render_timer) {
	if (scheduler->impl != &predictive_scheduler_impl) {
		return;
	}
	struct predictive_scheduler *predictive =
		wl_container_of(scheduler, predictive, base);

	predictive->pending_pre_render_ns = pre_render_duration_ns;
	predictive->pending_render_timer = render_timer;
	predictive->has_pending_feedback = true;
}

struct wlr_frame_scheduler *wlr_frame_scheduler_autocreate(struct wlr_output *output) {
	if (wlr_output_is_wl(output) && !wlr_wl_backend_has_presentation_time(output->backend)) {
		wlr_log(WLR_INFO, "wp_presentation not available, falling back to frame callbacks");
		return wl_scheduler_create(output);
	}

	if (wlr_output_is_headless(output)) {
		return wlr_interval_scheduler_create(output);
	}

	return wlr_present_scheduler_create(output);
}
