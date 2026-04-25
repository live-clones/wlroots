#ifndef RENDER_PASS_EXT_H
#define RENDER_PASS_EXT_H

#include <stddef.h>

#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>

#include <wayland-server-core.h>

struct custom_render_triangle_options {
	struct wlr_box box;
};

struct render_triangle_pass;

struct render_triangle_pass_impl {
	void (*destroy)(struct render_triangle_pass *pass);
	void (*render)(struct wlr_render_pass *render_pass,
		const struct custom_render_triangle_options *options);
};

struct render_triangle_pass {
	const struct render_triangle_pass_impl *impl;
	struct {
		struct wl_signal destroy;
	} events;
};

void render_triangle_pass_init(struct render_triangle_pass *pass,
	const struct render_triangle_pass_impl *impl);
void render_triangle_pass_destroy(struct render_triangle_pass *pass);

struct render_triangle_pass *get_or_create_render_triangle_pass(
	struct wlr_renderer *renderer);
void render_triangle_pass_add(struct wlr_render_pass *render_pass,
	const struct custom_render_triangle_options *options);
void custom_render_triangle_options_get_box(const struct custom_render_triangle_options *options,
	const struct wlr_buffer *buffer, struct wlr_box *box);

#endif // RENDER_PASS_EXT_H
