/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_FRAME_SCHEDULER_H
#define WLR_TYPES_WLR_FRAME_SCHEDULER_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct wlr_frame_scheduler_impl;

struct wlr_frame_scheduler {
	struct wlr_output *output;

	struct {
		struct wl_signal frame;
	} events;

	// Whether the render loop should be kept awake. True if wlr_frame_scheduler_schedule_frame()
	// was called since the last frame event.
	bool needs_frame;

	// private state

	const struct wlr_frame_scheduler_impl *impl;

	struct wl_listener backend_needs_frame;
};

/**
 * The present scheduler maintains a render loop based on `wlr_output.events.present`. To wake the
 * render loop, it emits the frame signal when the compositor's event loop is idle.
 */
struct wlr_frame_scheduler *wlr_present_scheduler_create(struct wlr_output *output);

/**
 * Creates an appropriate frame scheduler for the given output's backend capabilities.
 */
struct wlr_frame_scheduler *wlr_frame_scheduler_autocreate(struct wlr_output *output);
/**
 * Inform the scheduler that a frame signal is needed. The scheduler implementation will choose a
 * good time to emit the signal. The signal is emitted only if this function has been called at
 * least once since the last signal.
 */
void wlr_frame_scheduler_schedule_frame(struct wlr_frame_scheduler *scheduler);
/**
 * Emits a frame signal if `wlr_frame_scheduler_schedule_frame()` has been called since the last
 * frame signal.
 */
void wlr_frame_scheduler_emit_frame(struct wlr_frame_scheduler *scheduler);
void wlr_frame_scheduler_destroy(struct wlr_frame_scheduler *scheduler);

#endif
