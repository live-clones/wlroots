/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_PRIMARY_SELECTION_H
#define WLR_TYPES_WLR_PRIMARY_SELECTION_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_data_receiver.h>

struct wlr_primary_selection_source;

/**
 * A data source implementation. Only the `send` function is mandatory.
 */
struct wlr_primary_selection_source_impl {
	void (*send)(struct wlr_primary_selection_source *source,
		const char *mime_type, struct wlr_data_receiver *receiver);
	void (*destroy)(struct wlr_primary_selection_source *source);

	/**
	 * Returns the unwrapped source object. This source object maybe is a
	 * wrapper, its a proxy for the others source.
	 */
	struct wlr_primary_selection_source *(*get_original)(struct wlr_primary_selection_source *source);
};

/**
 * A source is the sending side of a selection.
 */
struct wlr_primary_selection_source {
	const struct wlr_primary_selection_source_impl *impl;

	// source metadata
	struct wl_array mime_types;

	// source information
	struct wl_client *client;
	pid_t pid; // PID of the source process (for XWayland, X11 client PID)

	struct {
		struct wl_signal destroy;
	} events;

	void *data;
};

void wlr_primary_selection_source_init(
	struct wlr_primary_selection_source *source,
	const struct wlr_primary_selection_source_impl *impl);
void wlr_primary_selection_source_destroy(
	struct wlr_primary_selection_source *source);
void wlr_primary_selection_source_send(
	struct wlr_primary_selection_source *source,
	const char *mime_type, struct wlr_data_receiver *receiver);

/**
 * Copy metadata from one primary selection source to another. This is useful for implementing
 * wrapper primary selection sources that can filter MIME types or other metadata.
 */
void wlr_primary_selection_source_copy(struct wlr_primary_selection_source *dest,
	struct wlr_primary_selection_source *src);

/**
 * Returns the original primary selection source object, it isn't NULL if the source argument
 * isn't NULL.
 */
struct wlr_primary_selection_source *
wlr_primary_selection_source_get_original(struct wlr_primary_selection_source *source);

/**
 * Request setting the primary selection. If `client` is not null, then the
 * serial will be checked against the set of serials sent to the client on that
 * seat.
 */
void wlr_seat_request_set_primary_selection(struct wlr_seat *seat,
	struct wlr_seat_client *client,
	struct wlr_primary_selection_source *source, uint32_t serial);
/**
 * Sets the current primary selection for the seat. NULL can be provided to
 * clear it. This removes the previous one if there was any. In case the
 * selection doesn't come from a client, wl_display_next_serial() can be used to
 * generate a serial.
 */
void wlr_seat_set_primary_selection(struct wlr_seat *seat,
	struct wlr_primary_selection_source *source, uint32_t serial);

#endif
