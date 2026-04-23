#include <assert.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <pixman.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_viewporter.h>
#include "util/shm.h"
#include "viewporter-client-protocol.h"

#define BUF_WIDTH 24
#define BUF_HEIGHT 12
#define BUF_STRIDE (BUF_WIDTH * 4)
#define BUF_SIZE (BUF_STRIDE * BUF_HEIGHT)

struct test_context {
	struct wl_display *server_display;
	struct wlr_compositor *compositor;
	struct wlr_shm *shm;
	struct wlr_viewporter *viewporter;
	struct wlr_surface *wlr_surface;
	struct wl_listener new_surface;

	struct wl_display *client_display;
	struct wl_compositor *client_compositor;
	struct wl_shm *client_shm;
	struct wp_viewporter *client_viewporter;
	struct wl_surface *client_surface;
	struct wl_buffer *client_buffer;
};

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct test_context *ctx =
		wl_container_of(listener, ctx, new_surface);
	ctx->wlr_surface = data;
}

static void registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct test_context *ctx = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		ctx->client_compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, version);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		ctx->client_shm = wl_registry_bind(registry, name,
			&wl_shm_interface, version);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		ctx->client_viewporter = wl_registry_bind(registry, name,
			&wp_viewporter_interface, version);
	}
}

static void registry_handle_global_remove(void *data,
		struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static void client_roundtrip(struct test_context *ctx) {
	assert(wl_display_flush(ctx->client_display) >= 0);
	assert(wl_event_loop_dispatch(
		wl_display_get_event_loop(ctx->server_display), -1) == 0);
}

static void full_roundtrip(struct test_context *ctx) {
	assert(wl_display_flush(ctx->client_display) >= 0);
	assert(wl_event_loop_dispatch(
		wl_display_get_event_loop(ctx->server_display), -1) == 0);
	wl_display_flush_clients(ctx->server_display);
	assert(wl_display_dispatch(ctx->client_display) != -1);
}

static bool region_is_empty(pixman_region32_t *region) {
	return !pixman_region32_not_empty(region);
}

static bool region_is_rect(pixman_region32_t *region,
		int x, int y, int w, int h) {
	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(region, &nrects);
	return nrects == 1 &&
		rects[0].x1 == x && rects[0].y1 == y &&
		rects[0].x2 == x + w && rects[0].y2 == y + h;
}

static void setup(struct test_context *ctx) {
	memset(ctx, 0, sizeof(*ctx));

	ctx->server_display = wl_display_create();
	assert(ctx->server_display);
	ctx->compositor = wlr_compositor_create(ctx->server_display, 6, NULL);
	assert(ctx->compositor);
	uint32_t fmts[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888 };
	ctx->shm = wlr_shm_create(ctx->server_display, 1, fmts, 2);
	assert(ctx->shm);
	ctx->viewporter = wlr_viewporter_create(ctx->server_display);
	assert(ctx->viewporter);

	ctx->new_surface.notify = handle_new_surface;
	wl_signal_add(&ctx->compositor->events.new_surface, &ctx->new_surface);

	int sv[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	assert(fcntl(sv[0], F_SETFD, FD_CLOEXEC) == 0);
	assert(fcntl(sv[1], F_SETFD, FD_CLOEXEC) == 0);
	assert(wl_client_create(ctx->server_display, sv[0]));

	ctx->client_display = wl_display_connect_to_fd(sv[1]);
	assert(ctx->client_display);

	struct wl_registry *registry =
		wl_display_get_registry(ctx->client_display);
	wl_registry_add_listener(registry, &registry_listener, ctx);
	full_roundtrip(ctx);
	assert(ctx->client_compositor);
	assert(ctx->client_shm);
	assert(ctx->client_viewporter);

	ctx->client_surface =
		wl_compositor_create_surface(ctx->client_compositor);
	assert(ctx->client_surface);
	client_roundtrip(ctx);
	assert(ctx->wlr_surface);

	int fd = allocate_shm_file(BUF_SIZE);
	assert(fd >= 0);
	struct wl_shm_pool *pool =
		wl_shm_create_pool(ctx->client_shm, fd, BUF_SIZE);
	assert(pool);
	ctx->client_buffer = wl_shm_pool_create_buffer(pool, 0,
		BUF_WIDTH, BUF_HEIGHT, BUF_STRIDE, WL_SHM_FORMAT_ARGB8888);
	assert(ctx->client_buffer);
	wl_shm_pool_destroy(pool);
	close(fd);

	wl_registry_destroy(registry);
}

static void teardown(struct test_context *ctx) {
	wl_list_remove(&ctx->new_surface.link);
	wl_buffer_destroy(ctx->client_buffer);
	wl_surface_destroy(ctx->client_surface);
	wp_viewporter_destroy(ctx->client_viewporter);
	wl_compositor_destroy(ctx->client_compositor);
	wl_shm_destroy(ctx->client_shm);
	wl_display_disconnect(ctx->client_display);
	wl_display_destroy_clients(ctx->server_display);
	wl_display_destroy(ctx->server_display);
}

static void test_surface_damage(void) {
	struct test_context ctx;
	setup(&ctx);

	wl_surface_attach(ctx.client_surface, ctx.client_buffer, 0, 0);
	wl_surface_set_buffer_scale(ctx.client_surface, 2);
	wl_surface_damage(ctx.client_surface, 0, 0, 6, 3);
	wl_surface_commit(ctx.client_surface);
	client_roundtrip(&ctx);

	assert(region_is_rect(&ctx.wlr_surface->buffer_damage, 0, 0, 12, 6));

	teardown(&ctx);
}

static void test_buffer_damage(void) {
	struct test_context ctx;
	setup(&ctx);

	wl_surface_attach(ctx.client_surface, ctx.client_buffer, 0, 0);
	wl_surface_set_buffer_scale(ctx.client_surface, 2);
	wl_surface_damage_buffer(ctx.client_surface, 0, 0, 10, 5);
	wl_surface_commit(ctx.client_surface);
	client_roundtrip(&ctx);

	assert(region_is_rect(&ctx.wlr_surface->buffer_damage, 0, 0, 10, 5));

	teardown(&ctx);
}

static void test_surface_damage_transform(void) {
	struct test_context ctx;
	setup(&ctx);

	wl_surface_attach(ctx.client_surface, ctx.client_buffer, 0, 0);
	wl_surface_set_buffer_transform(ctx.client_surface,
		WL_OUTPUT_TRANSFORM_90);
	wl_surface_damage(ctx.client_surface, 0, 0, 6, 12);
	wl_surface_commit(ctx.client_surface);
	client_roundtrip(&ctx);

	assert(region_is_rect(&ctx.wlr_surface->buffer_damage, 0, 6, 12, 6));

	teardown(&ctx);
}

static void test_surface_damage_viewport(void) {
	struct test_context ctx;
	setup(&ctx);

	struct wp_viewport *viewport = wp_viewporter_get_viewport(
		ctx.client_viewporter, ctx.client_surface);

	wl_surface_attach(ctx.client_surface, ctx.client_buffer, 0, 0);
	wp_viewport_set_destination(viewport, 6, 3);
	wl_surface_damage(ctx.client_surface, 0, 0, 3, 3);
	wl_surface_commit(ctx.client_surface);
	client_roundtrip(&ctx);

	assert(region_is_rect(&ctx.wlr_surface->buffer_damage, 0, 0, 12, 12));

	wp_viewport_destroy(viewport);
	teardown(&ctx);
}

static void test_surface_damage_all(void) {
	struct test_context ctx;
	setup(&ctx);

	struct wp_viewport *viewport = wp_viewporter_get_viewport(
		ctx.client_viewporter, ctx.client_surface);

	wl_surface_attach(ctx.client_surface, ctx.client_buffer, 0, 0);
	wl_surface_set_buffer_scale(ctx.client_surface, 2);
	wl_surface_set_buffer_transform(ctx.client_surface,
		WL_OUTPUT_TRANSFORM_90);
	wp_viewport_set_destination(viewport, 3, 6);
	wl_surface_damage(ctx.client_surface, 0, 0, 1, 2);
	wl_surface_commit(ctx.client_surface);
	client_roundtrip(&ctx);

	assert(region_is_rect(&ctx.wlr_surface->buffer_damage, 0, 8, 8, 4));

	wp_viewport_destroy(viewport);
	teardown(&ctx);
}

static void test_cached_surface_damage(void) {
	struct test_context ctx;
	setup(&ctx);

	struct wp_viewport *viewport = wp_viewporter_get_viewport(
		ctx.client_viewporter, ctx.client_surface);

	wl_surface_attach(ctx.client_surface, ctx.client_buffer, 0, 0);
	wl_surface_set_buffer_scale(ctx.client_surface, 2);
	wl_surface_set_buffer_transform(ctx.client_surface,
		WL_OUTPUT_TRANSFORM_90);
	wp_viewport_set_destination(viewport, 3, 6);
	wl_surface_commit(ctx.client_surface);
	client_roundtrip(&ctx);

	assert(ctx.wlr_surface->current.scale == 2);
	assert(region_is_empty(&ctx.wlr_surface->buffer_damage));

	uint32_t seq = wlr_surface_lock_pending(ctx.wlr_surface);

	wl_surface_damage(ctx.client_surface, 0, 0, 1, 2);
	wl_surface_commit(ctx.client_surface);
	client_roundtrip(&ctx);

	assert(region_is_empty(&ctx.wlr_surface->buffer_damage));

	wlr_surface_unlock_cached(ctx.wlr_surface, seq);
	assert(region_is_rect(&ctx.wlr_surface->buffer_damage, 0, 8, 8, 4));

	wp_viewport_destroy(viewport);
	teardown(&ctx);
}

int main(void) {
#ifdef NDEBUG
	fprintf(stderr, "NDEBUG must be disabled for tests\n");
	return 1;
#endif

	test_surface_damage();
	test_buffer_damage();
	test_surface_damage_transform();
	test_surface_damage_viewport();
	test_surface_damage_all();
	test_cached_surface_damage();
	return 0;
}
