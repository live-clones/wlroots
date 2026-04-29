#ifndef GLES2_TRIANGLE_PASS_PASS_H
#define GLES2_TRIANGLE_PASS_PASS_H

#include "../triangle_pass.h"

#include <render/gles2.h>

#include <GLES2/gl2.h>

struct gles2_render_triangle_pass {
	struct render_triangle_pass base;
	struct wlr_gles2_renderer *renderer;
	struct {
		GLuint program;
		GLint proj;
		GLint pos_attrib;
		GLint color_attrib;
	} shader;
};

struct render_triangle_pass *gles2_render_triangle_pass_create(
	struct wlr_renderer *wlr_renderer);
bool render_triangle_pass_is_gles2(const struct render_triangle_pass *triangle_pass);
struct gles2_render_triangle_pass *gles2_render_triangle_pass_from_pass(
	struct render_triangle_pass *triangle_pass);

#endif // GLES2_TRIANGLE_PASS_PASS_H
