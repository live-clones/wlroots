#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>

struct surface {
	struct wl_surface *client;
	struct wl_subsurface *subsurface;
	struct wlr_surface *server;
};

struct fixture {
	struct wl_display *server_display, *client_display;
	struct wl_client *server_client;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct surface surfaces[8];
	size_t surfaces_len;
};

static void handle_sync_done(void *data, struct wl_callback *callback, uint32_t serial) {
	bool *done = data;
	*done = true;
	wl_callback_destroy(callback);
}

static const struct wl_callback_listener sync_listener = {
	.done = handle_sync_done,
};

static void roundtrip(struct fixture *fixture) {
	bool done = false;
	struct wl_callback *callback = wl_display_sync(fixture->client_display);
	assert(callback != NULL);
	wl_callback_add_listener(callback, &sync_listener, &done);
	assert(wl_display_flush(fixture->client_display) >= 0);
	struct wl_event_loop *loop = wl_display_get_event_loop(fixture->server_display);
	assert(wl_event_loop_dispatch(loop, 0) >= 0);
	wl_display_flush_clients(fixture->server_display);
	while (!done) {
		assert(wl_display_dispatch(fixture->client_display) >= 0);
	}
}

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct fixture *fixture = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		fixture->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		fixture->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static void fixture_init(struct fixture *fixture) {
	*fixture = (struct fixture){0};
	fixture->server_display = wl_display_create();
	assert(fixture->server_display != NULL);
	assert(wlr_compositor_create(fixture->server_display, 4, NULL) != NULL);
	assert(wlr_subcompositor_create(fixture->server_display) != NULL);

	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	fixture->server_client = wl_client_create(fixture->server_display, sockets[0]);
	assert(fixture->server_client != NULL);
	fixture->client_display = wl_display_connect_to_fd(sockets[1]);
	assert(fixture->client_display != NULL);
	fixture->registry = wl_display_get_registry(fixture->client_display);
	wl_registry_add_listener(fixture->registry, &registry_listener, fixture);
	roundtrip(fixture);
	assert(fixture->compositor != NULL && fixture->subcompositor != NULL);
}

static void fixture_finish(struct fixture *fixture) {
	for (size_t i = fixture->surfaces_len; i > 0; i--) {
		struct surface *surface = &fixture->surfaces[i - 1];
		if (surface->subsurface != NULL) {
			wl_subsurface_destroy(surface->subsurface);
		}
		if (surface->client != NULL) {
			wl_surface_destroy(surface->client);
		}
	}
	wl_subcompositor_destroy(fixture->subcompositor);
	wl_compositor_destroy(fixture->compositor);
	wl_registry_destroy(fixture->registry);
	roundtrip(fixture);
	wl_display_disconnect(fixture->client_display);
	wl_display_destroy_clients(fixture->server_display);
	wl_display_destroy(fixture->server_display);
}

static struct surface *surface_create(struct fixture *fixture, struct surface *parent,
		bool below) {
	assert(fixture->surfaces_len < sizeof(fixture->surfaces) / sizeof(fixture->surfaces[0]));
	struct surface *surface = &fixture->surfaces[fixture->surfaces_len++];
	surface->client = wl_compositor_create_surface(fixture->compositor);
	assert(surface->client != NULL);
	if (parent != NULL) {
		surface->subsurface = wl_subcompositor_get_subsurface(fixture->subcompositor,
			surface->client, parent->client);
		assert(surface->subsurface != NULL);
		if (below) {
			wl_subsurface_place_below(surface->subsurface, parent->client);
		}
	}
	roundtrip(fixture);
	struct wl_resource *resource = wl_client_get_object(fixture->server_client,
		wl_proxy_get_id((struct wl_proxy *)surface->client));
	assert(resource != NULL);
	surface->server = wlr_surface_from_resource(resource);
	assert(surface->server->current.scale == 1);
	return surface;
}

// Buffer scale is double-buffered surface state, so no renderer or buffers are
// needed to observe whether a commit has been applied.
static void surface_commit_scale(struct surface *surface, int scale) {
	wl_surface_set_buffer_scale(surface->client, scale);
	wl_surface_commit(surface->client);
}

static void test_parent_commit(struct fixture *fixture, bool below) {
	struct surface *root = surface_create(fixture, NULL, below);
	struct surface *child = surface_create(fixture, root, below);
	struct surface *grandchild = surface_create(fixture, child, below);
	wl_subsurface_set_desync(grandchild->subsurface);

	surface_commit_scale(grandchild, 2);
	surface_commit_scale(grandchild, 3);
	surface_commit_scale(child, 2);
	roundtrip(fixture);
	assert(child->server->current.scale == 1);
	assert(grandchild->server->current.scale == 1);

	wl_surface_commit(root->client);
	roundtrip(fixture);
	assert(child->server->current.scale == 2);
	assert(grandchild->server->current.scale == 3);
}

static void test_ancestor_desync(struct fixture *fixture, bool below) {
	struct surface *root = surface_create(fixture, NULL, below);
	struct surface *child = surface_create(fixture, root, below);
	struct surface *grandchild = surface_create(fixture, child, below);
	struct surface *leaf = surface_create(fixture, grandchild, below);
	wl_subsurface_set_desync(grandchild->subsurface);
	wl_subsurface_set_desync(leaf->subsurface);
	surface_commit_scale(leaf, 2);
	roundtrip(fixture);
	assert(leaf->server->current.scale == 1);

	// Neither intermediate surface has committed: their children are only in
	// the pending lists, and they have no cached state of their own to unlock.
	wl_subsurface_set_desync(child->subsurface);
	roundtrip(fixture);
	assert(leaf->server->current.scale == 2);
}

static void test_ancestor_desync_cached(struct fixture *fixture, bool below) {
	struct surface *root = surface_create(fixture, NULL, below);
	struct surface *child = surface_create(fixture, root, below);
	struct surface *grandchild = surface_create(fixture, child, below);
	surface_commit_scale(grandchild, 2);
	surface_commit_scale(child, 2);
	surface_commit_scale(grandchild, 3);
	wl_subsurface_set_desync(grandchild->subsurface);
	roundtrip(fixture);
	assert(child->server->current.scale == 1);
	assert(grandchild->server->current.scale == 1);

	wl_subsurface_set_desync(child->subsurface);
	roundtrip(fixture);
	assert(child->server->current.scale == 2);
	assert(grandchild->server->current.scale == 3);
}

static void test_synchronized_ancestor(struct fixture *fixture, bool below) {
	struct surface *root = surface_create(fixture, NULL, below);
	struct surface *child = surface_create(fixture, root, below);
	struct surface *grandchild = surface_create(fixture, child, below);
	struct surface *leaf = surface_create(fixture, grandchild, below);
	wl_subsurface_set_desync(leaf->subsurface);
	surface_commit_scale(leaf, 2);
	surface_commit_scale(grandchild, 2);
	wl_subsurface_set_desync(grandchild->subsurface);
	roundtrip(fixture);
	assert(grandchild->server->current.scale == 1);
	assert(leaf->server->current.scale == 1);

	surface_commit_scale(child, 2);
	wl_surface_commit(root->client);
	roundtrip(fixture);
	assert(grandchild->server->current.scale == 2);
	assert(leaf->server->current.scale == 2);
}

static void test_synchronized_descendant(struct fixture *fixture, bool below) {
	struct surface *root = surface_create(fixture, NULL, below);
	struct surface *child = surface_create(fixture, root, below);
	struct surface *grandchild = surface_create(fixture, child, below);
	struct surface *leaf = surface_create(fixture, grandchild, below);
	wl_subsurface_set_desync(leaf->subsurface);
	surface_commit_scale(leaf, 2);
	surface_commit_scale(grandchild, 2);
	wl_subsurface_set_desync(child->subsurface);
	roundtrip(fixture);
	assert(grandchild->server->current.scale == 1);
	assert(leaf->server->current.scale == 1);

	wl_surface_commit(child->client);
	roundtrip(fixture);
	assert(grandchild->server->current.scale == 2);
	assert(leaf->server->current.scale == 2);
}

static void test_ancestor_destroy(struct fixture *fixture, bool below) {
	struct surface *root = surface_create(fixture, NULL, below);
	struct surface *child = surface_create(fixture, root, below);
	struct surface *grandchild = surface_create(fixture, child, below);
	wl_subsurface_set_desync(grandchild->subsurface);
	surface_commit_scale(grandchild, 2);
	roundtrip(fixture);
	assert(grandchild->server->current.scale == 1);

	wl_subsurface_destroy(child->subsurface);
	child->subsurface = NULL;
	roundtrip(fixture);
	assert(grandchild->server->current.scale == 2);
}

static void test_parent_destroy(struct fixture *fixture, bool below) {
	struct surface *root = surface_create(fixture, NULL, below);
	struct surface *child = surface_create(fixture, root, below);
	struct surface *grandchild = surface_create(fixture, child, below);
	wl_subsurface_set_desync(grandchild->subsurface);
	surface_commit_scale(grandchild, 2);
	roundtrip(fixture);
	assert(grandchild->server->current.scale == 1);

	wl_surface_destroy(root->client);
	root->client = NULL;
	root->server = NULL;
	roundtrip(fixture);
	assert(grandchild->server->current.scale == 2);
}

static void test_external_lock(struct fixture *fixture, bool below) {
	struct surface *root = surface_create(fixture, NULL, below);
	struct surface *child = surface_create(fixture, root, below);
	struct surface *grandchild = surface_create(fixture, child, below);
	wl_subsurface_set_desync(grandchild->subsurface);
	uint32_t seq = wlr_surface_lock_pending(grandchild->server);
	surface_commit_scale(grandchild, 2);
	surface_commit_scale(child, 2);
	wl_surface_commit(root->client);
	roundtrip(fixture);
	assert(grandchild->server->current.scale == 1);

	// Releasing the subsurface's lock must preserve a compositor's own lock.
	wlr_surface_unlock_cached(grandchild->server, seq);
	assert(grandchild->server->current.scale == 2);

	wl_subsurface_set_sync(child->subsurface);
	seq = wlr_surface_lock_pending(grandchild->server);
	surface_commit_scale(grandchild, 3);
	wl_subsurface_set_desync(child->subsurface);
	roundtrip(fixture);
	assert(grandchild->server->current.scale == 2);
	wlr_surface_unlock_cached(grandchild->server, seq);
	assert(grandchild->server->current.scale == 3);
}

static void test_direct_modes(struct fixture *fixture, bool below) {
	struct surface *root = surface_create(fixture, NULL, below);
	struct surface *child = surface_create(fixture, root, below);
	surface_commit_scale(child, 2);
	roundtrip(fixture);
	assert(child->server->current.scale == 1);
	wl_surface_commit(root->client);
	roundtrip(fixture);
	assert(child->server->current.scale == 2);

	surface_commit_scale(child, 3);
	wl_subsurface_set_desync(child->subsurface);
	roundtrip(fixture);
	assert(child->server->current.scale == 3);
	surface_commit_scale(child, 4);
	roundtrip(fixture);
	assert(child->server->current.scale == 4);
}

static const struct {
	const char *name;
	void (*run)(struct fixture *fixture, bool below);
} tests[] = {
	{ "parent-commit", test_parent_commit },
	{ "ancestor-desync", test_ancestor_desync },
	{ "ancestor-desync-cached", test_ancestor_desync_cached },
	{ "synchronized-ancestor", test_synchronized_ancestor },
	{ "synchronized-descendant", test_synchronized_descendant },
	{ "ancestor-destroy", test_ancestor_destroy },
	{ "parent-destroy", test_parent_destroy },
	{ "external-lock", test_external_lock },
	{ "direct-modes", test_direct_modes },
};

int main(int argc, char *argv[]) {
#ifdef NDEBUG
	fprintf(stderr, "NDEBUG must be disabled for tests\n");
	return 1;
#endif
	assert(argc == 2);
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (strcmp(argv[1], tests[i].name) != 0) {
			continue;
		}
		for (size_t j = 0; j < 2; j++) {
			struct fixture fixture;
			fixture_init(&fixture);
			tests[i].run(&fixture, j != 0);
			fixture_finish(&fixture);
		}
		return 0;
	}
	fprintf(stderr, "Unknown test: %s\n", argv[1]);
	return 1;
}
