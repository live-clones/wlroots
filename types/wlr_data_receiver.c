#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/types/wlr_data_receiver.h>
#include <wlr/util/log.h>

void wlr_data_receiver_init(struct wlr_data_receiver *receiver,
		const struct wlr_data_receiver_impl *impl) {
	assert(receiver && impl);
	*receiver = (struct wlr_data_receiver){
		.impl = impl,
		.fd = -1,
		.pid = 0,
		.client = NULL,
	};
	wl_signal_init(&receiver->events.destroy);
}

void wlr_data_receiver_destroy(struct wlr_data_receiver *receiver) {
	if (!receiver) {
		return;
	}

	wl_signal_emit_mutable(&receiver->events.destroy, receiver);
	assert(wl_list_empty(&receiver->events.destroy.listener_list));

	int fd = receiver->fd;

	if (receiver->impl && receiver->impl->destroy) {
		receiver->impl->destroy(receiver);
	} else {
		free(receiver);
	}

	if (fd >= 0 && fcntl(fd, F_GETFD) != -1) {
		close(fd);
	}
}

void wlr_data_receiver_cancelled(struct wlr_data_receiver *receiver) {
	if (!receiver) {
		return;
	}

	if (receiver->impl && receiver->impl->cancelled) {
		receiver->impl->cancelled(receiver);
	}

	// Close fd if still valid
	if (receiver->fd >= 0 && fcntl(receiver->fd, F_GETFD) != -1) {
		close(receiver->fd);
		receiver->fd = -1;
	}
}
