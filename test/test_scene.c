#include <wlr/types/wlr_scene.h>
#include <types/wlr_scene.h>

#include <stdio.h>
#include <stdbool.h>

#define XSTR(s) STR(s)
#define STR(s) #s

#define EXPECT(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, XSTR(__LINE__) ": Expected " XSTR(expr) "\n"); \
		ok = false; \
	} \
} while (0)

#define ASSERT(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, XSTR(__LINE__) ": Asserted " XSTR(expr) "\n"); \
		return false; \
	} \
} while (0)

#define RUN(test) do { \
	bool test_ok = test(); \
	if (test_ok) { \
		printf("[ OK ] " XSTR(test) "\n"); \
	} else { \
		printf("[FAIL] " XSTR(test) "\n"); \
		ok = false; \
	} \
} while (0)

static bool _check_extents(int line, struct wlr_scene_node *node,
		int x_min, int y_min, int x_max, int y_max) {
	struct wlr_box extents;
	if (!scene_node_get_extents(node, &extents)) {
		fprintf(stderr, "%d: Expected non-empty extents\n", line);
		return false;
	}

	int ext_x_max = extents.x + extents.width;
	int ext_y_max = extents.y + extents.height;

	if (extents.x != x_min || extents.y != y_min ||
			ext_x_max != x_max || ext_y_max != y_max) {
		fprintf(stderr, "%d: Expected extents (%d, %d); (%d, %d), but got (%d, %d); (%d, %d)\n",
				line, x_min, y_min, x_max, y_max,
				extents.x, extents.y, ext_x_max, ext_y_max);

		return false;
	}

	return true;
}

#define EXPECT_EXTENTS(node, x_min, y_min, x_max, y_max) do { \
	if (!_check_extents(__LINE__, node, x_min, y_min, x_max, y_max)) { \
		ok = false; \
	} \
} while (0)

static bool test_scene_node_extents(struct wlr_scene_node *(*create_node)(
			struct wlr_scene_tree *parent, int width, int height)) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	// An empty tree should have no extents
	struct wlr_box extents;
	EXPECT(!scene_node_get_extents(&scene->tree.node, &extents));

	// Adding a node should update extents
	struct wlr_scene_node *node = create_node(&scene->tree, 5, 7);
	EXPECT_EXTENTS(&scene->tree.node, 0, 0, 5, 7);

	// Changing the node's position should update extents
	wlr_scene_node_set_position(node, 1, 2);
	EXPECT_EXTENTS(&scene->tree.node, 1, 2, 6, 9);

	// Adding another node should combine the extents
	struct wlr_scene_node *second_node = create_node(&scene->tree, 11, 23);
	EXPECT_EXTENTS(&scene->tree.node, 0, 0, 11, 23);

	// Moving the second node so that it doesn't overlap should grow the extents
	wlr_scene_node_set_position(second_node, 10, 10);
	EXPECT_EXTENTS(&scene->tree.node, 1, 2, 21, 33);

	// Removing the second node should restore the previous extents
	wlr_scene_node_destroy(second_node);
	EXPECT_EXTENTS(&scene->tree.node, 1, 2, 6, 9);

	// Disabling the first node should clear the extents
	wlr_scene_node_set_enabled(node, false);
	EXPECT(!scene_node_get_extents(&scene->tree.node, &extents));

	// Enabling the first node again should restore the extents
	wlr_scene_node_set_enabled(node, true);
	EXPECT_EXTENTS(&scene->tree.node, 1, 2, 6, 9);

	// Removing the first node should clear the extents
	wlr_scene_node_destroy(node);
	EXPECT(!scene_node_get_extents(&scene->tree.node, &extents));

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

static struct wlr_scene_node *create_scene_rect(
		struct wlr_scene_tree *parent, int width, int height) {
	return &wlr_scene_rect_create(parent, width, height, (float[4]){0})->node;
}

static struct wlr_scene_node *create_scene_tree_with_rect(
		struct wlr_scene_tree *parent, int width, int height) {
	struct wlr_scene_tree *tree = wlr_scene_tree_create(parent);
	wlr_scene_rect_create(tree, width, height, (float[4]){0});
	return &tree->node;
}

static struct wlr_scene_node *create_deep_scene_tree_with_rect(
		struct wlr_scene_tree *parent, int width, int height) {
	struct wlr_scene_tree *tree1 = wlr_scene_tree_create(parent);
	struct wlr_scene_tree *tree2 = wlr_scene_tree_create(tree1);
	wlr_scene_rect_create(tree2, width, height, (float[4]){0});
	return &tree1->node;
}

static bool test_scene_rect_extents(void) {
	return test_scene_node_extents(create_scene_rect);
}

static bool test_scene_tree_extents(void) {
	return test_scene_node_extents(create_scene_tree_with_rect);
}

static bool test_deep_scene_tree_extents(void) {
	return test_scene_node_extents(create_deep_scene_tree_with_rect);
}

static bool test_scene_extents_negative_coords(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	create_scene_rect(&scene->tree, 10, 10);

	struct wlr_scene_node *left_top = create_scene_rect(&scene->tree, 10, 10);
	wlr_scene_node_set_position(left_top, -5, -8);

	struct wlr_scene_node *right_bottom = create_scene_rect(&scene->tree, 10, 10);
	wlr_scene_node_set_position(right_bottom, 20, 25);

	EXPECT_EXTENTS(&scene->tree.node, -5, -8, 30, 35);

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Resizing a rect should update the extents.
static bool test_scene_extents_rect_set_size(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	struct wlr_scene_rect *rect = wlr_scene_rect_create(&scene->tree, 5, 5, (float[4]){0});
	EXPECT_EXTENTS(&scene->tree.node, 0, 0, 5, 5);

	wlr_scene_rect_set_size(rect, 20, 30);
	EXPECT_EXTENTS(&scene->tree.node, 0, 0, 20, 30);

	wlr_scene_rect_set_size(rect, 2, 3);
	EXPECT_EXTENTS(&scene->tree.node, 0, 0, 2, 3);

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Reparenting a node should update the extents of both the old and the new
// parent
static bool test_scene_extents_reparent(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	struct wlr_scene_tree *tree_a = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_set_position(&tree_a->node, 100, 100);

	struct wlr_scene_tree *tree_b = wlr_scene_tree_create(&scene->tree);

	struct wlr_scene_node *rect = create_scene_rect(tree_a, 10, 10);
	EXPECT_EXTENTS(&scene->tree.node, 100, 100, 110, 110);
	EXPECT_EXTENTS(&tree_a->node, 100, 100, 110, 110);

	wlr_scene_node_reparent(rect, tree_b);
	EXPECT_EXTENTS(&scene->tree.node, 0, 0, 10, 10);
	EXPECT_EXTENTS(&tree_b->node, 0, 0, 10, 10);
	EXPECT(!scene_node_get_extents(&tree_a->node, &(struct wlr_box){0}));

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Extents are reported in layout coordinates
static bool test_scene_extents_nested_offset(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	struct wlr_scene_tree *tree = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_set_position(&tree->node, 10, 20);

	struct wlr_scene_node *rect = create_scene_rect(tree, 5, 5);
	wlr_scene_node_set_position(rect, 3, 4);

	EXPECT_EXTENTS(&scene->tree.node, 13, 24, 18, 29);
	EXPECT_EXTENTS(&tree->node, 13, 24, 18, 29);

	// Moving the intermediate tree shifts the layout-space extents.
	wlr_scene_node_set_position(&tree->node, 0, 0);
	EXPECT_EXTENTS(&scene->tree.node, 3, 4, 8, 9);

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Toggling the enabled state of an intermediate tree updates the ancestors.
static bool test_scene_extents_toggle_nested(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	struct wlr_scene_tree *outer = wlr_scene_tree_create(&scene->tree);
	struct wlr_scene_tree *inner = wlr_scene_tree_create(outer);
	create_scene_rect(&scene->tree, 3, 3);
	create_scene_rect(inner, 50, 60);

	EXPECT_EXTENTS(&scene->tree.node, 0, 0, 50, 60);

	wlr_scene_node_set_enabled(&inner->node, false);
	EXPECT_EXTENTS(&scene->tree.node, 0, 0, 3, 3);
	EXPECT(!scene_node_get_extents(&outer->node, &(struct wlr_box){0}));

	wlr_scene_node_set_enabled(&inner->node, true);
	EXPECT_EXTENTS(&scene->tree.node, 0, 0, 50, 60);

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

int main(void) {
	bool ok = true;

	RUN(test_scene_rect_extents);
	RUN(test_scene_tree_extents);
	RUN(test_deep_scene_tree_extents);
	RUN(test_scene_extents_negative_coords);
	RUN(test_scene_extents_rect_set_size);
	RUN(test_scene_extents_reparent);
	RUN(test_scene_extents_nested_offset);
	RUN(test_scene_extents_toggle_nested);

	return ok ? 0 : 1;
}
