#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/util/log.h>

void wlr_primary_selection_source_init(
		struct wlr_primary_selection_source *source,
		const struct wlr_primary_selection_source_impl *impl) {
	assert(impl->send);
	*source = (struct wlr_primary_selection_source){
		.impl = impl,
	};
	wl_array_init(&source->mime_types);

	wl_signal_init(&source->events.destroy);
}

void wlr_primary_selection_source_destroy(
		struct wlr_primary_selection_source *source) {
	if (source == NULL) {
		return;
	}

	wl_signal_emit_mutable(&source->events.destroy, source);

	assert(wl_list_empty(&source->events.destroy.listener_list));

	char **p;
	wl_array_for_each(p, &source->mime_types) {
		free(*p);
	}
	wl_array_release(&source->mime_types);

	if (source->impl->destroy) {
		source->impl->destroy(source);
	} else {
		free(source);
	}
}

void wlr_primary_selection_source_send(
		struct wlr_primary_selection_source *source,
		const char *mime_type, struct wlr_data_receiver *receiver) {
	source->impl->send(source, mime_type, receiver);
}

void wlr_primary_selection_source_copy(struct wlr_primary_selection_source *dest,
		struct wlr_primary_selection_source *src) {
	if (dest == NULL || src == NULL) {
		return;
	}

	dest->client = src->client;
	dest->pid = src->pid;

	/* Clear any existing mime types before copying */
	if (dest->mime_types.size > 0) {
		char **p;
		wl_array_for_each(p, &dest->mime_types) {
			free(*p);
		}
		wl_array_release(&dest->mime_types);
		wl_array_init(&dest->mime_types);
	}

	// Copy MIME types from source
	char **p;
	wl_array_for_each(p, &src->mime_types) {
		char **dest_p = wl_array_add(&dest->mime_types, sizeof(*dest_p));
		if (dest_p == NULL) {
			wlr_log(WLR_ERROR, "Failed to add MIME type to destination");
			continue;
		}

		char *mime_type_copy = strdup(*p);
		if (mime_type_copy == NULL) {
			wlr_log(WLR_ERROR, "Failed to copy MIME type");
			continue;
		}

		*dest_p = mime_type_copy;
	}
}

struct wlr_primary_selection_source *wlr_primary_selection_source_get_original(
		struct wlr_primary_selection_source *source) {
	if (!source) {
		return NULL;
	}

	if (source->impl && source->impl->get_original) {
		return source->impl->get_original(source);
	}

	return source;
}

void wlr_seat_request_set_primary_selection(struct wlr_seat *seat,
		struct wlr_seat_client *client,
		struct wlr_primary_selection_source *source, uint32_t serial) {
	if (client && !wlr_seat_client_validate_event_serial(client, serial)) {
		wlr_log(WLR_DEBUG, "Rejecting set_primary_selection request, "
			"serial %"PRIu32" was never given to client", serial);
		return;
	}

	if (seat->primary_selection_source &&
			serial - seat->primary_selection_serial > UINT32_MAX / 2) {
		wlr_log(WLR_DEBUG, "Rejecting set_primary_selection request, "
			"serial indicates superseded (%"PRIu32" < %"PRIu32")",
			serial, seat->primary_selection_serial);
		return;
	}

	struct wlr_seat_request_set_primary_selection_event event = {
		.source = source,
		.serial = serial,
	};
	wl_signal_emit_mutable(&seat->events.request_set_primary_selection, &event);
}

static void seat_handle_primary_selection_source_destroy(
		struct wl_listener *listener, void *data) {
	struct wlr_seat *seat =
		wl_container_of(listener, seat, primary_selection_source_destroy);
	wl_list_remove(&seat->primary_selection_source_destroy.link);
	seat->primary_selection_source = NULL;
	wl_signal_emit_mutable(&seat->events.set_primary_selection, seat);
}

void wlr_seat_set_primary_selection(struct wlr_seat *seat,
		struct wlr_primary_selection_source *source, uint32_t serial) {
	if (seat->primary_selection_source == source) {
		seat->primary_selection_serial = serial;
		return;
	}

	if (seat->primary_selection_source != NULL) {
		wl_list_remove(&seat->primary_selection_source_destroy.link);
		wlr_primary_selection_source_destroy(seat->primary_selection_source);
		seat->primary_selection_source = NULL;
	}

	seat->primary_selection_source = source;
	seat->primary_selection_serial = serial;

	if (source != NULL) {
		seat->primary_selection_source_destroy.notify =
			seat_handle_primary_selection_source_destroy;
		wl_signal_add(&source->events.destroy,
			&seat->primary_selection_source_destroy);
	}

	wl_signal_emit_mutable(&seat->events.set_primary_selection, seat);
}
