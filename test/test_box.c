#include <stddef.h>
#include <wlr/util/box.h>

#include "util/testing.h"

static void test_box_empty(struct testing *t) {
	// NULL is empty
	t_expect_true(t, wlr_box_empty(NULL));

	// Zero width/height
	struct wlr_box box = { .x = 0, .y = 0, .width = 0, .height = 10 };
	t_expect_true(t, wlr_box_empty(&box));
	box = (struct wlr_box){ .x = 0, .y = 0, .width = 10, .height = 0 };
	t_expect_true(t, wlr_box_empty(&box));

	// Negative width/height
	box = (struct wlr_box){ .x = 0, .y = 0, .width = -1, .height = 10 };
	t_expect_true(t, wlr_box_empty(&box));
	box = (struct wlr_box){ .x = 0, .y = 0, .width = 10, .height = -1 };
	t_expect_true(t, wlr_box_empty(&box));

	// Valid box
	box = (struct wlr_box){ .x = 0, .y = 0, .width = 10, .height = 10 };
	t_expect_false(t, wlr_box_empty(&box));
}

static void test_box_intersection(struct testing *t) {
	struct wlr_box dest;

	// Overlapping
	struct wlr_box a = { .x = 0, .y = 0, .width = 100, .height = 100 };
	struct wlr_box b = { .x = 50, .y = 50, .width = 100, .height = 100 };
	t_expect_true(t, wlr_box_intersection(&dest, &a, &b));
	t_expect(t, dest.x, T_EQ, 50);
	t_expect(t, dest.y, T_EQ, 50);
	t_expect(t, dest.width, T_EQ, 50);
	t_expect(t, dest.height, T_EQ, 50);

	// Non-overlapping
	b = (struct wlr_box){ .x = 200, .y = 200, .width = 50, .height = 50 };
	t_expect_false(t, wlr_box_intersection(&dest, &a, &b));
	t_expect(t, dest.width, T_EQ, 0);
	t_expect(t, dest.height, T_EQ, 0);

	// Touching edges
	b = (struct wlr_box){ .x = 100, .y = 0, .width = 50, .height = 50 };
	t_expect_false(t, wlr_box_intersection(&dest, &a, &b));

	// Self-intersection
	t_expect_true(t, wlr_box_intersection(&dest, &a, &a));
	t_expect(t, dest.x, T_EQ, 0);
	t_expect(t, dest.y, T_EQ, 0);
	t_expect(t, dest.width, T_EQ, 100);
	t_expect(t, dest.height, T_EQ, 100);

	// Empty input
	struct wlr_box empty = { .x = 0, .y = 0, .width = 0, .height = 0 };
	t_expect_false(t, wlr_box_intersection(&dest, &a, &empty));

	// NULL input
	t_expect_false(t, wlr_box_intersection(&dest, &a, NULL));
	t_expect_false(t, wlr_box_intersection(&dest, NULL, &a));
}

static void test_box_intersects_box(struct testing *t) {
	// Overlapping
	struct wlr_box a = { .x = 0, .y = 0, .width = 100, .height = 100 };
	struct wlr_box b = { .x = 50, .y = 50, .width = 100, .height = 100 };
	t_expect_true(t, wlr_box_intersects(&a, &b));

	// Non-overlapping
	b = (struct wlr_box){ .x = 200, .y = 200, .width = 50, .height = 50 };
	t_expect_false(t, wlr_box_intersects(&a, &b));

	// Touching edges
	b = (struct wlr_box){ .x = 100, .y = 0, .width = 50, .height = 50 };
	t_expect_false(t, wlr_box_intersects(&a, &b));

	// Self-intersection
	t_expect_true(t, wlr_box_intersects(&a, &a));

	// Empty input
	struct wlr_box empty = { .x = 0, .y = 0, .width = 0, .height = 0 };
	t_expect_false(t, wlr_box_intersects(&a, &empty));

	// NULL input
	t_expect_false(t, wlr_box_intersects(&a, NULL));
	t_expect_false(t, wlr_box_intersects(NULL, &a));
}

static void test_box_contains_point(struct testing *t) {
	struct wlr_box box = { .x = 10, .y = 20, .width = 100, .height = 50 };

	// Interior point
	t_expect_true(t, wlr_box_contains_point(&box, 50, 40));

	// Inclusive lower bound
	t_expect_true(t, wlr_box_contains_point(&box, 10, 20));

	// Exclusive upper bound
	t_expect_false(t, wlr_box_contains_point(&box, 110, 70));
	t_expect_false(t, wlr_box_contains_point(&box, 110, 40));
	t_expect_false(t, wlr_box_contains_point(&box, 50, 70));

	// Outside
	t_expect_false(t, wlr_box_contains_point(&box, 5, 40));
	t_expect_false(t, wlr_box_contains_point(&box, 50, 15));

	// Empty box
	struct wlr_box empty = { .x = 0, .y = 0, .width = 0, .height = 0 };
	t_expect_false(t, wlr_box_contains_point(&empty, 0, 0));

	// NULL
	t_expect_false(t, wlr_box_contains_point(NULL, 0, 0));
}

static void test_box_contains_box(struct testing *t) {
	struct wlr_box outer = { .x = 0, .y = 0, .width = 100, .height = 100 };

	// Fully contained
	struct wlr_box inner = { .x = 10, .y = 10, .width = 50, .height = 50 };
	t_expect_true(t, wlr_box_contains_box(&outer, &inner));

	// Self-containment
	t_expect_true(t, wlr_box_contains_box(&outer, &outer));

	// Partial overlap — not contained
	struct wlr_box partial = { .x = 50, .y = 50, .width = 100, .height = 100 };
	t_expect_false(t, wlr_box_contains_box(&outer, &partial));

	// Empty inner
	struct wlr_box empty = { .x = 0, .y = 0, .width = 0, .height = 0 };
	t_expect_false(t, wlr_box_contains_box(&outer, &empty));

	// Empty outer
	t_expect_false(t, wlr_box_contains_box(&empty, &inner));

	// NULL
	t_expect_false(t, wlr_box_contains_box(&outer, NULL));
	t_expect_false(t, wlr_box_contains_box(NULL, &outer));
}

static const struct testing_case tests[] = {
	T_ENTRY(test_box_empty),
	T_ENTRY(test_box_intersection),
	T_ENTRY(test_box_intersects_box),
	T_ENTRY(test_box_contains_point),
	T_ENTRY(test_box_contains_box),
	{ NULL, NULL },
};

int main(void) {
	return t_main(tests);
}
