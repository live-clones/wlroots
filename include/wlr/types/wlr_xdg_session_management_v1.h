/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_XDG_SESSION_MANAGEMENT_V1_H
#define WLR_TYPES_WLR_XDG_SESSION_MANAGEMENT_V1_H

#include <wayland-server-core.h>
#include <wayland-protocols/xdg-session-management-v1-enum.h>

/**
 * A toplevel part of a session.
 */
struct wlr_xdg_toplevel_session_v1 {
	struct wl_resource *resource;
	struct wlr_xdg_session_v1 *session;
	struct wlr_xdg_toplevel *toplevel;
	struct wl_list link; // wlr_xdg_session_v1.toplevels

	// Client-provided identifier, unique per session.
	char *name;

	struct {
		struct wl_signal destroy;

		// Toplevel session name has changed.
		struct wl_signal rename;
	} events;

	struct {
		bool restorable;
		struct wl_listener toplevel_destroy;
	} WLR_PRIVATE;
};

/**
 * An application session.
 *
 * When a new session is created, compositors should load any saved state based
 * on wlr_xdg_session_v1.id (if non-NULL), then call
 * wlr_xdg_session_v1_notify_created() or wlr_xdg_session_v1_notify_restored().
 *
 * Compositors should save toplevel state for all toplevels added to the
 * session.
 */
struct wlr_xdg_session_v1 {
	struct wl_resource *resource;
	struct wl_list link; // wlr_xdg_session_manager_v1.sessions

	enum xdg_session_manager_v1_reason reason;
	char *id;
	struct wl_list toplevels; // wlr_xdg_toplevel_session_v1.link

	struct {
		struct wl_signal destroy;

		// All session saved state to be erased.
		struct wl_signal remove;

		// Toplevel state should be tracked as part of the session.
		struct wl_signal add_toplevel; // struct wlr_xdg_toplevel_session_v1 *

		// If the toplevel state was saved, it should be restored and
		// wlr_xdg_toplevel_session_v1_notify_restored() should be called. Then
		// toplevel state should be tracked as part of the session.
		struct wl_signal restore_toplevel; // struct wlr_xdg_toplevel_session_v1 *
	} events;

	struct {
		bool initialized; // added or restored
	} WLR_PRIVATE;
};

/**
 * Session manager global.
 */
struct wlr_xdg_session_manager_v1 {
	struct wl_global *global;

	struct wl_list sessions; // wlr_xdg_session_v1.link

	struct {
		struct wl_signal destroy;
		struct wl_signal new_session; // struct wlr_xdg_session_v1 *
	} events;

	struct {
		struct wl_listener display_destroy;
	} WLR_PRIVATE;
};

/**
 * Create the session manager global.
 */
struct wlr_xdg_session_manager_v1 *wlr_xdg_session_manager_v1_create(
	struct wl_display *display, uint32_t version);

/**
 * Notify that a new session state has been created.
 *
 * This function should be called in response to
 * wlr_xdg_session_manager_v1.events.new_session if no state could be restored.
 */
void wlr_xdg_session_v1_notify_created(struct wlr_xdg_session_v1 *session, const char *session_id);

/**
 * Notify that the session has been restored.
 *
 * This function should be called in response to
 * wlr_xdg_session_manager_v1.events.new_session if at least some state could
 * be restored.
 */
void wlr_xdg_session_v1_notify_restored(struct wlr_xdg_session_v1 *session);

/**
 * Notify that the session has been taken over by another client.
 *
 * This function destroys the session.
 */
void wlr_xdg_session_v1_notify_replaced_and_destroy(struct wlr_xdg_session_v1 *session);

/**
 * Notify that the toplevel state has been restored.
 *
 * This event is part of a toplevel configure sequence.
 *
 * This function should be called in response to
 * wlr_xdg_session_v1.events.restore_toplevel if the toplevel was successfully
 * restored.
 */
uint32_t wlr_xdg_toplevel_session_v1_notify_restored(
	struct wlr_xdg_toplevel_session_v1 *toplevel_session);

#endif
