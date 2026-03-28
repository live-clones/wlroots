#include <stdbool.h>
#include <stdlib.h>
#include <wlr/render/gles2.h>
#include <wlr/util/log.h>
#include <render/gles2.h>

#include "triangle_pass.h"

#include "triangle_vert.h"
#include "triangle_frag.h"

static void custom_triangle_render(struct wlr_render_pass *render_pass,
		const struct custom_render_triangle_options *options) {
	struct wlr_gles2_render_pass *wlr_gles2_pass =
		wlr_gles2_render_pass_from_render_pass(render_pass);
	struct render_triangle_pass *base =
		wlr_gles2_pass->buffer->renderer->wlr_renderer.data;
	struct gles2_render_triangle_pass *triangle_pass =
		gles2_render_triangle_pass_from_pass(base);
	struct wlr_gles2_renderer *renderer = wlr_gles2_pass->buffer->renderer;

	struct wlr_box box;
	struct wlr_buffer *wlr_buffer = wlr_gles2_pass->buffer->buffer;
	custom_render_triangle_options_get_box(options, wlr_buffer, &box);

	int width = wlr_gles2_pass->buffer->buffer->width;
	int height = wlr_gles2_pass->buffer->buffer->height;

	float x0 = options->box.x + options->box.width * 0.50f;
	float y0 = options->box.y + options->box.height * 0.10f;
	float x1 = options->box.x + options->box.width * 0.10f;
	float y1 = options->box.y + options->box.height * 0.90f;
	float x2 = options->box.x + options->box.width * 0.90f;
	float y2 = options->box.y + options->box.height * 0.90f;

	GLfloat verts[3][5] = {
		{ x0 / width * 2.0f - 1.0f, 1.0f - y0 / height * 2.0f, 1.0f, 0.1f, 0.1f },
		{ x1 / width * 2.0f - 1.0f, 1.0f - y1 / height * 2.0f, 0.1f, 1.0f, 0.1f },
		{ x2 / width * 2.0f - 1.0f, 1.0f - y2 / height * 2.0f, 0.1f, 0.2f, 1.0f },
	};

	wlr_gles2_push_debug(renderer);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);

	glUseProgram(triangle_pass->shader.program);
	wlr_gles_set_proj_matrix(triangle_pass->shader.proj, wlr_gles2_pass->projection_matrix, &box);
	glEnableVertexAttribArray(triangle_pass->shader.pos_attrib);
	glVertexAttribPointer(triangle_pass->shader.pos_attrib, 2, GL_FLOAT, GL_FALSE,
		5 * sizeof(GLfloat), verts);
	glEnableVertexAttribArray(triangle_pass->shader.color_attrib);
	glVertexAttribPointer(triangle_pass->shader.color_attrib, 3, GL_FLOAT, GL_FALSE,
		5 * sizeof(GLfloat), &verts[0][2]);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glDisableVertexAttribArray(triangle_pass->shader.pos_attrib);
	glDisableVertexAttribArray(triangle_pass->shader.color_attrib);
	wlr_gles2_pop_debug(renderer);
}

static void custom_triangle_destroy(struct render_triangle_pass *pass) {
	struct gles2_render_triangle_pass *gles2_pass =
		gles2_render_triangle_pass_from_pass(pass);
	if (gles2_pass->shader.program != 0) {
		glDeleteProgram(gles2_pass->shader.program);
	}

	free(gles2_pass);
}

static const struct render_triangle_pass_impl custom_triangle_impl = {
	.destroy = custom_triangle_destroy,
	.render = custom_triangle_render,
};

struct render_triangle_pass *gles2_render_triangle_pass_create(
		struct wlr_renderer *wlr_renderer) {
	struct gles2_render_triangle_pass *pass = calloc(1, sizeof(*pass));
	if (pass == NULL) {
		wlr_log_errno(WLR_ERROR, "failed to allocate gles2_render_triangle_pass");
		return NULL;
	}

	struct wlr_gles2_renderer *renderer =  wlr_gles2_renderer_from_renderer(wlr_renderer);
	if (!wlr_egl_make_current(renderer->egl, NULL)) {
		free(pass);
		return NULL;
	}

	render_triangle_pass_init(&pass->base, &custom_triangle_impl);
	wlr_gles2_push_debug(renderer);
	GLuint prog;
	pass->shader.program = prog =
		wlr_gles2_link_program(renderer, custom_triangle_vert, custom_triangle_frag);
	if (!pass->shader.program) {
		goto error;
	}

	pass->shader.proj = -1;
	pass->shader.color_attrib = glGetAttribLocation(pass->shader.program, "color");
	pass->shader.pos_attrib = glGetAttribLocation(pass->shader.program, "pos");
	if (pass->shader.pos_attrib < 0 || pass->shader.color_attrib < 0) {
		wlr_log(WLR_ERROR, "triangle shader attribute lookup failed: pos=%d color=%d",
			pass->shader.pos_attrib, pass->shader.color_attrib);
		goto error;
	}
	wlr_gles2_pop_debug(renderer);
	wlr_egl_unset_current(renderer->egl);
	pass->renderer = renderer;
	return &pass->base;

error:
	render_triangle_pass_destroy(&pass->base);
	wlr_gles2_pop_debug(renderer);
	wlr_egl_unset_current(renderer->egl);

	return NULL;
}

bool render_triangle_pass_is_gles2(const struct render_triangle_pass *triangle_pass) {
	return triangle_pass->impl == & custom_triangle_impl;
}

struct gles2_render_triangle_pass *gles2_render_triangle_pass_from_pass(
		struct render_triangle_pass *triangle_pass) {
	if (!render_triangle_pass_is_gles2(triangle_pass)) {
		return NULL;
	}

	struct gles2_render_triangle_pass *gles2_pass =
		wl_container_of(triangle_pass, gles2_pass, base);

	return gles2_pass;
}
