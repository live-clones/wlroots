#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_dbg_txn.h>

struct wlr_dbg_txn {
	struct wl_resource *resource;
	struct wl_list locks;
};

struct wlr_dbg_txn_lock {
	struct wlr_addon addon;
	struct wlr_surface_state_lock lock;
	struct wl_list link;
};

static const struct dbg_txn_interface txn_impl;

static struct wlr_dbg_txn *txn_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &dbg_txn_interface, &txn_impl));
	return wl_resource_get_user_data(resource);
}

static void txn_lock_destroy(struct wlr_dbg_txn_lock *lock) {
	wlr_surface_state_lock_release(&lock->lock);
	wl_list_remove(&lock->link);
	wlr_addon_finish(&lock->addon);
	free(lock);
}

static void addon_handle_destroy(struct wlr_addon *addon) {
	struct wlr_dbg_txn_lock *lock = wl_container_of(addon, lock, addon);
	txn_lock_destroy(lock);
}

static struct wlr_addon_interface addon_impl = {
	.name = "wlr_dbg_txn_lock",
	.destroy = addon_handle_destroy,
};

static void txn_handle_add_surface(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (wlr_addon_find(&surface->addons, NULL, &addon_impl) != NULL) {
		wl_resource_post_error(resource, -1, "already added");
		return;
	}

	struct wlr_dbg_txn *txn = txn_from_resource(resource);
	struct wlr_dbg_txn_lock *lock = calloc(1, sizeof(*lock));
	assert(lock != NULL);
	wlr_surface_state_lock_acquire(&lock->lock, surface);
	wl_list_insert(&txn->locks, &lock->link);
	wlr_addon_init(&lock->addon, &surface->addons, NULL, &addon_impl);
}

static void txn_handle_commit(struct wl_client *client, struct wl_resource *resource) {
	struct wlr_dbg_txn *txn = txn_from_resource(resource);

	struct wl_array buffer;
	wl_array_init(&buffer);

	struct wlr_surface_transaction surface_txn;
	wlr_surface_transaction_init(&surface_txn, &buffer);

	bool ok = true;
	struct wlr_dbg_txn_lock *lock;
	wl_list_for_each(lock, &txn->locks, link) {
		if (!wlr_surface_transaction_add_lock(&surface_txn, &lock->lock)) {
			wlr_surface_transaction_drop(&surface_txn);
			ok = false;
			break;
		}
	}
	if (ok) {
		ok = wlr_surface_transaction_commit(&surface_txn);
	}
	if (!ok) {
		wl_resource_post_no_memory(resource);
	}

	wl_array_release(&buffer);
	wl_resource_destroy(resource);
}

static const struct dbg_txn_interface txn_impl = {
	.add_surface = txn_handle_add_surface,
	.commit = txn_handle_commit,
};

static void txn_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_dbg_txn *txn = txn_from_resource(resource);
	struct wlr_dbg_txn_lock *lock, *tmp;
	wl_list_for_each_safe(lock, tmp, &txn->locks, link) {
		txn_lock_destroy(lock);
	}
	free(txn);
}

static void manager_handle_get_txn(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	struct wlr_dbg_txn *txn = calloc(1, sizeof(*txn));
	if (txn == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	txn->resource = wl_resource_create(client, &dbg_txn_interface, 1, id);
	assert(txn->resource != NULL);

	wl_resource_set_implementation(txn->resource, &txn_impl, txn, txn_handle_resource_destroy);

	wl_list_init(&txn->locks);
}

static const struct dbg_txn_manager_interface manager_impl = {
	.get_txn = manager_handle_get_txn,
};

static void manager_bind(struct wl_client *wl_client, void *data, uint32_t version, uint32_t id) {
	struct wlr_dbg_txn_manager *manager = data;

	struct wl_resource *resource =
		wl_resource_create(wl_client, &wl_compositor_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

static void manager_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_dbg_txn_manager *manager = wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_dbg_txn_manager *wlr_dbg_txn_manager_create(struct wl_display *display) {
	struct wlr_dbg_txn_manager *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		return NULL;
	}

	manager->global = wl_global_create(display, &dbg_txn_manager_interface,
		1, manager, manager_bind);
	if (!manager->global) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
