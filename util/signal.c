#include "util/signal.h"

void wlr_signal_emit_final(struct wl_signal *signal, void *data) {

	// We need to run all listeners one final time. To support all types of list mutations and to
	// ensure that all listeners including those added during this execution is run, we run until
	// the list is empty, removing listeners just before we run them. To not affect the behavior of
	// the listener, we re-initialize the listener's link element.
	while (signal->listener_list.next != &signal->listener_list) {
		struct wl_list *pos = signal->listener_list.next;
		struct wl_listener *l = wl_container_of(pos, l, link);

		wl_list_remove(pos);
		wl_list_init(pos);

		l->notify(l, data);
	}
}
