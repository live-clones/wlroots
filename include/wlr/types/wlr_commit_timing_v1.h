/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_TIMING_H
#define WLR_TYPES_WLR_TIMING_H

#include <wlr/types/wlr_compositor.h>

struct wlr_commit_timing_manager_v1_new_timer_event {
	struct wlr_commit_timer_v1 *timer;
};

struct wlr_commit_timer_v1_commit {
	struct wlr_commit_timer_v1 *timer;

	/**
	 * Commits' wlr_surface pending sequence when locking through
	 * wlr_surface_lock_pending(). Used to unlock the commit when the 'unlock_timer' goes off.
	 */
	uint32_t pending_seq;

	/**
	 * Timer for when this commit should be unlocked for presentation.
	 */
	struct wl_event_source *unlock_source;

	struct wl_list link; // wlr_commit_timer_v1::commits
};

struct wlr_commit_timer_v1_state {
	uint64_t base_present_nsec;
	int32_t refresh;
	uint64_t timestamp_nsec; // holds the timestamp for the .set_timestamp protocol request
};

struct wlr_commit_timer_v1 {
	struct wlr_commit_timing_manager_v1 *timing_manager;

	struct wl_resource *resource;
	struct wl_display *wl_display;
	struct wlr_addon addon;

	struct wlr_surface *surface;
	struct wlr_output *output;

	struct wlr_commit_timer_v1_state state;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct wl_listener client_commit;
		struct wl_listener output_present;
		struct wl_listener output_destroy;
	} WLR_PRIVATE;

	struct wl_list commits; // wlr_commit_timer_v1_commit.link

	struct wl_list scene_link; // wlr_scene.commit_timers
};

struct wlr_commit_timing_manager_v1 {
	struct wl_global *global;

	struct wl_listener display_destroy;

	struct {
		struct wl_signal new_timer;
		struct wl_signal destroy;
	} events;
};

/**
 * Set the output from which we will get the refresh cycle timings for this wlr_commit_timer_v1.
 */
void wlr_commit_timer_v1_set_output(struct wlr_commit_timer_v1 *timer, struct wlr_output *output);

/**
 * Create the wp_commit_timing_manager_v1_interface global, which can be used by clients to
 * set timestamps for surface commit request presentation.
 */
struct wlr_commit_timing_manager_v1 *wlr_commit_timing_manager_v1_create(struct wl_display *display,
	uint32_t version);

#endif
