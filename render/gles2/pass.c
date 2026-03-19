#include <stdlib.h>
#include <assert.h>
#include <pixman.h>
#include <time.h>
#include <unistd.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/util/transform.h>
#include "render/egl.h"
#include "render/gles2.h"
#include "util/matrix.h"

#define MAX_QUADS 86 // 4kb

static const struct wlr_render_pass_impl render_pass_impl;

static struct wlr_gles2_render_pass *get_render_pass(struct wlr_render_pass *wlr_pass) {
	assert(wlr_pass->impl == &render_pass_impl);
	struct wlr_gles2_render_pass *pass = wl_container_of(wlr_pass, pass, base);
	return pass;
}

static bool render_pass_submit(struct wlr_render_pass *wlr_pass) {
	struct wlr_gles2_render_pass *pass = get_render_pass(wlr_pass);
	struct wlr_gles2_renderer *renderer = pass->buffer->renderer;
	struct wlr_gles2_render_timer *timer = pass->timer;
	bool ok = false;

	push_gles2_debug(renderer);

	if (timer) {
		// clear disjoint flag
		GLint64 disjoint;
		renderer->procs.glGetInteger64vEXT(GL_GPU_DISJOINT_EXT, &disjoint);
		// set up the query
		renderer->procs.glQueryCounterEXT(timer->id, GL_TIMESTAMP_EXT);
		// get end-of-CPU-work time in GL time domain
		renderer->procs.glGetInteger64vEXT(GL_TIMESTAMP_EXT, &timer->gl_cpu_end);
		// get end-of-CPU-work time in CPU time domain
		clock_gettime(CLOCK_MONOTONIC, &timer->cpu_end);
	}

	if (pass->signal_timeline != NULL) {
		EGLSyncKHR sync = wlr_egl_create_sync(renderer->egl, -1);
		if (sync == EGL_NO_SYNC_KHR) {
			goto out;
		}

		int sync_file_fd = wlr_egl_dup_fence_fd(renderer->egl, sync);
		wlr_egl_destroy_sync(renderer->egl, sync);
		if (sync_file_fd < 0) {
			goto out;
		}

		ok = wlr_drm_syncobj_timeline_import_sync_file(pass->signal_timeline, pass->signal_point, sync_file_fd);
		close(sync_file_fd);
		if (!ok) {
			goto out;
		}
	} else {
		glFlush();
	}

	ok = true;

out:
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	pop_gles2_debug(renderer);
	wlr_egl_restore_context(&pass->prev_ctx);

	wlr_drm_syncobj_timeline_unref(pass->signal_timeline);
	wlr_buffer_unlock(pass->buffer->buffer);
	free(pass);

	return ok;
}

static void render(const struct wlr_box *box, const pixman_region32_t *clip, GLint attrib) {
	pixman_region32_t region;
	pixman_region32_init_rect(&region, box->x, box->y, box->width, box->height);

	if (clip) {
		pixman_region32_intersect(&region, &region, clip);
	}

	int rects_len;
	const pixman_box32_t *rects = pixman_region32_rectangles(&region, &rects_len);
	if (rects_len == 0) {
		pixman_region32_fini(&region);
		return;
	}

	glEnableVertexAttribArray(attrib);

	for (int i = 0; i < rects_len;) {
		int batch = rects_len - i < MAX_QUADS ? rects_len - i : MAX_QUADS;
		int batch_end = batch + i;

		size_t vert_index = 0;
		GLfloat verts[MAX_QUADS * 6 * 2];
		for (; i < batch_end; i++) {
			const pixman_box32_t *rect = &rects[i];

			verts[vert_index++] = (GLfloat)(rect->x1 - box->x) / box->width;
			verts[vert_index++] = (GLfloat)(rect->y1 - box->y) / box->height;
			verts[vert_index++] = (GLfloat)(rect->x2 - box->x) / box->width;
			verts[vert_index++] = (GLfloat)(rect->y1 - box->y) / box->height;
			verts[vert_index++] = (GLfloat)(rect->x1 - box->x) / box->width;
			verts[vert_index++] = (GLfloat)(rect->y2 - box->y) / box->height;
			verts[vert_index++] = (GLfloat)(rect->x2 - box->x) / box->width;
			verts[vert_index++] = (GLfloat)(rect->y1 - box->y) / box->height;
			verts[vert_index++] = (GLfloat)(rect->x2 - box->x) / box->width;
			verts[vert_index++] = (GLfloat)(rect->y2 - box->y) / box->height;
			verts[vert_index++] = (GLfloat)(rect->x1 - box->x) / box->width;
			verts[vert_index++] = (GLfloat)(rect->y2 - box->y) / box->height;
		}

		glVertexAttribPointer(attrib, 2, GL_FLOAT, GL_FALSE, 0, verts);
		glDrawArrays(GL_TRIANGLES, 0, batch * 6);
	}

	glDisableVertexAttribArray(attrib);

	pixman_region32_fini(&region);
}

static void set_proj_matrix(GLint loc, float proj[9], const struct wlr_box *box) {
	float gl_matrix[9];
	wlr_matrix_identity(gl_matrix);
	wlr_matrix_translate(gl_matrix, box->x, box->y);
	wlr_matrix_scale(gl_matrix, box->width, box->height);
	wlr_matrix_multiply(gl_matrix, proj, gl_matrix);
	glUniformMatrix3fv(loc, 1, GL_FALSE, gl_matrix);
}

static void set_tex_matrix(GLint loc, enum wl_output_transform trans,
		const struct wlr_fbox *box) {
	float tex_matrix[9];
	wlr_matrix_identity(tex_matrix);
	wlr_matrix_translate(tex_matrix, box->x, box->y);
	wlr_matrix_scale(tex_matrix, box->width, box->height);
	wlr_matrix_translate(tex_matrix, .5, .5);

	// since textures have a different origin point we have to transform
	// differently if we are rotating
	if (trans & WL_OUTPUT_TRANSFORM_90) {
		wlr_matrix_transform(tex_matrix, wlr_output_transform_invert(trans));
	} else {
		wlr_matrix_transform(tex_matrix, trans);
	}
	wlr_matrix_translate(tex_matrix, -.5, -.5);

	glUniformMatrix3fv(loc, 1, GL_FALSE, tex_matrix);
}

static void setup_blending(enum wlr_render_blend_mode mode) {
	switch (mode) {
	case WLR_RENDER_BLEND_MODE_PREMULTIPLIED:
		glEnable(GL_BLEND);
		break;
	case WLR_RENDER_BLEND_MODE_NONE:
		glDisable(GL_BLEND);
		break;
	}
}

static void render_pass_add_texture(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_texture_options *options) {
	struct wlr_gles2_render_pass *pass = get_render_pass(wlr_pass);
	struct wlr_gles2_renderer *renderer = pass->buffer->renderer;
	struct wlr_gles2_texture *texture = gles2_get_texture(options->texture);

	struct wlr_gles2_tex_shader *shader = NULL;

	switch (texture->target) {
	case GL_TEXTURE_2D:
		if (texture->has_alpha) {
			shader = &renderer->shaders.tex_rgba;
		} else {
			shader = &renderer->shaders.tex_rgbx;
		}
		break;
	case GL_TEXTURE_EXTERNAL_OES:
		// EGL_EXT_image_dma_buf_import_modifiers requires
		// GL_OES_EGL_image_external
		assert(renderer->exts.OES_egl_image_external);
		shader = &renderer->shaders.tex_ext;
		break;
	default:
		abort();
	}

	struct wlr_box dst_box;
	struct wlr_fbox src_fbox;
	wlr_render_texture_options_get_src_box(options, &src_fbox);
	wlr_render_texture_options_get_dst_box(options, &dst_box);
	float alpha = wlr_render_texture_options_get_alpha(options);

	src_fbox.x /= options->texture->width;
	src_fbox.y /= options->texture->height;
	src_fbox.width /= options->texture->width;
	src_fbox.height /= options->texture->height;

	push_gles2_debug(renderer);

	if (options->wait_timeline != NULL) {
		int sync_file_fd =
			wlr_drm_syncobj_timeline_export_sync_file(options->wait_timeline, options->wait_point);
		if (sync_file_fd < 0) {
			return;
		}

		EGLSyncKHR sync = wlr_egl_create_sync(renderer->egl, sync_file_fd);
		close(sync_file_fd);
		if (sync == EGL_NO_SYNC_KHR) {
			return;
		}

		bool ok = wlr_egl_wait_sync(renderer->egl, sync);
		wlr_egl_destroy_sync(renderer->egl, sync);
		if (!ok) {
			return;
		}
	}

	setup_blending(!texture->has_alpha && alpha == 1.0 ?
		WLR_RENDER_BLEND_MODE_NONE : options->blend_mode);

	glUseProgram(shader->program);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(texture->target, texture->tex);

	switch (options->filter_mode) {
	case WLR_SCALE_FILTER_BILINEAR:
		glTexParameteri(texture->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(texture->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		break;
	case WLR_SCALE_FILTER_NEAREST:
		glTexParameteri(texture->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(texture->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		break;
	}

	glUniform1i(shader->tex, 0);
	glUniform1f(shader->alpha, alpha);
	set_proj_matrix(shader->proj, pass->projection_matrix, &dst_box);
	set_tex_matrix(shader->tex_proj, options->transform, &src_fbox);

	render(&dst_box, options->clip, shader->pos_attrib);

	glBindTexture(texture->target, 0);
	pop_gles2_debug(renderer);
}

static void render_pass_add_rect(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_rect_options *options) {
	struct wlr_gles2_render_pass *pass = get_render_pass(wlr_pass);
	struct wlr_gles2_renderer *renderer = pass->buffer->renderer;

	const struct wlr_render_color *color = &options->color;
	struct wlr_box box;
	struct wlr_buffer *wlr_buffer = pass->buffer->buffer;
	wlr_render_rect_options_get_box(options, wlr_buffer, &box);

	push_gles2_debug(renderer);
	enum wlr_render_blend_mode blend_mode =
		color->a == 1.0 ? WLR_RENDER_BLEND_MODE_NONE : options->blend_mode;
	if (blend_mode == WLR_RENDER_BLEND_MODE_NONE &&
			options->clip == NULL &&
			box.x == 0 && box.y == 0 &&
			box.width == wlr_buffer->width &&
			box.height == wlr_buffer->height) {
		glClearColor(color->r, color->g, color->b, color->a);
		glClear(GL_COLOR_BUFFER_BIT);
	} else {
		setup_blending(blend_mode);
		glUseProgram(renderer->shaders.quad.program);
		set_proj_matrix(renderer->shaders.quad.proj, pass->projection_matrix, &box);
		glUniform4f(renderer->shaders.quad.color, color->r, color->g, color->b, color->a);
		render(&box, options->clip, renderer->shaders.quad.pos_attrib);
	}

	pop_gles2_debug(renderer);
}

static void render_pass_add_blur(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_blur_options *options) {
	struct wlr_gles2_render_pass *pass = get_render_pass(wlr_pass);
	struct wlr_gles2_renderer *renderer = pass->buffer->renderer;

	int bx = options->box.x;
	int by = options->box.y;
	int bw = options->box.width;
	int bh = options->box.height;
	int buf_w = pass->buffer->buffer->width;
	int buf_h = pass->buffer->buffer->height;

	if (bw <= 0 || bh <= 0) {
		return;
	}
	if (bx < 0) {
		bw += bx;
		bx = 0;
	}
	if (by < 0) {
		bh += by;
		by = 0;
	}
	if (bx + bw > buf_w) {
		bw = buf_w - bx;
	}
	if (by + bh > buf_h) {
		bh = buf_h - by;
	}
	if (bw <= 0 || bh <= 0) {
		return;
	}

	push_gles2_debug(renderer);

	if (renderer->blur_scratch.tex[0] == 0 || renderer->blur_scratch.width != bw ||
			renderer->blur_scratch.height != bh) {
		if (renderer->blur_scratch.tex[0] != 0) {
			glDeleteFramebuffers(2, renderer->blur_scratch.fbo);
			glDeleteTextures(2, renderer->blur_scratch.tex);
			memset(&renderer->blur_scratch, 0, sizeof(renderer->blur_scratch));
		}

		glGenTextures(2, renderer->blur_scratch.tex);
		glGenFramebuffers(2, renderer->blur_scratch.fbo);

		for (int i = 0; i < 2; i++) {
			glBindTexture(GL_TEXTURE_2D, renderer->blur_scratch.tex[i]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glBindTexture(GL_TEXTURE_2D, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, renderer->blur_scratch.fbo[i]);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
				renderer->blur_scratch.tex[i], 0);

			GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
			if (status != GL_FRAMEBUFFER_COMPLETE) {
				wlr_log(WLR_ERROR, "Blur scratch FBO %d incomplete: 0x%x", i, status);
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glDeleteFramebuffers(2, renderer->blur_scratch.fbo);
				glDeleteTextures(2, renderer->blur_scratch.tex);
				memset(&renderer->blur_scratch, 0, sizeof(renderer->blur_scratch));
				pop_gles2_debug(renderer);
				return;
			}
		}

		renderer->blur_scratch.width = bw;
		renderer->blur_scratch.height = bh;
	}

	GLuint main_fbo = gles2_buffer_get_fbo(pass->buffer);
	glBindFramebuffer(GL_FRAMEBUFFER, main_fbo);
	glBindTexture(GL_TEXTURE_2D, renderer->blur_scratch.tex[0]);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bx, by, bw, bh);
	glBindTexture(GL_TEXTURE_2D, 0);

	float scratch_proj[9];
	matrix_projection(scratch_proj, bw, bh, WL_OUTPUT_TRANSFORM_FLIPPED_180);
	struct wlr_box full_box = {
	 	.x = 0,
		.y = 0,
		.width = bw,
		.height = bh
	};
	struct wlr_fbox full_tex = {
		.x = 0,
		.y = 0,
		.width = 1,
		.height = 1
	};

	glUseProgram(renderer->shaders.blur.program);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(renderer->shaders.blur.tex, 0);
	set_proj_matrix(renderer->shaders.blur.proj, scratch_proj, &full_box);
	set_tex_matrix(renderer->shaders.blur.tex_proj, WL_OUTPUT_TRANSFORM_NORMAL, &full_tex);
	setup_blending(WLR_RENDER_BLEND_MODE_NONE);

	glBindFramebuffer(GL_FRAMEBUFFER, renderer->blur_scratch.fbo[1]);
	glViewport(0, 0, bw, bh);
	glBindTexture(GL_TEXTURE_2D, renderer->blur_scratch.tex[0]);
	glUniform2f(renderer->shaders.blur.texel_step, 1.0f / bw, 0.0f);
	render(&full_box, NULL, renderer->shaders.blur.pos_attrib);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, renderer->blur_scratch.fbo[0]);
	glBindTexture(GL_TEXTURE_2D, renderer->blur_scratch.tex[1]);
	glUniform2f(renderer->shaders.blur.texel_step, 0.0f, 1.0f / bh);
	render(&full_box, NULL, renderer->shaders.blur.pos_attrib);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, main_fbo);
	glViewport(0, 0, buf_w, buf_h);

	glUseProgram(renderer->shaders.tex_rgba.program);
	setup_blending(WLR_RENDER_BLEND_MODE_NONE);
	glBindTexture(GL_TEXTURE_2D, renderer->blur_scratch.tex[0]);
	glUniform1i(renderer->shaders.tex_rgba.tex, 0);

	float alpha = 1.0f;
	glUniform1f(renderer->shaders.tex_rgba.alpha, alpha);
	set_proj_matrix(renderer->shaders.tex_rgba.proj, pass->projection_matrix,
		&(struct wlr_box){
			.x = bx,
			.y = by,
			.width = bw,
			.height = bh
		});
	set_tex_matrix(renderer->shaders.tex_rgba.tex_proj, WL_OUTPUT_TRANSFORM_NORMAL, &full_tex);
	render(&(struct wlr_box){
		.x = bx,
		.y = by,
		.width = bw,
		.height = bh
	}, options->clip, renderer->shaders.tex_rgba.pos_attrib);
	glBindTexture(GL_TEXTURE_2D, 0);

	pop_gles2_debug(renderer);
}

static const struct wlr_render_pass_impl render_pass_impl = {
	.submit = render_pass_submit,
	.add_texture = render_pass_add_texture,
	.add_rect = render_pass_add_rect,
	.add_blur = render_pass_add_blur,
};

static const char *reset_status_str(GLenum status) {
	switch (status) {
	case GL_GUILTY_CONTEXT_RESET_KHR:
		return "guilty";
	case GL_INNOCENT_CONTEXT_RESET_KHR:
		return "innocent";
	case GL_UNKNOWN_CONTEXT_RESET_KHR:
		return "unknown";
	default:
		return "<invalid>";
	}
}

struct wlr_gles2_render_pass *begin_gles2_buffer_pass(struct wlr_gles2_buffer *buffer,
		struct wlr_egl_context *prev_ctx, struct wlr_gles2_render_timer *timer,
		struct wlr_drm_syncobj_timeline *signal_timeline, uint64_t signal_point) {
	struct wlr_gles2_renderer *renderer = buffer->renderer;
	struct wlr_buffer *wlr_buffer = buffer->buffer;

	if (renderer->procs.glGetGraphicsResetStatusKHR) {
		GLenum status = renderer->procs.glGetGraphicsResetStatusKHR();
		if (status != GL_NO_ERROR) {
			wlr_log(WLR_ERROR, "GPU reset (%s)", reset_status_str(status));
			wl_signal_emit_mutable(&renderer->wlr_renderer.events.lost, NULL);
			return NULL;
		}
	}

	GLint fbo = gles2_buffer_get_fbo(buffer);
	if (!fbo) {
		return NULL;
	}

	struct wlr_gles2_render_pass *pass = calloc(1, sizeof(*pass));
	if (pass == NULL) {
		return NULL;
	}

	wlr_render_pass_init(&pass->base, &render_pass_impl);
	wlr_buffer_lock(wlr_buffer);
	pass->buffer = buffer;
	pass->timer = timer;
	pass->prev_ctx = *prev_ctx;
	if (signal_timeline != NULL) {
		pass->signal_timeline = wlr_drm_syncobj_timeline_ref(signal_timeline);
		pass->signal_point = signal_point;
	}

	matrix_projection(pass->projection_matrix, wlr_buffer->width, wlr_buffer->height,
		WL_OUTPUT_TRANSFORM_FLIPPED_180);

	push_gles2_debug(renderer);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	glViewport(0, 0, wlr_buffer->width, wlr_buffer->height);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_SCISSOR_TEST);
	pop_gles2_debug(renderer);

	return pass;
}
