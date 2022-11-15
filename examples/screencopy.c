/*
 * Copyright © 2008 Kristian Høgsberg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <png.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <assert.h>
#include <libdrm/drm_fourcc.h>
#include <wayland-client-protocol.h>
#include "ext-screencopy-v1-client-protocol.h"

struct format {
	enum wl_shm_format wl_format;
	bool is_bgr;
};

static struct wl_shm *shm = NULL;
static struct ext_screencopy_manager_v1 *screencopy_manager = NULL;
static struct wl_output *output = NULL;

static struct {
	struct wl_buffer *wl_buffer;
	void *data;
	enum wl_shm_format format;
	int width, height, stride;
	bool y_invert;
} buffer;
bool buffer_copy_done = false;

// wl_shm_format describes little-endian formats, libpng uses big-endian
// formats (so Wayland's ABGR is libpng's RGBA).
static const struct format formats[] = {
	{WL_SHM_FORMAT_XRGB8888, true},
	{WL_SHM_FORMAT_ARGB8888, true},
	{WL_SHM_FORMAT_XBGR8888, false},
	{WL_SHM_FORMAT_ABGR8888, false},
};

static enum wl_shm_format drm_format_to_wl_shm(uint32_t in) {
	switch (in) {
	case DRM_FORMAT_ARGB8888: return WL_SHM_FORMAT_ARGB8888;
	case DRM_FORMAT_XRGB8888: return WL_SHM_FORMAT_XRGB8888;
	}

	return in;
}

static struct wl_buffer *create_shm_buffer(enum wl_shm_format fmt,
		int width, int height, int stride, void **data_out) {
	int size = stride * height;

	const char shm_name[] = "/wlroots-screencopy";
	int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		fprintf(stderr, "shm_open failed\n");
		return NULL;
	}
	shm_unlink(shm_name);

	int ret;
	while ((ret = ftruncate(fd, size)) == EINTR) {
		// No-op
	}
	if (ret < 0) {
		close(fd);
		fprintf(stderr, "ftruncate failed\n");
		return NULL;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		perror("mmap failed");
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	close(fd);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
		stride, fmt);
	wl_shm_pool_destroy(pool);

	*data_out = data;
	return buffer;
}

static void commit_buffer(struct ext_screencopy_session_v1 *session) {
	if (buffer.wl_buffer == NULL) {
		fprintf(stderr, "no supported buffer format reported\n");
		exit(EXIT_FAILURE);
	}

	ext_screencopy_session_v1_attach_buffer(session, buffer.wl_buffer);
	ext_screencopy_session_v1_damage_buffer(session, 0, 0, buffer.width,
			buffer.height);

	ext_screencopy_session_v1_commit(session, 0);
}

static void session_handle_buffer_info(void *data,
		struct ext_screencopy_session_v1 *session,
		enum ext_screencopy_session_v1_buffer_type type,
		uint32_t format, uint32_t width, uint32_t height,
		uint32_t stride) {
	if (type != EXT_SCREENCOPY_SESSION_V1_BUFFER_TYPE_WL_SHM) {
		return;
	}

	buffer.format = drm_format_to_wl_shm(format);
	buffer.width = width;
	buffer.height = height;
	buffer.stride = stride;

	// Make sure the buffer is not allocated
	assert(!buffer.wl_buffer);
	buffer.wl_buffer =
		create_shm_buffer(buffer.format, width, height, stride, &buffer.data);
	if (buffer.wl_buffer == NULL) {
		fprintf(stderr, "failed to create buffer\n");
		exit(EXIT_FAILURE);
	}
}

static void session_handle_cursor_buffer_info(void *data,
		struct ext_screencopy_session_v1 *session, const char *name,
		enum ext_screencopy_session_v1_input_type input_type,
		enum ext_screencopy_session_v1_buffer_type type,
		uint32_t format, uint32_t width, uint32_t height,
		uint32_t stride) {
}

static void session_handle_init_done(void *data,
		struct ext_screencopy_session_v1 *session) {
	commit_buffer(session);
	return;
}

static void session_handle_transform(void *data,
		struct ext_screencopy_session_v1 *session,
		int32_t transform) {
	if (transform == WL_OUTPUT_TRANSFORM_FLIPPED_180) {
		buffer.y_invert = true;
	}
}

static void session_handle_ready(void *data,
		struct ext_screencopy_session_v1 *session) {
	buffer_copy_done = true;
}

static void session_handle_failed(void *data,
		struct ext_screencopy_session_v1 *session,
		enum ext_screencopy_session_v1_failure_reason reason) {
	fprintf(stderr, "failed to copy frame\n");
	exit(EXIT_FAILURE);
}

static void session_handle_damage(void *data,
		struct ext_screencopy_session_v1 *session,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
}

static void session_handle_cursor_info(void *data,
		struct ext_screencopy_session_v1 *session, const char *name,
		enum ext_screencopy_session_v1_input_type input_type,
		int has_damage, int32_t pos_x, int32_t pos_y, int32_t width,
		int32_t height, int32_t hotspot_x, int32_t hotspot_y) {
}

static void session_handle_presentation_time(void *data,
		struct ext_screencopy_session_v1 *session,
		uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
}

static const struct ext_screencopy_session_v1_listener frame_listener = {
	.buffer_info = session_handle_buffer_info,
	.cursor_buffer_info = session_handle_cursor_buffer_info,
	.init_done = session_handle_init_done,
	.damage = session_handle_damage,
	.cursor_info = session_handle_cursor_info,
	.presentation_time = session_handle_presentation_time,
	.transform = session_handle_transform,
	.ready = session_handle_ready,
	.failed = session_handle_failed,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_output_interface.name) == 0 && output == NULL) {
		output = wl_registry_bind(registry, name, &wl_output_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface,
			ext_screencopy_manager_v1_interface.name) == 0) {
		screencopy_manager = wl_registry_bind(registry, name,
			&ext_screencopy_manager_v1_interface, 1);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	// Who cares?
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static void write_image(char *filename, enum wl_shm_format wl_fmt, int width,
		int height, int stride, bool y_invert, png_bytep data) {
	const struct format *fmt = NULL;
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
		if (formats[i].wl_format == wl_fmt) {
			fmt = &formats[i];
			break;
		}
	}
	if (fmt == NULL) {
		fprintf(stderr, "unsupported format %"PRIu32"\n", wl_fmt);
		exit(EXIT_FAILURE);
	}

	FILE *f = fopen(filename, "wb");
	if (f == NULL) {
		fprintf(stderr, "failed to open output file\n");
		exit(EXIT_FAILURE);
	}

	png_structp png =
		png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png_create_info_struct(png);

	png_init_io(png, f);

	png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA,
		PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT);

	if (fmt->is_bgr) {
		png_set_bgr(png);
	}

	png_write_info(png, info);

	for (size_t i = 0; i < (size_t)height; ++i) {
		png_bytep row;
		if (y_invert) {
			row = data + (height - i - 1) * stride;
		} else {
			row = data + i * stride;
		}
		png_write_row(png, row);
	}

	png_write_end(png, NULL);

	png_destroy_write_struct(&png, &info);

	fclose(f);
}

int main(int argc, char *argv[]) {
	struct wl_display * display = wl_display_connect(NULL);
	if (display == NULL) {
		perror("failed to create display");
		return EXIT_FAILURE;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (shm == NULL) {
		fprintf(stderr, "compositor is missing wl_shm\n");
		return EXIT_FAILURE;
	}
	if (screencopy_manager == NULL) {
		fprintf(stderr, "compositor doesn't support ext-screencopy-v1\n");
		return EXIT_FAILURE;
	}
	if (output == NULL) {
		fprintf(stderr, "no output available\n");
		return EXIT_FAILURE;
	}

	struct ext_screencopy_session_v1 *session =
		ext_screencopy_manager_v1_capture_output(screencopy_manager,
				output,
				EXT_SCREENCOPY_MANAGER_V1_OPTIONS_RENDER_CURSORS);
	ext_screencopy_session_v1_add_listener(session, &frame_listener, NULL);

	while (!buffer_copy_done && wl_display_dispatch(display) != -1) {
		// This space is intentionally left blank
	}

	write_image("wayland-screenshot.png", buffer.format, buffer.width,
		buffer.height, buffer.stride, buffer.y_invert, buffer.data);
	wl_buffer_destroy(buffer.wl_buffer);
	munmap(buffer.data, buffer.stride * buffer.height);

	return EXIT_SUCCESS;
}
