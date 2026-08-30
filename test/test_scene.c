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

#define MAX_VISITS 8

struct visit {
	struct wlr_scene_node *node;
	int sx, sy;
};

struct visit_data {
	struct visit visits[MAX_VISITS];
	size_t len;
	// If non-NULL, the iterator stops once this node has been visited
	struct wlr_scene_node *stop_at;
};

static bool collect_iterator(struct wlr_scene_node *node, int sx, int sy, void *data) {
	struct visit_data *visit_data = data;

	if (visit_data->len < MAX_VISITS) {
		visit_data->visits[visit_data->len] = (struct visit){
			.node = node,
			.sx = sx,
			.sy = sy,
		};
	}
	visit_data->len++;

	return visit_data->stop_at == node;
}

static bool _check_visit(int line, struct visit_data *visit_data, size_t index,
		struct wlr_scene_node *node, int sx, int sy) {
	if (index >= visit_data->len || index >= MAX_VISITS) {
		fprintf(stderr, "%d: Expected at least %zu visits, but got %zu\n",
			line, index + 1, visit_data->len);
		return false;
	}

	struct visit *visit = &visit_data->visits[index];
	if (visit->node != node || visit->sx != sx || visit->sy != sy) {
		fprintf(stderr, "%d: Expected visit %zu to be node %p at (%d, %d), "
			"but got node %p at (%d, %d)\n", line, index, (void *)node, sx, sy,
			(void *)visit->node, visit->sx, visit->sy);
		return false;
	}

	return true;
}

#define EXPECT_VISIT(visit_data, index, node, sx, sy) do { \
	if (!_check_visit(__LINE__, visit_data, index, node, sx, sy)) { \
		ok = false; \
	} \
} while (0)

// Only nodes intersecting the box are visited, in layout coordinates
static bool test_nodes_in_box_intersection(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	struct wlr_scene_node *inside = create_scene_rect(&scene->tree, 10, 10);
	struct wlr_scene_node *outside = create_scene_rect(&scene->tree, 10, 10);
	wlr_scene_node_set_position(outside, 100, 100);

	struct visit_data visit_data = {0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 0, 0, 10, 10 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 1);
	EXPECT_VISIT(&visit_data, 0, inside, 0, 0);

	// A box touching the right edge of a node doesn't intersect it
	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 10, 0, 10, 10 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 0);

	// A single pixel overlap is enough
	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 9, 9, 1, 1 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 1);
	EXPECT_VISIT(&visit_data, 0, inside, 0, 0);

	// An empty box never intersects anything
	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 0, 0, 0, 0 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 0);

	// A box large enough covers both nodes
	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 0, 0, 200, 200 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 2);

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Zero-sized nodes are never visited
static bool test_nodes_in_box_empty_node(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	create_scene_rect(&scene->tree, 0, 0);

	struct visit_data visit_data = {0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 0, 0, 10, 10 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 0);

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Buffer nodes are visited, using their destination size
static bool test_nodes_in_box_buffer(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	struct wlr_scene_buffer *buffer = wlr_scene_buffer_create(&scene->tree, NULL);
	ASSERT(buffer);

	// A buffer without a buffer nor a destination size has no size
	struct visit_data visit_data = {0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 0, 0, 100, 100 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 0);

	wlr_scene_buffer_set_dest_size(buffer, 10, 10);
	wlr_scene_node_set_position(&buffer->node, 5, 5);

	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 0, 0, 100, 100 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 1);
	EXPECT_VISIT(&visit_data, 0, &buffer->node, 5, 5);

	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 0, 0, 5, 5 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 0);

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Nodes are visited from top to bottom
static bool test_nodes_in_box_order(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	struct wlr_scene_node *bottom = create_scene_rect(&scene->tree, 10, 10);
	struct wlr_scene_node *middle = create_scene_rect(&scene->tree, 10, 10);
	struct wlr_scene_node *top = create_scene_rect(&scene->tree, 10, 10);

	struct visit_data visit_data = {0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 0, 0, 10, 10 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 3);
	EXPECT_VISIT(&visit_data, 0, top, 0, 0);
	EXPECT_VISIT(&visit_data, 1, middle, 0, 0);
	EXPECT_VISIT(&visit_data, 2, bottom, 0, 0);

	// Restacking changes the order
	wlr_scene_node_raise_to_top(bottom);

	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 0, 0, 10, 10 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 3);
	EXPECT_VISIT(&visit_data, 0, bottom, 0, 0);
	EXPECT_VISIT(&visit_data, 1, top, 0, 0);
	EXPECT_VISIT(&visit_data, 2, middle, 0, 0);

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Tree nodes are descended into, but never passed to the iterator themselves
static bool test_nodes_in_box_nested(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	struct wlr_scene_tree *outer = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_set_position(&outer->node, 10, 20);

	struct wlr_scene_tree *inner = wlr_scene_tree_create(outer);
	wlr_scene_node_set_position(&inner->node, 3, 4);

	struct wlr_scene_node *rect = create_scene_rect(inner, 5, 5);
	wlr_scene_node_set_position(rect, 1, 1);

	struct visit_data visit_data = {0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 0, 0, 100, 100 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 1);
	EXPECT_VISIT(&visit_data, 0, rect, 14, 25);

	// The box is in layout coordinates, not relative to the node
	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node,
		&(struct wlr_box){ 0, 0, 14, 25 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 0);

	// Starting from a nested tree offsets the coordinates the same way
	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(&inner->node,
		&(struct wlr_box){ 0, 0, 100, 100 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 1);
	EXPECT_VISIT(&visit_data, 0, rect, 14, 25);

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Only the nodes below the starting node are visited
static bool test_nodes_in_box_subtree(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	create_scene_rect(&scene->tree, 10, 10);

	struct wlr_scene_tree *tree = wlr_scene_tree_create(&scene->tree);
	struct wlr_scene_node *child = create_scene_rect(tree, 10, 10);

	struct visit_data visit_data = {0};
	EXPECT(!scene_nodes_in_box(&tree->node,
		&(struct wlr_box){ 0, 0, 10, 10 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 1);
	EXPECT_VISIT(&visit_data, 0, child, 0, 0);

	// A leaf node can be used as the starting node too
	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(child,
		&(struct wlr_box){ 0, 0, 10, 10 }, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 1);
	EXPECT_VISIT(&visit_data, 0, child, 0, 0);

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Disabled nodes and the children of disabled trees are skipped
static bool test_nodes_in_box_disabled(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	struct wlr_scene_node *rect = create_scene_rect(&scene->tree, 10, 10);

	struct wlr_scene_tree *tree = wlr_scene_tree_create(&scene->tree);
	struct wlr_scene_node *child = create_scene_rect(tree, 10, 10);

	struct wlr_box box = { 0, 0, 10, 10 };

	wlr_scene_node_set_enabled(rect, false);

	struct visit_data visit_data = {0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node, &box, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 1);
	EXPECT_VISIT(&visit_data, 0, child, 0, 0);

	// Disabling the tree hides its children
	wlr_scene_node_set_enabled(&tree->node, false);

	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node, &box, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 0);

	// The starting node itself is checked as well
	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(&tree->node, &box, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 0);

	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(rect, &box, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 0);

	wlr_scene_node_set_enabled(&tree->node, true);

	visit_data = (struct visit_data){0};
	EXPECT(!scene_nodes_in_box(&scene->tree.node, &box, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 1);
	EXPECT_VISIT(&visit_data, 0, child, 0, 0);

	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// An iterator returning true stops the iteration
static bool test_nodes_in_box_early_exit(void) {
	bool ok = true;
	struct wlr_scene *scene = wlr_scene_create();
	ASSERT(scene);

	struct wlr_scene_tree *tree = wlr_scene_tree_create(&scene->tree);
	struct wlr_scene_node *bottom = create_scene_rect(tree, 10, 10);
	struct wlr_scene_node *middle = create_scene_rect(tree, 10, 10);
	struct wlr_scene_node *top = create_scene_rect(tree, 10, 10);

	struct wlr_box box = { 0, 0, 10, 10 };

	// Stopping at the topmost node visits nothing else
	struct visit_data visit_data = { .stop_at = top };
	EXPECT(scene_nodes_in_box(&scene->tree.node, &box, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 1);
	EXPECT_VISIT(&visit_data, 0, top, 0, 0);

	// Stopping in the middle unwinds through the parent trees
	visit_data = (struct visit_data){ .stop_at = middle };
	EXPECT(scene_nodes_in_box(&scene->tree.node, &box, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 2);
	EXPECT_VISIT(&visit_data, 0, top, 0, 0);
	EXPECT_VISIT(&visit_data, 1, middle, 0, 0);

	// Stopping at the last node still reports that the iteration was stopped
	visit_data = (struct visit_data){ .stop_at = bottom };
	EXPECT(scene_nodes_in_box(&scene->tree.node, &box, collect_iterator, &visit_data));
	EXPECT(visit_data.len == 3);

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
	RUN(test_nodes_in_box_intersection);
	RUN(test_nodes_in_box_empty_node);
	RUN(test_nodes_in_box_buffer);
	RUN(test_nodes_in_box_order);
	RUN(test_nodes_in_box_nested);
	RUN(test_nodes_in_box_subtree);
	RUN(test_nodes_in_box_disabled);
	RUN(test_nodes_in_box_early_exit);

	return ok ? 0 : 1;
}
