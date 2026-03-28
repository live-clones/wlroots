#ifndef PIXMAN_TRIANGLE_PASS_PASS_H
#define PIXMAN_TRIANGLE_PASS_PASS_H

#include "../triangle_pass.h"

struct pixman_render_triangle_pass {
	struct render_triangle_pass base;
};

struct render_triangle_pass *pixman_render_triangle_pass_create(
	struct wlr_renderer *renderer);
bool render_triangle_pass_is_pixman(const struct render_triangle_pass *triangle_pass);
struct pixman_render_triangle_pass *pixman_render_triangle_pass_from_pass(
	struct render_triangle_pass *triangle_pass);

#endif // PIXMAN_TRIANGLE_PASS_PASS_H
