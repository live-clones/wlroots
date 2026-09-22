#include <stddef.h>
#include <wlr/util/box.h>
#include "util/unit_test.h"

static bool test_box_empty(void) {
	// NULL is empty
	ASSERT_TRUE(wlr_box_empty(NULL));

	// Zero width/height
	struct wlr_box box = { .x = 0, .y = 0, .width = 0, .height = 10 };
	ASSERT_TRUE(wlr_box_empty(&box));
	box = (struct wlr_box){ .x = 0, .y = 0, .width = 10, .height = 0 };
	ASSERT_TRUE(wlr_box_empty(&box));

	// Negative width/height
	box = (struct wlr_box){ .x = 0, .y = 0, .width = -1, .height = 10 };
	ASSERT_TRUE(wlr_box_empty(&box));
	box = (struct wlr_box){ .x = 0, .y = 0, .width = 10, .height = -1 };
	ASSERT_TRUE(wlr_box_empty(&box));

	// Valid box
	box = (struct wlr_box){ .x = 0, .y = 0, .width = 10, .height = 10 };
	ASSERT_FALSE(wlr_box_empty(&box));

	return true;
}

static bool test_box_intersection(void) {
	struct wlr_box dest;

	// Overlapping
	struct wlr_box a = { .x = 0, .y = 0, .width = 100, .height = 100 };
	struct wlr_box b = { .x = 50, .y = 50, .width = 100, .height = 100 };
	ASSERT_TRUE(wlr_box_intersection(&dest, &a, &b));
	ASSERT_INT_EQ(50, dest.x);
	ASSERT_INT_EQ(50, dest.y);
	ASSERT_INT_EQ(50, dest.width);
	ASSERT_INT_EQ(50, dest.height);

	// Non-overlapping
	b = (struct wlr_box){ .x = 200, .y = 200, .width = 50, .height = 50 };
	ASSERT_FALSE(wlr_box_intersection(&dest, &a, &b));
	ASSERT_INT_EQ(0, dest.width);
	ASSERT_INT_EQ(0, dest.height);

	// Touching edges
	b = (struct wlr_box){ .x = 100, .y = 0, .width = 50, .height = 50 };
	ASSERT_FALSE(wlr_box_intersection(&dest, &a, &b));

	// Self-intersection
	ASSERT_TRUE(wlr_box_intersection(&dest, &a, &a));
	ASSERT_INT_EQ(a.x, dest.x);
	ASSERT_INT_EQ(a.y, dest.y);
	ASSERT_INT_EQ(a.width, dest.width);
	ASSERT_INT_EQ(a.height, dest.height);

	// Empty input
	struct wlr_box empty = { .x = 0, .y = 0, .width = 0, .height = 0 };
	ASSERT_FALSE(wlr_box_intersection(&dest, &a, &empty));

	// NULL input
	ASSERT_FALSE(wlr_box_intersection(&dest, &a, NULL));
	ASSERT_FALSE(wlr_box_intersection(&dest, NULL, &a));

	return true;
}

static bool test_box_intersects_box(void) {
	// Overlapping
	struct wlr_box a = { .x = 0, .y = 0, .width = 100, .height = 100 };
	struct wlr_box b = { .x = 50, .y = 50, .width = 100, .height = 100 };
	ASSERT_TRUE(wlr_box_intersects(&a, &b));

	// Non-overlapping
	b = (struct wlr_box){ .x = 200, .y = 200, .width = 50, .height = 50 };
	ASSERT_FALSE(wlr_box_intersects(&a, &b));

	// Touching edges
	b = (struct wlr_box){ .x = 100, .y = 0, .width = 50, .height = 50 };
	ASSERT_FALSE(wlr_box_intersects(&a, &b));

	// Self-intersection
	ASSERT_TRUE(wlr_box_intersects(&a, &a));

	// Empty input
	struct wlr_box empty = { .x = 0, .y = 0, .width = 0, .height = 0 };
	ASSERT_FALSE(wlr_box_intersects(&a, &empty));

	// NULL input
	ASSERT_FALSE(wlr_box_intersects(&a, NULL));
	ASSERT_FALSE(wlr_box_intersects(NULL, &a));

	return true;
}

static bool test_box_contains_point(void) {
	struct wlr_box box = { .x = 10, .y = 20, .width = 100, .height = 50 };

	// Interior point
	ASSERT_TRUE(wlr_box_contains_point(&box, 50, 40));

	// Inclusive lower bound
	ASSERT_TRUE(wlr_box_contains_point(&box, 10, 20));

	// Exclusive upper bound
	ASSERT_FALSE(wlr_box_contains_point(&box, 110, 70));
	ASSERT_FALSE(wlr_box_contains_point(&box, 110, 40));
	ASSERT_FALSE(wlr_box_contains_point(&box, 50, 70));

	// Outside
	ASSERT_FALSE(wlr_box_contains_point(&box, 5, 40));
	ASSERT_FALSE(wlr_box_contains_point(&box, 50, 15));

	// Empty box
	struct wlr_box empty = { .x = 0, .y = 0, .width = 0, .height = 0 };
	ASSERT_FALSE(wlr_box_contains_point(&empty, 0, 0));

	// NULL
	ASSERT_FALSE(wlr_box_contains_point(NULL, 0, 0));

	return true;
}

static bool test_box_contains_box(void) {
	struct wlr_box outer = { .x = 0, .y = 0, .width = 100, .height = 100 };

	// Fully contained
	struct wlr_box inner = { .x = 10, .y = 10, .width = 50, .height = 50 };
	ASSERT_TRUE(wlr_box_contains_box(&outer, &inner));

	// Self-containment
	ASSERT_TRUE(wlr_box_contains_box(&outer, &outer));

	// Partial overlap — not contained
	struct wlr_box partial = { .x = 50, .y = 50, .width = 100, .height = 100 };
	ASSERT_FALSE(wlr_box_contains_box(&outer, &partial));

	// Empty inner
	struct wlr_box empty = { .x = 0, .y = 0, .width = 0, .height = 0 };
	ASSERT_FALSE(wlr_box_contains_box(&outer, &empty));

	// Empty outer
	ASSERT_FALSE(wlr_box_contains_box(&empty, &inner));

	// NULL
	ASSERT_FALSE(wlr_box_contains_box(&outer, NULL));
	ASSERT_FALSE(wlr_box_contains_box(NULL, &outer));

	return true;
}

int main(void) {
#ifdef NDEBUG
	fprintf(stderr, "NDEBUG must be disabled for tests\n");
	return 1;
#endif

	int r = 0;

	RUN_TEST(test_box_empty);
	RUN_TEST(test_box_intersection);
	RUN_TEST(test_box_intersects_box);
	RUN_TEST(test_box_contains_point);
	RUN_TEST(test_box_contains_box);

	return r;
}
