#include <assert.h>
#include <wayland-util.h>

#include "render/vulkan.h"
#include "util/unit_test.h"

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

static void push_watermark(struct wlr_vk_stage_buffer *buf,
		uint64_t timeline_point) {
	struct wlr_vk_stage_watermark *mark = wl_array_add(
		&buf->watermarks, sizeof(*mark));
	assert(mark != NULL);
	*mark = (struct wlr_vk_stage_watermark){
		.head = buf->head,
		.timeline_point = timeline_point,
	};
}

static size_t watermark_count(const struct wlr_vk_stage_buffer *buf) {
	return buf->watermarks.size / sizeof(struct wlr_vk_stage_watermark);
}

static bool test_alloc_simple(void) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	ASSERT_UINT64_EQ(0, vulkan_stage_buffer_alloc(&buf, 100, 1));
	ASSERT_UINT64_EQ(100, buf.head);
	ASSERT_UINT64_EQ(100, vulkan_stage_buffer_alloc(&buf, 200, 1));
	ASSERT_UINT64_EQ(300, buf.head);
	ASSERT_UINT64_EQ(0, buf.tail);

	stage_buffer_finish(&buf);
	return true;
}

static bool test_alloc_alignment(void) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	ASSERT_UINT64_EQ(0, vulkan_stage_buffer_alloc(&buf, 7, 1));
	ASSERT_UINT64_EQ(7, buf.head);

	ASSERT_UINT64_EQ(16, vulkan_stage_buffer_alloc(&buf, 4, 16));
	ASSERT_UINT64_EQ(20, buf.head);

	ASSERT_UINT64_EQ(24, vulkan_stage_buffer_alloc(&buf, 8, 8));
	ASSERT_UINT64_EQ(32, buf.head);

	stage_buffer_finish(&buf);
	return true;
}

static bool test_alloc_limit(void) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	// We do not allow allocations that would cause head to equal tail
	ASSERT_UINT64_EQ(ALLOC_FAIL, vulkan_stage_buffer_alloc(&buf, BUF_SIZE, 1));
	ASSERT_UINT64_EQ(0, buf.head);

	ASSERT_UINT64_EQ(0, vulkan_stage_buffer_alloc(&buf, BUF_SIZE-1, 1));
	ASSERT_UINT64_EQ(BUF_SIZE-1, buf.head);

	stage_buffer_finish(&buf);
	return true;
}

static bool test_alloc_wrap(void) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	// Fill the first 924 bytes
	ASSERT_UINT64_EQ(0, vulkan_stage_buffer_alloc(&buf, BUF_SIZE - 100, 1));
	push_watermark(&buf, 1);

	// Fill the end of the buffer
	ASSERT_UINT64_EQ(924, vulkan_stage_buffer_alloc(&buf, 50, 1));
	push_watermark(&buf, 2);

	// First, check that we don't wrap prematurely
	ASSERT_UINT64_EQ(ALLOC_FAIL, vulkan_stage_buffer_alloc(&buf, 50, 1));
	ASSERT_UINT64_EQ(ALLOC_FAIL, vulkan_stage_buffer_alloc(&buf, 100, 1));

	// Free the beginning of the buffer and try to wrap again
	vulkan_stage_buffer_reclaim(&buf, 1);
	ASSERT_UINT64_EQ(0, vulkan_stage_buffer_alloc(&buf, 50, 1));
	ASSERT_UINT64_EQ(924, buf.tail);
	ASSERT_UINT64_EQ(50, buf.head);

	// Check that freeing from the end of the buffer still works
	vulkan_stage_buffer_reclaim(&buf, 2);
	ASSERT_UINT64_EQ(974, buf.tail);
	ASSERT_UINT64_EQ(50, buf.head);

	// Check that allocations still work
	ASSERT_UINT64_EQ(50, vulkan_stage_buffer_alloc(&buf, 100, 1));
	ASSERT_UINT64_EQ(974, buf.tail);
	ASSERT_UINT64_EQ(150, buf.head);

	stage_buffer_finish(&buf);
	return true;
}

static bool test_reclaim_empty(void) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	// Fresh buffer with no watermarks and head == tail == 0 is drained.
	vulkan_stage_buffer_reclaim(&buf, 0);
	ASSERT_UINT64_EQ(buf.tail, buf.head);
	ASSERT_UINT64_EQ(0, buf.tail);

	stage_buffer_finish(&buf);
	return true;
}

static bool test_reclaim_pending_not_completed(void) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	ASSERT_UINT64_EQ(0, vulkan_stage_buffer_alloc(&buf, 100, 1));
	push_watermark(&buf, 1);

	// current point hasn't reached the watermark yet.
	vulkan_stage_buffer_reclaim(&buf, 0);
	ASSERT_NEQ(buf.tail, buf.head);
	ASSERT_UINT64_EQ(0, buf.tail);
	ASSERT_UINT64_EQ(1, watermark_count(&buf));

	stage_buffer_finish(&buf);
	return true;
}

static bool test_reclaim_partial(void) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	ASSERT_UINT64_EQ(0, vulkan_stage_buffer_alloc(&buf, 100, 1));
	push_watermark(&buf, 1);
	ASSERT_UINT64_EQ(100, vulkan_stage_buffer_alloc(&buf, 100, 1));
	push_watermark(&buf, 2);

	// Only the first watermark is reached.
	vulkan_stage_buffer_reclaim(&buf, 1);
	ASSERT_NEQ(buf.tail, buf.head);
	ASSERT_UINT64_EQ(100, buf.tail);
	ASSERT_UINT64_EQ(1, watermark_count(&buf));

	const struct wlr_vk_stage_watermark *remaining = buf.watermarks.data;
	ASSERT_UINT64_EQ(200, remaining[0].head);
	ASSERT_UINT64_EQ(2, remaining[0].timeline_point);

	stage_buffer_finish(&buf);
	return true;
}

static bool test_reclaim_all(void) {
	struct wlr_vk_stage_buffer buf;
	stage_buffer_init(&buf);

	ASSERT_UINT64_EQ(0, vulkan_stage_buffer_alloc(&buf, 100, 1));
	push_watermark(&buf, 1);
	ASSERT_UINT64_EQ(100, vulkan_stage_buffer_alloc(&buf, 100, 1));
	push_watermark(&buf, 2);
	ASSERT_UINT64_EQ(200, vulkan_stage_buffer_alloc(&buf, 100, 1));
	push_watermark(&buf, 3);

	vulkan_stage_buffer_reclaim(&buf, 100);
	ASSERT_UINT64_EQ(buf.tail, buf.head);
	ASSERT_UINT64_EQ(300, buf.tail);
	ASSERT_UINT64_EQ(0, watermark_count(&buf));

	stage_buffer_finish(&buf);
	return true;
}

int main(void) {
#ifdef NDEBUG
	fprintf(stderr, "NDEBUG must be disabled for tests\n");
	return 1;
#endif

	int r = 0;

	RUN_TEST(test_alloc_simple);
	RUN_TEST(test_alloc_alignment);
	RUN_TEST(test_alloc_limit);
	RUN_TEST(test_alloc_wrap);

	RUN_TEST(test_reclaim_empty);
	RUN_TEST(test_reclaim_pending_not_completed);
	RUN_TEST(test_reclaim_partial);
	RUN_TEST(test_reclaim_all);

	return r;
}
