#ifndef RENDER_GLES_H
#define RENDER_GLES_H

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles.h>
#include <wlr/render/interface.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>

#include "render/egl.h"

// mesa ships old GL headers that don't include this type, so for distros that use headers from
// mesa we need to def it ourselves until they update.
// https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/23144
typedef void (GL_APIENTRYP PFNGLGETINTEGER64VEXTPROC) (GLenum pname, GLint64 *data);

struct wlr_gles_pixel_format {
	uint32_t drm_format;
	// optional field, if empty then internalformat = format
	GLint gl_internalformat;
	GLint gl_format, gl_type;
};

struct wlr_gles_tex_shader {
	GLuint program;
	GLint proj;
	GLint tex_proj;
	GLint tex;
	GLint alpha;
	GLint pos_attrib;
};

struct wlr_gles_renderer {
	struct wlr_renderer wlr_renderer;

	struct wlr_egl *egl;
	int drm_fd;

	struct wlr_drm_format_set shm_texture_formats;

	const char *exts_str;
	struct {
		bool EXT_read_format_bgra;
		bool KHR_debug;
		bool OES_egl_image_external;
		bool OES_egl_image;
		bool EXT_texture_type_2_10_10_10_REV;
		bool OES_texture_half_float_linear;
		bool EXT_texture_norm16;
		bool EXT_disjoint_timer_query;
	} exts;

	struct {
		PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
		PFNGLDEBUGMESSAGECALLBACKKHRPROC glDebugMessageCallbackKHR;
		PFNGLDEBUGMESSAGECONTROLKHRPROC glDebugMessageControlKHR;
		PFNGLPOPDEBUGGROUPKHRPROC glPopDebugGroupKHR;
		PFNGLPUSHDEBUGGROUPKHRPROC glPushDebugGroupKHR;
		PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES;
		PFNGLGETGRAPHICSRESETSTATUSKHRPROC glGetGraphicsResetStatusKHR;
		PFNGLGENQUERIESEXTPROC glGenQueriesEXT;
		PFNGLDELETEQUERIESEXTPROC glDeleteQueriesEXT;
		PFNGLQUERYCOUNTEREXTPROC glQueryCounterEXT;
		PFNGLGETQUERYOBJECTIVEXTPROC glGetQueryObjectivEXT;
		PFNGLGETQUERYOBJECTUI64VEXTPROC glGetQueryObjectui64vEXT;
		PFNGLGETINTEGER64VEXTPROC glGetInteger64vEXT;
	} procs;

	struct {
		struct {
			GLuint program;
			GLint proj;
			GLint color;
			GLint pos_attrib;
		} quad;
		struct wlr_gles_tex_shader tex_rgba;
		struct wlr_gles_tex_shader tex_rgbx;
		struct wlr_gles_tex_shader tex_ext;
	} shaders;

	struct wl_list buffers; // wlr_gles_buffer.link
	struct wl_list textures; // wlr_gles_texture.link
};

struct wlr_gles_render_timer {
	struct wlr_render_timer base;
	struct wlr_gles_renderer *renderer;
	struct timespec cpu_start;
	struct timespec cpu_end;
	GLuint id;
	GLint64 gl_cpu_end;
};

struct wlr_gles_buffer {
	struct wlr_buffer *buffer;
	struct wlr_gles_renderer *renderer;
	struct wl_list link; // wlr_gles_renderer.buffers
	bool external_only;

	EGLImageKHR image;
	GLuint rbo;
	GLuint fbo;
	GLuint tex;

	struct wlr_addon addon;
};

struct wlr_gles_texture {
	struct wlr_texture wlr_texture;
	struct wlr_gles_renderer *renderer;
	struct wl_list link; // wlr_gles_renderer.textures

	GLenum target;

	// If this texture is imported from a buffer, the texture is does not own
	// these states. These cannot be destroyed along with the texture in this
	// case.
	GLuint tex;
	GLuint fbo;

	bool has_alpha;

	uint32_t drm_format; // for mutable textures only, used to interpret upload data
	struct wlr_gles_buffer *buffer; // for DMA-BUF imports only
};

struct wlr_gles_render_pass {
	struct wlr_render_pass base;
	struct wlr_gles_buffer *buffer;
	float projection_matrix[9];
	struct wlr_egl_context prev_ctx;
	struct wlr_gles_render_timer *timer;
	struct wlr_drm_syncobj_timeline *signal_timeline;
	uint64_t signal_point;
};

bool is_gles_pixel_format_supported(const struct wlr_gles_renderer *renderer,
	const struct wlr_gles_pixel_format *format);
const struct wlr_gles_pixel_format *get_gles_format_from_drm(uint32_t fmt);
const struct wlr_gles_pixel_format *get_gles_format_from_gl(
	GLint gl_format, GLint gl_type, bool alpha);
void get_gles_shm_formats(const struct wlr_gles_renderer *renderer,
	struct wlr_drm_format_set *out);

GLuint gles_buffer_get_fbo(struct wlr_gles_buffer *buffer);

struct wlr_gles_renderer *gles_get_renderer(
	struct wlr_renderer *wlr_renderer);
struct wlr_gles_render_timer *gles_get_render_timer(
	struct wlr_render_timer *timer);
struct wlr_gles_texture *gles_get_texture(
	struct wlr_texture *wlr_texture);
struct wlr_gles_buffer *gles_buffer_get_or_create(struct wlr_gles_renderer *renderer,
	struct wlr_buffer *wlr_buffer);

struct wlr_texture *gles_texture_from_buffer(struct wlr_renderer *wlr_renderer,
	struct wlr_buffer *buffer);
void gles_texture_destroy(struct wlr_gles_texture *texture);

void push_gles_debug_(struct wlr_gles_renderer *renderer,
	const char *file, const char *func);
#define push_gles_debug(renderer) push_gles_debug_(renderer, _WLR_FILENAME, __func__)
void pop_gles_debug(struct wlr_gles_renderer *renderer);

struct wlr_gles_render_pass *begin_gles_buffer_pass(struct wlr_gles_buffer *buffer,
	struct wlr_egl_context *prev_ctx, struct wlr_gles_render_timer *timer,
	struct wlr_drm_syncobj_timeline *signal_timeline, uint64_t signal_point);

#endif
