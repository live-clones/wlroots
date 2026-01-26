#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/xwayland.h>
#include "xwayland/selection.h"
#include <xcb/xcb.h>
#include <xcb/res.h>
#include <xkbcommon/xkbcommon.h>
#include <cairo.h>
#include <drm_fourcc.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/interfaces/wlr_buffer.h>

/* Clipboard control data source wrapper */
struct clipboard_data_source {
	struct wlr_data_source base;
	struct wlr_data_source *wrapped_source;
	struct wlr_seat *seat;
	struct clipboard_server *server;

	struct wl_listener wrapped_source_destroy;
	struct wl_list cache_link; // For wrapper cache reuse
};

/* Primary selection control data source wrapper */
struct clipboard_primary_source {
	struct wlr_primary_selection_source base;
	struct wlr_primary_selection_source *wrapped_source;
	struct wlr_seat *seat;
	struct clipboard_server *server;

	struct wl_listener wrapped_source_destroy;
	struct wl_list cache_link; // For wrapper cache reuse
};

enum clipboard_request_type {
	CLIPBOARD_REQUEST_SELECTION,
	CLIPBOARD_REQUEST_PRIMARY,
	CLIPBOARD_REQUEST_DRAG,
};

/* Pending clipboard request */
struct clipboard_request {
	enum clipboard_request_type type;
	union {
		struct clipboard_data_source *data_source;
		struct clipboard_primary_source *primary_source;
	};
	struct wlr_data_receiver *receiver;
	char *mime_type;

	/* For async processing */
	struct clipboard_server *server;
	bool waiting_for_response;

	struct wl_list link;

	/* Listener for receiver destroy signal */
	struct wl_listener receiver_destroy;
};

enum clipboard_cursor_mode {
	CLIPBOARD_CURSOR_PASSTHROUGH,
	CLIPBOARD_CURSOR_MOVE,
	CLIPBOARD_CURSOR_RESIZE,
};

struct clipboard_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_compositor *compositor;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list toplevels;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum clipboard_cursor_mode cursor_mode;
	struct clipboard_toplevel *grabbed_toplevel;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	struct wlr_xwayland *xwayland;

	/* XWayland surface support */
	struct wl_listener xwayland_new_surface;

	/* Startup command storage */
	char *startup_cmd;
	struct wl_listener xwayland_ready;

	/* Primary selection support */
	struct wlr_primary_selection_v1_device_manager *primary_selection_manager;
	struct wl_listener request_set_primary_selection;

	/* Drag and drop support */
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;

	/* Clipboard control */
	struct wl_list pending_requests; // clipboard_request::link

	/* Wrapper caches for reuse */
	struct wl_list active_data_source_wrappers; // clipboard_data_source::cache_link
	struct wl_list active_primary_source_wrappers; // clipboard_primary_source::cache_link

	/* Dialog system */
	bool dialog_visible;
	struct wlr_scene_buffer *dialog_buffer;
	struct clipboard_request *current_request;
	struct wlr_buffer *dialog_wlr_buffer;
};

/* PID query function for X11 windows */
/* Get command line for a PID (Linux/proc filesystem only) */
static char* get_pid_cmdline(pid_t pid) {
	if (pid <= 0) {
		return NULL;
	}

#ifdef __linux__
	// Check if /proc filesystem is available
	struct stat st;
	if (stat("/proc", &st) != 0 || !S_ISDIR(st.st_mode)) {
		return NULL;
	}

	char proc_path[256];
	snprintf(proc_path, sizeof(proc_path), "/proc/%d/cmdline", pid);

	FILE *file = fopen(proc_path, "r");
	if (!file) {
		return NULL;
	}

	char *cmdline = calloc(1024, 1);
	if (!cmdline) {
		fclose(file);
		return NULL;
	}

	size_t len = fread(cmdline, 1, 1023, file);
	fclose(file);

	if (len == 0) {
		free(cmdline);
		return NULL;
	}

	// Replace null bytes with spaces (cmdline uses null separators)
	for (size_t i = 0; i < len; i++) {
		if (cmdline[i] == '\0') {
			cmdline[i] = ' ';
		}
	}

	// Remove trailing spaces
	while (len > 0 && cmdline[len - 1] == ' ') {
		cmdline[--len] = '\0';
	}

	return cmdline;
#else
	// Not supported on non-Linux systems
	(void)pid;
	return NULL;
#endif
}

/* Cairo buffer implementation for dialog */
struct cairo_dialog_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void cairo_dialog_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct cairo_dialog_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	wlr_buffer_finish(wlr_buffer);
	cairo_surface_destroy(buffer->surface);
	free(buffer);
}

static bool cairo_dialog_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct cairo_dialog_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);

	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
		return false;
	}

	*format = DRM_FORMAT_ARGB8888;
	*data = cairo_image_surface_get_data(buffer->surface);
	*stride = cairo_image_surface_get_stride(buffer->surface);
	return true;
}

static void cairo_dialog_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	// Nothing to do
}

static const struct wlr_buffer_impl cairo_dialog_buffer_impl = {
	.destroy = cairo_dialog_buffer_destroy,
	.begin_data_ptr_access = cairo_dialog_buffer_begin_data_ptr_access,
	.end_data_ptr_access = cairo_dialog_buffer_end_data_ptr_access
};

/* Handle receiver destroy signal */
static void handle_request_receiver_destroy(struct wl_listener *listener, void *data) {
	struct clipboard_request *request =
		wl_container_of(listener, request, receiver_destroy);

	printf("Receiver destroyed, cleaning up request\n");

	/* Remove the receiver reference */
	request->receiver = NULL;

	wl_list_remove(&request->receiver_destroy.link);
}

static void safe_receiver_cancelled(struct wlr_data_receiver *receiver) {
	if (receiver) {
		wlr_data_receiver_cancelled(receiver);
	}
}

/* Create dialog buffer using Cairo */
static struct wlr_buffer *create_dialog_buffer(struct clipboard_server *server,
	struct clipboard_request *request) {
	const int width = 480;
	const int height = 200;

	struct cairo_dialog_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		return NULL;
	}

	wlr_buffer_init(&buffer->base, &cairo_dialog_buffer_impl, width, height);

	buffer->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(buffer->surface) != CAIRO_STATUS_SUCCESS) {
		free(buffer);
		return NULL;
	}

	// Draw the dialog using Cairo
	cairo_t *cr = cairo_create(buffer->surface);

	// Clear background with blue dialog color
	cairo_set_source_rgba(cr, 0.2, 0.3, 0.5, 0.9);
	cairo_paint(cr);

	// Draw border
	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
	cairo_set_line_width(cr, 4.0);
	cairo_rectangle(cr, 2, 2, width - 4, height - 4);
	cairo_stroke(cr);

	// Draw title
	cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, 1.0);
	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 16);
	cairo_move_to(cr, 10, 25);

	const char *operation_type = "";
	switch (request->type) {
	case CLIPBOARD_REQUEST_SELECTION:
		operation_type = "Clipboard Selection Request";
		break;
	case CLIPBOARD_REQUEST_PRIMARY:
		operation_type = "Primary Selection Request";
		break;
	case CLIPBOARD_REQUEST_DRAG:
		operation_type = "Drag & Drop Request";
		break;
	}
	cairo_show_text(cr, operation_type);

#ifdef __linux__
	// Check if /proc filesystem is available
	struct stat st;
	bool proc_available = (stat("/proc", &st) == 0 && S_ISDIR(st.st_mode));
#else
	bool proc_available = false;
#endif

	// Draw content with conditional command line info
	cairo_set_font_size(cr, 12);
	int current_y = 50;

	char info_text[512];

	// Get source PID
	pid_t source_pid = 0;
	if (request->type == CLIPBOARD_REQUEST_SELECTION &&
		request->data_source && request->data_source->wrapped_source) {
		source_pid = request->data_source->wrapped_source->pid;
	} else if (request->type == CLIPBOARD_REQUEST_PRIMARY &&
		request->primary_source && request->primary_source->wrapped_source) {
		source_pid = request->primary_source->wrapped_source->pid;
	}

	// Source PID info
	cairo_move_to(cr, 10, current_y);
	if (proc_available) {
		char *source_cmdline = get_pid_cmdline(source_pid);
		if (source_cmdline) {
			snprintf(info_text, sizeof(info_text), "Source: PID %d (%s)",
				source_pid, source_cmdline);
			free(source_cmdline);
		} else {
			snprintf(info_text, sizeof(info_text), "Source: PID %d", source_pid);
		}
	} else {
		snprintf(info_text, sizeof(info_text), "Source: PID %d", source_pid);
	}
	cairo_show_text(cr, info_text);
	current_y += 20;

	// Target info
	cairo_move_to(cr, 10, current_y);
	pid_t target_pid = request->receiver ? request->receiver->pid : 0;
	if (proc_available && target_pid > 0) {
		char *target_cmdline = get_pid_cmdline(target_pid);
		if (target_cmdline) {
			snprintf(info_text, sizeof(info_text), "Target: PID %d (%s)",
				target_pid, target_cmdline);
			free(target_cmdline);
		} else {
			snprintf(info_text, sizeof(info_text), "Target: PID %d", target_pid);
		}
	} else if (target_pid > 0) {
		snprintf(info_text, sizeof(info_text), "Target: PID %d", target_pid);
	} else {
		snprintf(info_text, sizeof(info_text), "Target: Unknown client");
	}
	cairo_show_text(cr, info_text);
	current_y += 20;

	// MIME type
	cairo_move_to(cr, 10, current_y);
	snprintf(info_text, sizeof(info_text), "MIME type: %s", request->mime_type);
	cairo_show_text(cr, info_text);

	// Draw instructions
	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
	cairo_set_font_size(cr, 14);
	cairo_move_to(cr, 10, 140);
	cairo_show_text(cr, "Press Y to Allow, N to Deny");

	// Draw keyboard hint boxes
	cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 0.8);
	cairo_rectangle(cr, 10, 160, 30, 25);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
	cairo_move_to(cr, 20, 178);
	cairo_show_text(cr, "Y");

	cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.8);
	cairo_rectangle(cr, 50, 160, 30, 25);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
	cairo_move_to(cr, 60, 178);
	cairo_show_text(cr, "N");

	cairo_destroy(cr);

	return &buffer->base;
}

/* Show graphical dialog for clipboard approval */
static void show_graphical_dialog(struct clipboard_server *server,
		struct clipboard_request *request) {
	if (server->dialog_visible) {
		// Hide existing dialog first
		if (server->dialog_buffer) {
			wlr_scene_node_destroy(&server->dialog_buffer->node);
			server->dialog_buffer = NULL;
		}
		if (server->dialog_wlr_buffer) {
			wlr_buffer_drop(server->dialog_wlr_buffer);
			server->dialog_wlr_buffer = NULL;
		}
	}

	// Create dialog buffer
	server->dialog_wlr_buffer = create_dialog_buffer(server, request);
	if (!server->dialog_wlr_buffer) {
		printf("Failed to create dialog buffer, falling back to console\n");
		return;
	}

	// Add dialog to scene at the center
	server->dialog_buffer = wlr_scene_buffer_create(&server->scene->tree, server->dialog_wlr_buffer);
	if (!server->dialog_buffer) {
		wlr_buffer_drop(server->dialog_wlr_buffer);
		server->dialog_wlr_buffer = NULL;
		return;
	}

	// Center the dialog on screen
	struct wlr_output *output = wlr_output_layout_get_center_output(server->output_layout);
	if (output) {
		wlr_scene_node_set_position(&server->dialog_buffer->node,
			output->width / 2 - 240, output->height / 2 - 100);
	} else {
		// Default position if no output found
		wlr_scene_node_set_position(&server->dialog_buffer->node, 100, 100);
	}

	server->dialog_visible = true;
	server->current_request = request;

	printf("Graphical dialog shown - Press Y to allow, N to deny\n");
}

/* Process next pending dialog request */
static void process_next_pending_dialog(struct clipboard_server *server) {
	if (server->dialog_visible) {
		// Already showing a dialog
		return;
	}

	if (wl_list_empty(&server->pending_requests)) {
		// No pending requests
		return;
	}

	// Get the next request from the pending list
	struct clipboard_request *next_request = wl_container_of(
		server->pending_requests.next, next_request, link);

	// Remove from pending list (but don't free it yet)
	wl_list_remove(&next_request->link);

	// Show the dialog for this request
	if (server->scene) {
		show_graphical_dialog(server, next_request);
	}

	printf("Processing next pending dialog request\n");
}

/* Hide the graphical dialog */
static void hide_dialog(struct clipboard_server *server) {
	if (!server->dialog_visible) {
		return;
	}

	if (server->dialog_buffer) {
		wlr_scene_node_destroy(&server->dialog_buffer->node);
		server->dialog_buffer = NULL;
	}

	if (server->dialog_wlr_buffer) {
		wlr_buffer_drop(server->dialog_wlr_buffer);
		server->dialog_wlr_buffer = NULL;
	}

	server->dialog_visible = false;
	server->current_request = NULL;

	// Process next pending dialog request
	process_next_pending_dialog(server);
}

/* Handle dialog response */
static void handle_dialog_response(struct clipboard_server *server, bool approved) {
	if (!server->dialog_visible || !server->current_request) {
		return;
	}

	struct clipboard_request *request = server->current_request;

	if (approved) {
		printf("✓ Clipboard transfer approved via GUI\n\n");
	} else {
		printf("✗ Clipboard transfer denied via GUI\n\n");
	}

	// Process the request based on its type
	switch (request->type) {
	case CLIPBOARD_REQUEST_SELECTION:
		if (approved && request->data_source &&
			request->data_source->wrapped_source && request->receiver) {
			// Send data using the new interface
			wlr_data_source_send(request->data_source->wrapped_source,
				request->mime_type, request->receiver);
		} else {
			safe_receiver_cancelled(request->receiver);
		}
		break;

	case CLIPBOARD_REQUEST_PRIMARY:
		if (approved && request->primary_source &&
			request->primary_source->wrapped_source && request->receiver) {
			// Send data using the new interface
			wlr_primary_selection_source_send(request->primary_source->wrapped_source,
				request->mime_type, request->receiver);
		} else {
			// Handle denial
			safe_receiver_cancelled(request->receiver);
		}
		break;

	case CLIPBOARD_REQUEST_DRAG:
		// Drag operations are handled differently
		break;
	}

	/* Remove receiver destroy listener if receiver still exists */
	if (request->receiver) {
		wl_list_remove(&request->receiver_destroy.link);
	}

	free(request->mime_type);
	free(request);

	hide_dialog(server);
}

/* Show graphical dialog for clipboard approval - ASYNC */
static void show_clipboard_dialog(struct clipboard_server *server,
		struct clipboard_request *request) {
	// Store the request for async processing
	request->server = server;
	request->waiting_for_response = true;

	// If no dialog is currently visible, show this one immediately
	if (!server->dialog_visible) {
		// Show graphical dialog (if possible)
		if (server->scene) {
			show_graphical_dialog(server, request);
		} else {
			printf("Failed to show graphical dialog, using console fallback\n");
		}
	} else {
		// Add to pending requests list
		wl_list_insert(&server->pending_requests, &request->link);
		printf("Dialog already visible, added request to pending queue\n");
	}

	// Print console info as well
	const char *operation_type = "";
	switch (request->type) {
	case CLIPBOARD_REQUEST_SELECTION:
		operation_type = "Clipboard Selection";
		break;
	case CLIPBOARD_REQUEST_PRIMARY:
		operation_type = "Primary Selection";
		break;
	case CLIPBOARD_REQUEST_DRAG:
		operation_type = "Drag & Drop";
		break;
	}

	printf("\n========== %s Request ==========\n", operation_type);

	// Show source process information with command line
	pid_t source_pid = 0;
	if (request->type == CLIPBOARD_REQUEST_SELECTION &&
		request->data_source && request->data_source->wrapped_source) {
		source_pid = request->data_source->wrapped_source->pid;
	} else if (request->type == CLIPBOARD_REQUEST_PRIMARY &&
		request->primary_source && request->primary_source->wrapped_source) {
		source_pid = request->primary_source->wrapped_source->pid;
	}

	printf("Source PID: %d", source_pid);
	char *source_cmdline = get_pid_cmdline(source_pid);
	if (source_cmdline) {
		printf(" (%s)", source_cmdline);
		free(source_cmdline);
	}
	printf("\n");

	// Show target process information
	pid_t target_pid = request->receiver ? request->receiver->pid : 0;
	if (target_pid > 0) {
		printf("Target PID: %d", target_pid);
		char *target_cmdline = get_pid_cmdline(target_pid);
		if (target_cmdline) {
			printf(" (%s)", target_cmdline);
			free(target_cmdline);
		}
		printf("\n");
	} else {
		printf("Target: Unknown client\n");
	}
	printf("MIME type: %s\n", request->mime_type);
	printf("================================================\n");
	printf("Press Y to Allow, N to Deny\n");
}

static void clipboard_data_source_send(struct wlr_data_source *source,
		const char *mime_type, struct wlr_data_receiver *receiver) {
	struct clipboard_data_source *wrapper =
		wl_container_of(source, wrapper, base);

	if (!wrapper->wrapped_source) {
		wlr_data_receiver_cancelled(receiver);
		return;
	}

	/* Create clipboard request */
	struct clipboard_request *request = calloc(1, sizeof(*request));
	if (!request) {
		wlr_data_receiver_cancelled(receiver);
		return;
	}

	request->type = CLIPBOARD_REQUEST_SELECTION;
	request->data_source = wrapper;
	request->receiver = receiver;
	request->mime_type = strdup(mime_type);

	/* Set up receiver destroy listener */
	request->receiver_destroy.notify = handle_request_receiver_destroy;
	wl_signal_add(&receiver->events.destroy, &request->receiver_destroy);

	/* Show dialog for approval - ASYNC */
	show_clipboard_dialog(wrapper->server, request);

	/* The actual transfer will happen in handle_dialog_response */
	/* Don't cleanup request here - it will be cleaned up after user response */
}

static void clipboard_data_source_accept(struct wlr_data_source *source,
		uint32_t serial, const char *mime_type, struct wlr_data_receiver *receiver) {
	struct clipboard_data_source *wrapper =
		wl_container_of(source, wrapper, base);

	if (!wrapper->wrapped_source) {
		return;
	}

	wlr_data_source_accept(wrapper->wrapped_source, serial, mime_type, receiver);
}

static void remove_data_source_wrapper(struct clipboard_data_source *wrapper) {
	printf("Removing data source wrapper from active list\n");
	wl_list_remove(&wrapper->cache_link);
	free(wrapper);
}

static void clipboard_data_source_destroy(struct wlr_data_source *source) {
	struct clipboard_data_source *wrapper =
		wl_container_of(source, wrapper, base);

	if (wrapper->wrapped_source) {
		wl_list_remove(&wrapper->wrapped_source_destroy.link);
		wlr_data_source_destroy(wrapper->wrapped_source);
	}

	/* Return wrapper to cache instead of freeing */
	remove_data_source_wrapper(wrapper);
}

static struct wlr_data_source *clipboard_data_source_get_original(struct wlr_data_source *source) {
	struct clipboard_data_source *wrapper =
		wl_container_of(source, wrapper, base);

	if (wrapper->wrapped_source) {
		return wlr_data_source_get_original(wrapper->wrapped_source);
	}

	return source;
}

static void handle_wrapped_source_destroy(struct wl_listener *listener, void *data) {
	struct clipboard_data_source *wrapper =
		wl_container_of(listener, wrapper, wrapped_source_destroy);

	wl_list_remove(&wrapper->wrapped_source_destroy.link);
	wrapper->wrapped_source = NULL;
}

static const struct wlr_data_source_impl clipboard_source_impl = {
	.send = clipboard_data_source_send,
	.accept = clipboard_data_source_accept,
	.destroy = clipboard_data_source_destroy,
	.get_original = clipboard_data_source_get_original,
};

/* Primary selection wrapper functions */
static void clipboard_primary_source_send(struct wlr_primary_selection_source *source,
		const char *mime_type, struct wlr_data_receiver *receiver) {
	struct clipboard_primary_source *wrapper =
		wl_container_of(source, wrapper, base);

	if (!wrapper->wrapped_source) {
		wlr_data_receiver_cancelled(receiver);
		return;
	}

	/* Create primary selection request */
	struct clipboard_request *request = calloc(1, sizeof(*request));
	if (!request) {
		wlr_data_receiver_cancelled(receiver);
		wlr_primary_selection_source_send(wrapper->wrapped_source, mime_type, receiver);
		return;
	}

	request->type = CLIPBOARD_REQUEST_PRIMARY;
	request->primary_source = wrapper;
	request->receiver = receiver;
	request->mime_type = strdup(mime_type);

	/* Set up receiver destroy listener */
	request->receiver_destroy.notify = handle_request_receiver_destroy;
	wl_signal_add(&receiver->events.destroy, &request->receiver_destroy);

	/* Show dialog for approval - ASYNC */
	show_clipboard_dialog(wrapper->server, request);

	/* The actual transfer will happen in handle_dialog_response */
	/* Don't cleanup request here - it will be cleaned up after user response */
}

static void remove_primary_source_wrapper(struct clipboard_primary_source *wrapper) {
	printf("Removing primary source wrapper from active list\n");
	wl_list_remove(&wrapper->cache_link);
	free(wrapper);
}

static void clipboard_primary_source_destroy(struct wlr_primary_selection_source *source) {
	struct clipboard_primary_source *wrapper =
		wl_container_of(source, wrapper, base);

	if (wrapper->wrapped_source) {
		wl_list_remove(&wrapper->wrapped_source_destroy.link);
		wlr_primary_selection_source_destroy(wrapper->wrapped_source);
	}

	/* Return wrapper to cache instead of freeing */
	remove_primary_source_wrapper(wrapper);
}

static struct wlr_primary_selection_source *clipboard_primary_source_get_original(struct wlr_primary_selection_source *source) {
	struct clipboard_primary_source *wrapper =
		wl_container_of(source, wrapper, base);

	if (wrapper->wrapped_source) {
		return wlr_primary_selection_source_get_original(wrapper->wrapped_source);
	}

	return source;
}

static void handle_wrapped_primary_source_destroy(struct wl_listener *listener, void *data) {
	struct clipboard_primary_source *wrapper =
		wl_container_of(listener, wrapper, wrapped_source_destroy);

	wl_list_remove(&wrapper->wrapped_source_destroy.link);
	wrapper->wrapped_source = NULL;
}

static const struct wlr_primary_selection_source_impl clipboard_primary_source_impl = {
	.send = clipboard_primary_source_send,
	.destroy = clipboard_primary_source_destroy,
	.get_original = clipboard_primary_source_get_original,
};

/* Wrapper cache management functions */
static struct clipboard_data_source *get_or_create_data_source_wrapper(
	struct clipboard_server *server, struct wlr_data_source *source) {
	/* First, try to find existing wrapper for this source */
	struct clipboard_data_source *wrapper;
	wl_list_for_each(wrapper, &server->active_data_source_wrappers, cache_link) {
		if (wrapper->wrapped_source == source) {
			printf("Found existing data source wrapper for source %p\n", source);
			/* Update wrapper data */
			return wrapper;
		}
	}

	/* Allocate new wrapper if none found */
	wrapper = calloc(1, sizeof(struct clipboard_data_source));
	if (!wrapper) {
		return NULL;
	}

	printf("Creating new data source wrapper for source %p\n", source);
	wrapper->wrapped_source = source;
	wrapper->seat = server->seat;
	wrapper->server = server;

	/* Initialize wrapper with clipboard control impl */
	wlr_data_source_init(&wrapper->base, &clipboard_source_impl);

	/* Add to active list */
	wl_list_insert(&server->active_data_source_wrappers, &wrapper->cache_link);

	/* Listen for original source destruction */
	wrapper->wrapped_source_destroy.notify = handle_wrapped_source_destroy;
	wl_signal_add(&source->events.destroy, &wrapper->wrapped_source_destroy);

	return wrapper;
}

static struct clipboard_primary_source *get_or_create_primary_source_wrapper(
	struct clipboard_server *server, struct wlr_primary_selection_source *source) {
	/* First, try to find existing wrapper for this source */
	struct clipboard_primary_source *wrapper;
	wl_list_for_each(wrapper, &server->active_primary_source_wrappers, cache_link) {
		if (wrapper->wrapped_source == source) {
			printf("Found existing primary source wrapper for source %p\n", source);
			/* Update wrapper data */
			return wrapper;
		}
	}

	/* Allocate new wrapper if none found */
	wrapper = calloc(1, sizeof(struct clipboard_primary_source));
	if (!wrapper) {
		return NULL;
	}

	printf("Creating new primary source wrapper for source %p\n", source);
	wrapper->wrapped_source = source;
	wrapper->seat = server->seat;
	wrapper->server = server;

	/* Initialize wrapper with primary selection control impl */
	wlr_primary_selection_source_init(&wrapper->base, &clipboard_primary_source_impl);

	/* Add to active list */
	wl_list_insert(&server->active_primary_source_wrappers, &wrapper->cache_link);

	/* Listen for original source destruction */
	wrapper->wrapped_source_destroy.notify = handle_wrapped_primary_source_destroy;
	wl_signal_add(&source->events.destroy, &wrapper->wrapped_source_destroy);

	return wrapper;
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct clipboard_server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;

	if (event->source == NULL) {
		wlr_seat_set_selection(server->seat, NULL, event->serial);
		return;
	}

	/* Get wrapper from cache or allocate new one */
	struct clipboard_data_source *wrapper = get_or_create_data_source_wrapper(server, event->source);
	if (!wrapper) {
		return;
	}

	/* Copy metadata from original source */
	wlr_data_source_copy(&wrapper->base, event->source);

	/* Use wrapper instead of original source */
	wlr_seat_set_selection(server->seat, &wrapper->base, event->serial);
}

static void seat_request_set_primary_selection(struct wl_listener *listener, void *data) {
	struct clipboard_server *server = wl_container_of(
			listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;

	if (event->source == NULL) {
		wlr_seat_set_primary_selection(server->seat, NULL, event->serial);
		return;
	}

	/* Get primary selection wrapper from cache or allocate new one */
	struct clipboard_primary_source *wrapper =
		get_or_create_primary_source_wrapper(server, event->source);
	if (!wrapper) {
		return;
	}

	/* Copy metadata from original source */
	wlr_primary_selection_source_copy(&wrapper->base, event->source);

	/* Use wrapper instead of original source */
	wlr_seat_set_primary_selection(server->seat, &wrapper->base, event->serial);
}

static void seat_request_start_drag(struct wl_listener *listener, void *data) {
	struct clipboard_server *server = wl_container_of(
			listener, server, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;

	/* DND operations are allowed without permission control */
	printf("✓ Drag & Drop operation allowed (no permission control)\n");
	wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
}

static void seat_start_drag(struct wl_listener *listener, void *data) {
	(void)listener; /* Unused parameter */
	(void)data; /* Unused parameter */

	printf("Drag operation started\n");
}

/* XWayland surface data structure */
struct clipboard_xwayland_surface {
	struct wlr_xwayland_surface *xsurface;
	struct clipboard_server *server;
	struct wlr_scene_surface *scene_surface;

	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener destroy;
	struct wl_listener set_geometry;
};

/* XWayland surface handlers */
static void xwayland_surface_set_geometry(struct wl_listener *listener, void *data) {
	struct clipboard_xwayland_surface *surface =
		wl_container_of(listener, surface, set_geometry);
	struct wlr_xwayland_surface *xsurface = surface->xsurface;

	printf("XWayland surface geometry changed: %s at position (%d, %d) size %dx%d\n",
		xsurface->title ? xsurface->title : "unknown",
		xsurface->x, xsurface->y, xsurface->width, xsurface->height);

	// Update the position when geometry changes
	if (surface->scene_surface) {
		wlr_scene_node_set_position(&surface->scene_surface->buffer->node,
			xsurface->x, xsurface->y);
		printf("Updated XWayland surface position to (%d, %d)\n", xsurface->x, xsurface->y);
	}
}

static void xwayland_surface_associate(struct wl_listener *listener, void *data) {
	struct clipboard_xwayland_surface *surface =
		wl_container_of(listener, surface, associate);
	struct wlr_xwayland_surface *xsurface = surface->xsurface;
	struct clipboard_server *server = surface->server;

	printf("XWayland surface associated: %s (class: %s, PID: %d) at position (%d, %d)\n",
		xsurface->title ? xsurface->title : "unknown",
		xsurface->class ? xsurface->class : "unknown",
		xsurface->pid, xsurface->x, xsurface->y);

	// Now we have the wl_surface, create scene node
	if (xsurface->surface) {
		surface->scene_surface = wlr_scene_surface_create(&server->scene->tree, xsurface->surface);
		if (surface->scene_surface) {
			surface->scene_surface->buffer->node.data = surface;
			printf("Created scene surface for XWayland surface\n");

			// Set initial position based on XWayland coordinates
			wlr_scene_node_set_position(&surface->scene_surface->buffer->node,
				xsurface->x, xsurface->y);
			printf("Set initial XWayland surface position to (%d, %d)\n", xsurface->x, xsurface->y);

			// Set keyboard focus for XWayland surface
			struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
			if (keyboard != NULL) {
				wlr_seat_keyboard_notify_enter(server->seat, xsurface->surface,
					keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
				printf("Set keyboard focus for XWayland surface\n");
			}
		}
	}
}

static void xwayland_surface_dissociate(struct wl_listener *listener, void *data) {
	struct clipboard_xwayland_surface *surface =
		wl_container_of(listener, surface, dissociate);

	printf("XWayland surface dissociated\n");

	// Clean up scene surface
	if (surface->scene_surface) {
		wlr_scene_node_destroy(&surface->scene_surface->buffer->node);
		surface->scene_surface = NULL;
	}
}

static void xwayland_surface_destroy(struct wl_listener *listener, void *data) {
	struct clipboard_xwayland_surface *surface =
		wl_container_of(listener, surface, destroy);

	printf("XWayland surface destroyed\n");

	// Clean up scene surface if still exists
	if (surface->scene_surface) {
		wlr_scene_node_destroy(&surface->scene_surface->buffer->node);
		surface->scene_surface = NULL;
	}

	// Remove listeners
	wl_list_remove(&surface->associate.link);
	wl_list_remove(&surface->dissociate.link);
	wl_list_remove(&surface->destroy.link);
	wl_list_remove(&surface->set_geometry.link);

	free(surface);
}

static void server_new_xwayland_surface(struct wl_listener *listener, void *data) {
	struct clipboard_server *server = wl_container_of(listener, server, xwayland_new_surface);
	struct wlr_xwayland_surface *xsurface = data;

	printf("New XWayland surface: %s (class: %s, PID: %d)\n",
		xsurface->title ? xsurface->title : "unknown",
		xsurface->class ? xsurface->class : "unknown",
		xsurface->pid);

	// Create wrapper structure for the XWayland surface
	struct clipboard_xwayland_surface *surface = calloc(1, sizeof(*surface));
	if (!surface) {
		return;
	}

	surface->xsurface = xsurface;
	surface->server = server;
	surface->scene_surface = NULL;

	// Set up listeners for associate/dissociate signals
	surface->associate.notify = xwayland_surface_associate;
	wl_signal_add(&xsurface->events.associate, &surface->associate);

	surface->dissociate.notify = xwayland_surface_dissociate;
	wl_signal_add(&xsurface->events.dissociate, &surface->dissociate);

	surface->destroy.notify = xwayland_surface_destroy;
	wl_signal_add(&xsurface->events.destroy, &surface->destroy);

	// Set up listener for geometry changes
	surface->set_geometry.notify = xwayland_surface_set_geometry;
	wl_signal_add(&xsurface->events.set_geometry, &surface->set_geometry);

	// Store our wrapper in the xsurface data
	xsurface->data = surface;

	// If already associated (unlikely but possible), handle it immediately
	if (xsurface->surface) {
		xwayland_surface_associate(&surface->associate, NULL);
	}
}

static void xwayland_ready(struct wl_listener *listener, void *data) {
	struct clipboard_server *server = wl_container_of(
			listener, server, xwayland_ready);

	/* XWayland is ready, now we can start the startup command with proper DISPLAY */
	if (server->startup_cmd) {
		/* Set the DISPLAY environment variable for X11 applications */
		if (server->xwayland->display_name) {
			setenv("DISPLAY", server->xwayland->display_name, true);
			printf("XWayland ready on DISPLAY=%s\n", server->xwayland->display_name);
		}

		printf("Starting command: %s\n", server->startup_cmd);
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", server->startup_cmd, (void *)NULL);
		}
	} else {
		/* Even without startup command, announce XWayland is ready */
		if (server->xwayland->display_name) {
			setenv("DISPLAY", server->xwayland->display_name, true);
			printf("XWayland ready on DISPLAY=%s\n", server->xwayland->display_name);
		}
	}
}

/* Basic compositor setup (reference from tinywl) */
struct clipboard_output {
	struct wl_list link;
	struct clipboard_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct clipboard_toplevel {
	struct wl_list link;
	struct clipboard_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
};

struct clipboard_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct clipboard_keyboard {
	struct wl_list link;
	struct clipboard_server *server;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* Output event handlers (from tinywl) */
static void output_frame(struct wl_listener *listener, void *data) {
	struct clipboard_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene *scene = output->server->scene;

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);

	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct clipboard_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct clipboard_output *output = wl_container_of(listener, output, destroy);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct clipboard_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct clipboard_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout,
		wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
}

/* Focus management (from tinywl) */
static void focus_xwayland_surface(struct clipboard_xwayland_surface *xwayland_surface) {
	if (xwayland_surface == NULL || !xwayland_surface->xsurface ||
		!xwayland_surface->xsurface->surface) {
		return;
	}

	struct clipboard_server *server = xwayland_surface->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = xwayland_surface->xsurface->surface;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;

	if (prev_surface == surface) {
		return;
	}

	// Deactivate previous surface
	if (prev_surface) {
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
		// Also check for XWayland surfaces
		struct wlr_xwayland_surface *prev_xsurface =
			wlr_xwayland_surface_try_from_wlr_surface(prev_surface);
		if (prev_xsurface != NULL) {
			wlr_xwayland_surface_activate(prev_xsurface, false);
		}
	}

	// Activate the XWayland surface
	wlr_xwayland_surface_activate(xwayland_surface->xsurface, true);

	// Raise the XWayland surface to front by moving its scene node
	if (xwayland_surface->scene_surface) {
		wlr_scene_node_raise_to_top(&xwayland_surface->scene_surface->buffer->node);
	}

	// Set keyboard focus
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}

	printf("Focused XWayland surface: %s\n",
		xwayland_surface->xsurface->title ? xwayland_surface->xsurface->title : "unknown");
}

static void focus_toplevel(struct clipboard_toplevel *toplevel) {
	if (toplevel == NULL) {
		return;
	}
	struct clipboard_server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	if (prev_surface == surface) {
		return;
	}
	if (prev_surface) {
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
		// Also check for XWayland surfaces
		struct wlr_xwayland_surface *prev_xsurface =
			wlr_xwayland_surface_try_from_wlr_surface(prev_surface);
		if (prev_xsurface != NULL) {
			wlr_xwayland_surface_activate(prev_xsurface, false);
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

/* Desktop interaction helpers */
static struct clipboard_xwayland_surface *desktop_xwayland_surface_at(
		struct clipboard_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;

	// Check if this scene_surface belongs to an XWayland surface
	if (scene_buffer->node.data) {
		struct clipboard_xwayland_surface *xwayland_surface =
			(struct clipboard_xwayland_surface *)scene_buffer->node.data;
		// Verify this is actually an XWayland surface by checking the structure
		if (xwayland_surface->xsurface && xwayland_surface->scene_surface == scene_surface) {
			return xwayland_surface;
		}
	}

	return NULL;
}

static struct clipboard_toplevel *desktop_toplevel_at(
		struct clipboard_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}

	// Check if tree is NULL before accessing its data
	if (tree == NULL) {
		return NULL;
	}

	return tree->node.data;
}

static void reset_cursor_mode(struct clipboard_server *server) {
	server->cursor_mode = CLIPBOARD_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

static void process_cursor_move(struct clipboard_server *server) {
	struct clipboard_toplevel *toplevel = server->grabbed_toplevel;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - server->grab_x,
		server->cursor->y - server->grab_y);
}

static void process_cursor_motion(struct clipboard_server *server, uint32_t time) {
	if (server->cursor_mode == CLIPBOARD_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	}

	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct clipboard_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!toplevel) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface) {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(seat);
	}
}

/* Cursor event handlers */
static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct clipboard_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
			event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	struct clipboard_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct clipboard_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		reset_cursor_mode(server);
	} else {
		double sx, sy;
		struct wlr_surface *surface = NULL;

		// First check for XWayland surfaces
		struct clipboard_xwayland_surface *xwayland_surface = desktop_xwayland_surface_at(server,
				server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		if (xwayland_surface) {
			focus_xwayland_surface(xwayland_surface);
		} else {
			// Then check for regular toplevel surfaces
			struct clipboard_toplevel *toplevel = desktop_toplevel_at(server,
					server->cursor->x, server->cursor->y, &surface, &sx, &sy);
			focus_toplevel(toplevel);
		}
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct clipboard_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct clipboard_server *server =
		wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* Toplevel event handlers */
static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct clipboard_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
	focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct clipboard_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	if (toplevel == toplevel->server->grabbed_toplevel) {
		reset_cursor_mode(toplevel->server);
	}
	wl_list_remove(&toplevel->link);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct clipboard_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
	if (toplevel->xdg_toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct clipboard_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);

	free(toplevel);
}

static void begin_interactive(struct clipboard_toplevel *toplevel,
		enum clipboard_cursor_mode mode, uint32_t edges) {
	struct clipboard_server *server = toplevel->server;

	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	if (mode == CLIPBOARD_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
	}
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
	struct clipboard_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	begin_interactive(toplevel, CLIPBOARD_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct clipboard_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	begin_interactive(toplevel, CLIPBOARD_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(
		struct wl_listener *listener, void *data) {
	struct clipboard_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {
	struct clipboard_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct clipboard_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct clipboard_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->scene_tree =
		wlr_scene_xdg_surface_create(&toplevel->server->scene->tree, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

/* Popup handlers */
static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	struct clipboard_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct clipboard_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;

	struct clipboard_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	assert(parent != NULL);
	struct wlr_scene_tree *parent_tree = parent->data;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* Input handlers */
static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	struct clipboard_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static bool handle_keybinding(struct clipboard_server *server, xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_Escape:
		wl_display_terminate(server->wl_display);
		break;
	case XKB_KEY_F1:
		if (wl_list_length(&server->toplevels) < 2) {
			break;
		}
		struct clipboard_toplevel *next_toplevel =
			wl_container_of(server->toplevels.prev, next_toplevel, link);
		focus_toplevel(next_toplevel);
		break;
	default:
		return false;
	}
	return true;
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	struct clipboard_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct clipboard_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;

	// First check if dialog is visible and handle Y/N keys
	if (server->dialog_visible && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			if (syms[i] == XKB_KEY_y || syms[i] == XKB_KEY_Y) {
				handle_dialog_response(server, true);
				handled = true;
				break;
			} else if (syms[i] == XKB_KEY_n || syms[i] == XKB_KEY_N) {
				handle_dialog_response(server, false);
				handled = true;
				break;
			}
		}
	}

	if (!handled) {
		uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
		if ((modifiers & WLR_MODIFIER_ALT) &&
				event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			for (int i = 0; i < nsyms; i++) {
				handled = handle_keybinding(server, syms[i]);
			}
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct clipboard_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct clipboard_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct clipboard_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct clipboard_server *server,
		struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct clipboard_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct clipboard_server *server = wl_container_of(
			listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct clipboard_server *server = wl_container_of(
			listener, server, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		case 'h':
			printf("Clipboard Control Compositor\n");
			printf("Usage: %s [-s startup command]\n\n", argv[0]);
			printf("Options:\n");
			printf("  -s <command>  Execute command after starting the compositor\n");
			printf("  -h			Show this help message\n\n");
			printf("Controls:\n");
			printf("  Alt+Esc	   Exit compositor\n");
			printf("  Alt+F1		Switch between windows\n");
			printf("  Mouse		 Click and drag to move windows\n\n");
			printf("Examples:\n");
			printf("  %s									# Start without applications\n", argv[0]);
			printf("  %s -s \"weston-terminal\"			   # Start with terminal\n", argv[0]);
			printf("  %s -s \"weston-terminal & gedit\"	  # Start with multiple apps\n", argv[0]);
			return 0;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			printf("Use -h for detailed help.\n");
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

	printf("Starting clipboard control compositor...\n");
	printf("This compositor will show approval dialogs for all clipboard operations.\n\n");

	struct clipboard_server server = {0};

	server.wl_display = wl_display_create();
	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	/* Create compositor protocols */
	server.compositor = wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);
	server.primary_selection_manager =
		wlr_primary_selection_v1_device_manager_create(server.wl_display);

	server.output_layout = wlr_output_layout_create(server.wl_display);

	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

	wl_list_init(&server.toplevels);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	server.cursor_mode = CLIPBOARD_CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);
	server.pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
			&server.pointer_focus_change);

	/* Set up clipboard control */
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
			&server.request_set_selection);
	server.request_set_primary_selection.notify = seat_request_set_primary_selection;
	wl_signal_add(&server.seat->events.request_set_primary_selection,
			&server.request_set_primary_selection);
	server.request_start_drag.notify = seat_request_start_drag;
	wl_signal_add(&server.seat->events.request_start_drag,
			&server.request_start_drag);
	server.start_drag.notify = seat_start_drag;
	wl_signal_add(&server.seat->events.start_drag,
			&server.start_drag);
	wl_list_init(&server.pending_requests);

	/* Initialize wrapper caches */
	wl_list_init(&server.active_data_source_wrappers);
	wl_list_init(&server.active_primary_source_wrappers);

	/* Initialize dialog system */
	server.dialog_visible = false;
	server.dialog_buffer = NULL;
	server.current_request = NULL;
	server.dialog_wlr_buffer = NULL;

	/* Create XWayland instance (non-lazy for this example) */
	server.xwayland = wlr_xwayland_create(server.wl_display, server.compositor, false);
	if (!server.xwayland) {
		wlr_log(WLR_ERROR, "Cannot create XWayland server");
		return 1;
	}

	/* Set the seat for XWayland - required for clipboard operations */
	wlr_xwayland_set_seat(server.xwayland, server.seat);
	printf("Set seat for XWayland server\n");

	/* Set up XWayland surface listener */
	server.xwayland_new_surface.notify = server_new_xwayland_surface;
	wl_signal_add(&server.xwayland->events.new_surface, &server.xwayland_new_surface);

	/* Store startup command and set up XWayland ready listener */
	server.startup_cmd = startup_cmd;
	server.xwayland_ready.notify = xwayland_ready;
	wl_signal_add(&server.xwayland->events.ready, &server.xwayland_ready);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);

	printf("Running clipboard control compositor on WAYLAND_DISPLAY=%s\n", socket);
	if (startup_cmd) {
		printf("Waiting for XWayland to be ready before starting: %s\n", startup_cmd);
	}
	printf("All clipboard operations will require approval.\n");
	printf("Controls:\n");
	printf("  Alt+Esc: Exit compositor\n");
	printf("  Alt+F1:  Switch between windows\n");
	printf("  Mouse:   Click and drag to move windows\n");
	printf("Press Ctrl+C to exit.\n\n");

	wl_display_run(server.wl_display);

	/* Cleanup */
	wl_display_destroy_clients(server.wl_display);

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);

	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);

	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.pointer_focus_change.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.request_set_primary_selection.link);
	wl_list_remove(&server.request_start_drag.link);
	wl_list_remove(&server.start_drag.link);

	wl_list_remove(&server.new_output.link);

	if (server.xwayland) {
		wl_list_remove(&server.xwayland_ready.link);
		wl_list_remove(&server.xwayland_new_surface.link);
		wlr_xwayland_destroy(server.xwayland);
	}

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);

	return 0;
}
