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

struct box_intersection_case {
	const char *name;
	const struct wlr_box *a, *b; // may be NULL
	bool intersects;
	struct wlr_box dest;
};

static const struct wlr_box square = { .x = 0, .y = 0, .width = 100, .height = 100 };

static const struct box_intersection_case box_intersection_cases[] = {
	{
		.name = "overlapping",
		.a = &square,
		.b = &(struct wlr_box){ .x = 50, .y = 50, .width = 100, .height = 100 },
		.intersects = true,
		.dest = { .x = 50, .y = 50, .width = 50, .height = 50 },
	},
	{
		.name = "non_overlapping",
		.a = &square,
		.b = &(struct wlr_box){ .x = 200, .y = 200, .width = 50, .height = 50 },
	},
	{
		.name = "touching_edges",
		.a = &square,
		.b = &(struct wlr_box){ .x = 100, .y = 0, .width = 50, .height = 50 },
	},
	{
		.name = "self",
		.a = &square,
		.b = &square,
		.intersects = true,
		.dest = { .x = 0, .y = 0, .width = 100, .height = 100 },
	},
	{
		.name = "empty_input",
		.a = &square,
		.b = &(struct wlr_box){ .x = 0, .y = 0, .width = 0, .height = 0 },
	},
	{
		.name = "null_b",
		.a = &square,
		.b = NULL,
	},
	{
		.name = "null_a",
		.a = NULL,
		.b = &square,
	},
};

static void run_box_intersection(struct testing *t, void *arg) {
	const struct box_intersection_case *c = arg;

	struct wlr_box dest;
	bool intersects = wlr_box_intersection(&dest, c->a, c->b);

	t_assert_true(t, intersects == c->intersects);
	t_expect(t, dest.x, T_EQ, c->dest.x);
	t_expect(t, dest.y, T_EQ, c->dest.y);
	t_expect(t, dest.width, T_EQ, c->dest.width);
	t_expect(t, dest.height, T_EQ, c->dest.height);
}

static void test_box_intersection(struct testing *t) {
	size_t n = sizeof(box_intersection_cases) / sizeof(box_intersection_cases[0]);
	for (size_t i = 0; i < n; i++) {
		const struct box_intersection_case *c = &box_intersection_cases[i];
		t_run(t, c->name, run_box_intersection, (void *)c);
	}
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
