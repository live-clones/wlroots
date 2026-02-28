/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_DATA_RECEIVER_H
#define WLR_TYPES_WLR_DATA_RECEIVER_H

#include <wayland-server-core.h>
#include <unistd.h>

struct wlr_data_receiver;

/**
 * A data receiver implementation. All callbacks are optional.
 */
struct wlr_data_receiver_impl {
	/**
	 * Called when the transfer is cancelled before completion. This should
	 * clean up any ongoing transfer state. The fd will be automatically
	 * closed after this callback returns.
	 */
	void (*cancelled)(struct wlr_data_receiver *receiver);

	/**
	 * Called when the receiver is being destroyed. This should free any
	 * resources allocated for the receiver implementation.
	 */
	void (*destroy)(struct wlr_data_receiver *receiver);
};

/**
 * A receiver is the receiving side of a data transfer. It represents the
 * target of clipboard, primary selection, or drag-and-drop operations.
 *
 * This abstraction unifies the handling of data transfers across different
 * protocols (Wayland native, XWayland, etc.) and provides a consistent
 * interface for permission systems and transfer tracking.
 */
struct wlr_data_receiver {
	const struct wlr_data_receiver_impl *impl;

	/**
	 * File descriptor for data transfer. This is typically a pipe write end
	 * that will be passed to the data source for writing clipboard data.
	 */
	int fd;

	/**
	 * Process ID of the receiving client. For XWayland windows, this is the
	 * X11 client PID, not the XWayland server PID. For native Wayland clients,
	 * this is the client process PID.
	 */
	pid_t pid;

	/**
	 * The Wayland client associated with this receiver. For XWayland windows,
	 * this refers to the XWayland server client, not the X11 application.
	 * For native Wayland clients, this is the actual client.
	 */
	struct wl_client *client;

	struct {
		struct wl_signal destroy;
	} events;
};

/**
 * Initialize a data receiver with the given implementation. The receiver
 * should be embedded in a larger structure (e.g., wlr_data_offer) to manage
 * its lifecycle properly.
 */
void wlr_data_receiver_init(struct wlr_data_receiver *receiver,
	const struct wlr_data_receiver_impl *impl);

/**
 * Destroy a data receiver. This will emit the destroy signal, call the
 * implementation's destroy callback if present, and close any open file
 * descriptor. After calling this function, the receiver should not be used.
 */
void wlr_data_receiver_destroy(struct wlr_data_receiver *receiver);

/**
 * Notify the receiver that the transfer has been cancelled. This calls the
 * cancelled callback if implemented, then automatically closes the file
 * descriptor. Should be called when a transfer is aborted before completion,
 * such as when a drag operation is cancelled or a selection is cleared.
 */
void wlr_data_receiver_cancelled(struct wlr_data_receiver *receiver);


#endif
