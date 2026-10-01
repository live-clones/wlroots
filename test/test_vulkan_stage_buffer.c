#include <wayland-util.h>

#include "render/vulkan.h"
#include "util/testing.h"

#define BUF_SIZE 1024
#define ALLOC_FAIL ((VkDeviceSize)-1)

static void stage_buffer_init(struct wlr_vk_stage_buffer *buf) {
	*buf = (struct wlr_vk_stage_buffer){
		.buf_size = BUF_SIZE,
	};
	wl_array_init(&buf->watermarks);
}

static void stage_buffer_finish(struct wlr_vk_stage_buffer *buf) {
	wl_array_release(&buf->watermarks);
}

static bool push_watermark(struct wlr_vk_stage_buffer *buf,
		uint64_t timeline_point) {
	struct wlr_vk_stage_watermark *mark = wl_array_add(
		&buf->watermarks, sizeof(*mark));
	if (mark == NULL) {
		return false;
	}
	*mark = (struct wlr_vk_stage_watermark){
		.head = buf->head,
		.timeline_point = timeline_point,
	};
	return true;
}

static size_t watermark_count(const struct wlr_vk_stage_buffer *buf) {
	return buf->watermarks.size / sizeof(struct wlr_vk_stage_watermark);
}

static void test_alloc_simple(struct testing *t) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	t_expect(t, vulkan_stage_buffer_alloc(&buf, 100, 1), T_EQ, 0);
	t_expect(t, buf.head, T_EQ, 100);
	t_expect(t, vulkan_stage_buffer_alloc(&buf, 200, 1), T_EQ, 100);
	t_expect(t, buf.head, T_EQ, 300);
	t_expect(t, buf.tail, T_EQ, 0);

	stage_buffer_finish(&buf);
}

static void test_alloc_alignment(struct testing *t) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	t_expect(t, vulkan_stage_buffer_alloc(&buf, 7, 1), T_EQ, 0);
	t_expect(t, buf.head, T_EQ, 7);

	t_expect(t, vulkan_stage_buffer_alloc(&buf, 4, 16), T_EQ, 16);
	t_expect(t, buf.head, T_EQ, 20);

	t_expect(t, vulkan_stage_buffer_alloc(&buf, 8, 8), T_EQ, 24);
	t_expect(t, buf.head, T_EQ, 32);

	stage_buffer_finish(&buf);
}

static void test_alloc_limit(struct testing *t) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	// We do not allow allocations that would cause head to equal tail
	t_expect(t, vulkan_stage_buffer_alloc(&buf, BUF_SIZE, 1), T_EQ, ALLOC_FAIL);
	t_expect(t, buf.head, T_EQ, 0);

	t_expect(t, vulkan_stage_buffer_alloc(&buf, BUF_SIZE-1, 1), T_EQ, 0);
	t_expect(t, buf.head, T_EQ, BUF_SIZE-1);

	stage_buffer_finish(&buf);
}

static void test_alloc_wrap(struct testing *t) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	// Fill the first 924 bytes
	t_expect(t, vulkan_stage_buffer_alloc(&buf, BUF_SIZE - 100, 1), T_EQ, 0);
	t_assert_true(t, push_watermark(&buf, 1));

	// Fill the end of the buffer
	t_expect(t, vulkan_stage_buffer_alloc(&buf, 50, 1), T_EQ, 924);
	t_assert_true(t, push_watermark(&buf, 2));

	// First, check that we don't wrap prematurely
	t_expect(t, vulkan_stage_buffer_alloc(&buf, 50, 1), T_EQ, ALLOC_FAIL);
	t_expect(t, vulkan_stage_buffer_alloc(&buf, 100, 1), T_EQ, ALLOC_FAIL);

	// Free the beginning of the buffer and try to wrap again
	vulkan_stage_buffer_reclaim(&buf, 1);
	t_expect(t, vulkan_stage_buffer_alloc(&buf, 50, 1), T_EQ, 0);
	t_expect(t, buf.tail, T_EQ, 924);
	t_expect(t, buf.head, T_EQ, 50);

	// Check that freeing from the end of the buffer still works
	vulkan_stage_buffer_reclaim(&buf, 2);
	t_expect(t, buf.tail, T_EQ, 974);
	t_expect(t, buf.head, T_EQ, 50);

	// Check that allocations still work
	t_expect(t, vulkan_stage_buffer_alloc(&buf, 100, 1), T_EQ, 50);
	t_expect(t, buf.tail, T_EQ, 974);
	t_expect(t, buf.head, T_EQ, 150);

	stage_buffer_finish(&buf);
}

static void test_reclaim_empty(struct testing *t) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	// Fresh buffer with no watermarks and head == tail == 0 is drained.
	vulkan_stage_buffer_reclaim(&buf, 0);
	t_expect(t, buf.head, T_EQ, 0);
	t_expect(t, buf.tail, T_EQ, 0);

	stage_buffer_finish(&buf);
}

static void test_reclaim_pending_not_completed(struct testing *t) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	t_expect(t, vulkan_stage_buffer_alloc(&buf, 100, 1), T_EQ, 0);
	t_assert_true(t, push_watermark(&buf, 1));

	// current point hasn't reached the watermark yet.
	vulkan_stage_buffer_reclaim(&buf, 0);
	t_expect_true(t, buf.head != buf.tail);
	t_expect(t, buf.tail, T_EQ, 0);
	t_expect(t, watermark_count(&buf), T_EQ, 1);

	stage_buffer_finish(&buf);
}

static void test_reclaim_partial(struct testing *t) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	t_expect(t, vulkan_stage_buffer_alloc(&buf, 100, 1), T_EQ, 0);
	t_assert_true(t, push_watermark(&buf, 1));
	t_expect(t, vulkan_stage_buffer_alloc(&buf, 100, 1), T_EQ, 100);
	t_assert_true(t, push_watermark(&buf, 2));

	// Only the first watermark is reached.
	vulkan_stage_buffer_reclaim(&buf, 1);
	t_expect_true(t, buf.head != buf.tail);
	t_expect(t, buf.tail, T_EQ, 100);
	if (t_expect(t, watermark_count(&buf), T_EQ, 1)) {
		const struct wlr_vk_stage_watermark *remaining = buf.watermarks.data;
		t_expect(t, remaining[0].head, T_EQ, 200);
		t_expect(t, remaining[0].timeline_point, T_EQ, 2);
	}

	stage_buffer_finish(&buf);
}

static void test_reclaim_all(struct testing *t) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	t_expect(t, vulkan_stage_buffer_alloc(&buf, 100, 1), T_EQ, 0);
	t_assert_true(t, push_watermark(&buf, 1));
	t_expect(t, vulkan_stage_buffer_alloc(&buf, 100, 1), T_EQ, 100);
	t_assert_true(t, push_watermark(&buf, 2));
	t_expect(t, vulkan_stage_buffer_alloc(&buf, 100, 1), T_EQ, 200);
	t_assert_true(t, push_watermark(&buf, 3));

	vulkan_stage_buffer_reclaim(&buf, 100);
	t_expect(t, buf.head, T_EQ, 300);
	t_expect(t, buf.tail, T_EQ, 300);
	t_expect(t, watermark_count(&buf), T_EQ, 0);

	stage_buffer_finish(&buf);
}

static const struct testing_case tests[] = {
	T_ENTRY(test_alloc_simple),
	T_ENTRY(test_alloc_alignment),
	T_ENTRY(test_alloc_limit),
	T_ENTRY(test_alloc_wrap),

	T_ENTRY(test_reclaim_empty),
	T_ENTRY(test_reclaim_pending_not_completed),
	T_ENTRY(test_reclaim_partial),
	T_ENTRY(test_reclaim_all),

	{ NULL, NULL },
};

int main(void) {
	return t_main(tests);
}
