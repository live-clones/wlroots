/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_EXT_SCREENCOPY_V1_H
#define WLR_TYPES_WLR_EXT_SCREENCOPY_V1_H

#include <wlr/util/box.h>

#include <wayland-server-core.h>
#include <pixman.h>
#include <stdbool.h>

struct wlr_buffer;

struct wlr_ext_screencopy_manager_v1 {
	struct wl_global *global;

	struct wl_listener display_destroy;

	struct {
		struct wl_signal destroy;
	} events;

	void *data;
};

enum wlr_ext_screencopy_session_v1_state {
	WLR_EXT_SCREENCOPY_SESSION_V1_STATE_WAITING_FOR_BUFFER_FORMATS = 0,
	WLR_EXT_SCREENCOPY_SESSION_V1_STATE_READY,
};

struct wlr_ext_screencopy_session_v1_buffer {
	struct wl_resource *resource;
	struct pixman_region32 damage;
	struct wl_listener destroy;
};

struct wlr_ext_screencopy_session_v1 {
	struct wl_resource *resource;

	enum wlr_ext_screencopy_session_v1_state state;

	struct wlr_buffer *buffer;

	uint32_t wl_shm_format;
	int wl_shm_stride;
	uint32_t dmabuf_format;

	uint32_t cursor_wl_shm_format;
	int cursor_wl_shm_stride;
	uint32_t cursor_dmabuf_format;

	int cursor_width;
	int cursor_height;
	bool have_cursor;

        struct wlr_box last_cursor_box;

	uint32_t session_options;

	struct wlr_ext_screencopy_session_v1_buffer staged_buffer;
	struct wlr_ext_screencopy_session_v1_buffer current_buffer;

	struct wlr_ext_screencopy_session_v1_buffer staged_cursor_buffer;
	struct wlr_ext_screencopy_session_v1_buffer current_cursor_buffer;

	bool committed;

	bool have_presentation_time;
	uint64_t last_presentation_time_us;

	/* Accumulated damage for the session */
	struct pixman_region32 frame_damage;
	struct pixman_region32 cursor_damage;

	struct wlr_output *output;

	struct wl_listener output_precommit;
	struct wl_listener output_commit;
	struct wl_listener output_destroy;
	struct wl_listener output_set_cursor;
	struct wl_listener output_move_cursor;
	struct wl_listener output_present;

	void *data;
};

struct wlr_ext_screencopy_manager_v1 *wlr_ext_screencopy_manager_v1_create(
		struct wl_display *display);

#endif
