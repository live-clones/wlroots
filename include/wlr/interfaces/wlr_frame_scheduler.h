/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_INTERFACES_WLR_FRAME_SCHEDULER_H
#define WLR_INTERFACES_WLR_FRAME_SCHEDULER_H

struct wlr_frame_scheduler;
struct wlr_output;

struct wlr_frame_scheduler_impl {
	/**
	 * Ensure that the scheduler->frame signal will be fired in the future.
	 */
	void (*schedule_frame)(struct wlr_frame_scheduler *scheduler);
	void (*destroy)(struct wlr_frame_scheduler *scheduler);
};

void wlr_frame_scheduler_init(struct wlr_frame_scheduler *scheduler,
	const struct wlr_frame_scheduler_impl *impl, struct wlr_output *output);

#endif
