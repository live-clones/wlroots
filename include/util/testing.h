#ifndef UTIL_TESTING_H
#define UTIL_TESTING_H

#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>

struct testing;

typedef void (*testing_fn)(struct testing *t);
typedef void (*testing_sub_fn)(struct testing *t, void *arg);
typedef void (*testing_cleanup_fn)(void *arg);

struct testing_case {
	const char *name;
	testing_fn fn;
};

/**
 * A registration entry named after the function itself.
 */
#define T_ENTRY(fn) { #fn, fn }

#define t_log(t, ...) \
	_testing_log((t), __LINE__, __VA_ARGS__)
#define t_error(t, ...) \
	_testing_error((t), __LINE__, __VA_ARGS__)
#define t_fatal(t, ...) \
	_testing_fatal((t), __LINE__, __VA_ARGS__)
#define t_skip(t, ...) \
	_testing_skip((t), __LINE__, __VA_ARGS__)

/**
 * Report an error if expr is not true or false, respectively, and carry on.
 *
 * Evaluates to true if expr had the expected value.
 */
#define t_expect_true(t, expr) \
	_testing_expect_true((t), __LINE__, #expr, (expr))
#define t_expect_false(t, expr) \
	_testing_expect_false((t), __LINE__, #expr, (expr))

/**
 * Report an error if expr is not true or false, respectively, and stop the
 * test like t_fatal().
 */
#define t_assert_true(t, expr) \
	_testing_assert_true((t), __LINE__, #expr, (expr))
#define t_assert_false(t, expr) \
	_testing_assert_false((t), __LINE__, #expr, (expr))

enum testing_relation {
	T_EQ,
	T_NEQ,
	T_LT,
	T_GT,
	T_LE,
	T_GE,
};

#define TESTING_COMPARE_FN(actual) \
	_Generic((actual), \
		char: _testing_compare_int64, \
		signed char: _testing_compare_int64, \
		unsigned char: _testing_compare_uint64, \
		short: _testing_compare_int64, \
		unsigned short: _testing_compare_uint64, \
		int: _testing_compare_int64, \
		unsigned int: _testing_compare_uint64, \
		long: _testing_compare_int64, \
		unsigned long: _testing_compare_uint64, \
		long long: _testing_compare_int64, \
		unsigned long long: _testing_compare_uint64, \
		float: _testing_compare_double, \
		double: _testing_compare_double)

/**
 * Report an error if actual OP expected is false, and carry on. OP is
 * T_EQ, T_NEQ, T_LT, T_GT, T_LE or T_GE.
 *
 * Integers are compared as int64_t or uint64_t, depending on the signedness of
 * actual, so write e.g. UINT32_MAX rather than -1 for a narrower unsigned type.
 *
 * Evaluates to true if actual OP expected is true.
 */
#define t_expect(t, actual, relation, expected) \
	TESTING_COMPARE_FN(actual)((t), __LINE__, false, (actual), (relation), \
		(expected), #actual, #expected)

/**
 * Report an error if actual OP expected is false, and stop the test like
 * t_fatal(). See t_expect().
 */
#define t_assert(t, actual, relation, expected) \
	TESTING_COMPARE_FN(actual)((t), __LINE__, true, (actual), (relation), \
		(expected), #actual, #expected)

// The structures below are private; use the functions that follow.

struct testing_cleanup {
	testing_cleanup_fn fn;
	void *arg;
	struct testing_cleanup *next;
};

struct testing {
	struct testing *parent; // may be NULL
	char *name;
	bool failed;
	bool skipped;
	bool running;
	bool in_subtest;
	jmp_buf env;
	struct testing_cleanup *cleanups; // may be NULL
	int depth;
};

static inline void *_testing_check(void *p, const char *what) {
	if (!p) {
		perror(what);
		abort();
	}
	return p;
}

static inline struct testing *_testing_create(struct testing *parent,
		const char *name) {
	struct testing *t = _testing_check(calloc(1, sizeof(*t)), "testing: calloc");

	size_t len = strlen(name) + 1;
	if (parent) {
		len += strlen(parent->name) + 1;
	}
	t->name = _testing_check(calloc(len, 1), "testing: calloc");
	if (parent) {
		snprintf(t->name, len, "%s/%s", parent->name, name);
	} else {
		memcpy(t->name, name, len);
	}

	t->parent = parent;
	if (parent) {
		t->depth = parent->depth + 1;
	}
	return t;
}

static inline void _testing_destroy(struct testing *t) {
	if (!t) {
		return;
	}
	free(t->name);
	free(t);
}

static inline void _testing_vlog(struct testing *t, int line, const char *fmt,
		va_list ap) {
	printf("%*sline %d: ", (t->depth + 1) * 4, "", line);
	vprintf(fmt, ap);
	size_t len = strlen(fmt);
	if (len == 0 || fmt[len - 1] != '\n') {
		putchar('\n');
	}
	fflush(stdout);
}

static inline void t_fail(struct testing *t) {
	for (; t; t = t->parent) {
		t->failed = true;
	}
}

noreturn static inline void _testing_unwind(struct testing *t) {
	if (!t->running || t->in_subtest) {
		fflush(stdout);
		fprintf(stderr, "testing: %s stopped from outside its own function\n",
			t->name);
		abort();
	}
	longjmp(t->env, 1);
}

/**
 * Marks the test as failed and stops it.
 */
noreturn static inline void t_fail_now(struct testing *t) {
	t_fail(t);
	_testing_unwind(t);
}

/**
 * Marks the test as skipped and stops it.
 */
noreturn static inline void t_skip_now(struct testing *t) {
	t->skipped = true;
	_testing_unwind(t);
}

static inline void _testing_log(struct testing *t, int line,
		const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	_testing_vlog(t, line, fmt, ap);
	va_end(ap);
}

static inline void _testing_error(struct testing *t, int line,
		const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	_testing_vlog(t, line, fmt, ap);
	va_end(ap);
	t_fail(t);
}

noreturn static inline void _testing_fatal(struct testing *t, int line,
		const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	_testing_vlog(t, line, fmt, ap);
	va_end(ap);
	t_fail_now(t);
}

noreturn static inline void _testing_skip(struct testing *t, int line,
		const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	_testing_vlog(t, line, fmt, ap);
	va_end(ap);
	t_skip_now(t);
}

static inline bool _testing_expect_true(struct testing *t, int line,
		const char *expr, bool value) {
	if (!value) {
		_testing_error(t, line, "%s = false, want true", expr);
	}
	return value;
}

static inline bool _testing_expect_false(struct testing *t, int line,
		const char *expr, bool value) {
	if (value) {
		_testing_error(t, line, "%s = true, want false", expr);
	}
	return !value;
}

static inline void _testing_assert_true(struct testing *t, int line,
		const char *expr, bool value) {
	if (!value) {
		_testing_fatal(t, line, "%s = false, want true", expr);
	}
}

static inline void _testing_assert_false(struct testing *t, int line,
		const char *expr, bool value) {
	if (value) {
		_testing_fatal(t, line, "%s = true, want false", expr);
	}
}

static inline const char *_testing_relation_str(enum testing_relation rel) {
	switch (rel) {
	case T_EQ:
		return "==";
	case T_NEQ:
		return "!=";
	case T_LT:
		return "<";
	case T_GT:
		return ">";
	case T_LE:
		return "<=";
	case T_GE:
		return ">=";
	}
	abort();
}

static inline bool _testing_compare_int64(struct testing *t, int line, bool fatal,
		int64_t actual, enum testing_relation rel, int64_t expected,
		const char *actual_str, const char *expected_str) {
	bool ok;
	switch (rel) {
	case T_EQ:
		ok = actual == expected;
		break;
	case T_NEQ:
		ok = actual != expected;
		break;
	case T_LT:
		ok = actual < expected;
		break;
	case T_GT:
		ok = actual > expected;
		break;
	case T_LE:
		ok = actual <= expected;
		break;
	case T_GE:
		ok = actual >= expected;
		break;
	default:
		abort();
	}
	if (ok) {
		return true;
	}

	const char *op = _testing_relation_str(rel);
	if (fatal) {
		_testing_fatal(t, line, "want %s %s %s, got %" PRId64 " %s %" PRId64,
			actual_str, op, expected_str, actual, op, expected);
	}
	_testing_error(t, line, "want %s %s %s, got %" PRId64 " %s %" PRId64,
		actual_str, op, expected_str, actual, op, expected);
	return false;
}

static inline bool _testing_compare_uint64(struct testing *t, int line, bool fatal,
		uint64_t actual, enum testing_relation rel, uint64_t expected,
		const char *actual_str, const char *expected_str) {
	bool ok;
	switch (rel) {
	case T_EQ:
		ok = actual == expected;
		break;
	case T_NEQ:
		ok = actual != expected;
		break;
	case T_LT:
		ok = actual < expected;
		break;
	case T_GT:
		ok = actual > expected;
		break;
	case T_LE:
		ok = actual <= expected;
		break;
	case T_GE:
		ok = actual >= expected;
		break;
	default:
		abort();
	}
	if (ok) {
		return true;
	}

	const char *op = _testing_relation_str(rel);
	if (fatal) {
		_testing_fatal(t, line, "want %s %s %s, got %" PRIu64 " %s %" PRIu64,
			actual_str, op, expected_str, actual, op, expected);
	}
	_testing_error(t, line, "want %s %s %s, got %" PRIu64 " %s %" PRIu64,
		actual_str, op, expected_str, actual, op, expected);
	return false;
}

static inline bool _testing_compare_double(struct testing *t, int line, bool fatal,
		double actual, enum testing_relation rel, double expected,
		const char *actual_str, const char *expected_str) {
	bool ok;
	switch (rel) {
	case T_EQ:
		ok = actual == expected;
		break;
	case T_NEQ:
		ok = actual != expected;
		break;
	case T_LT:
		ok = actual < expected;
		break;
	case T_GT:
		ok = actual > expected;
		break;
	case T_LE:
		ok = actual <= expected;
		break;
	case T_GE:
		ok = actual >= expected;
		break;
	default:
		abort();
	}
	if (ok) {
		return true;
	}

	const char *op = _testing_relation_str(rel);
	if (fatal) {
		_testing_fatal(t, line, "want %s %s %s, got %.17g %s %.17g",
			actual_str, op, expected_str, actual, op, expected);
	}
	_testing_error(t, line, "want %s %s %s, got %.17g %s %.17g",
		actual_str, op, expected_str, actual, op, expected);
	return false;
}

/**
 * Calls fn(arg) in reverse order when the test finishes.
 */
static inline void t_cleanup(struct testing *t, testing_cleanup_fn fn,
		void *arg) {
	struct testing_cleanup *c = _testing_check(calloc(1, sizeof(*c)), "testing: calloc");
	c->fn = fn;
	c->arg = arg;
	c->next = t->cleanups;
	t->cleanups = c;
}

// Calls fn and then the cleanups
static inline void _testing_invoke(struct testing *t, testing_sub_fn fn, void *arg) {
	t->running = true;
	if (!setjmp(t->env)) {
		fn(t, arg);
	}
	while (t->cleanups) {
		struct testing_cleanup *c = t->cleanups;
		t->cleanups = c->next;
		if (!setjmp(t->env)) {
			c->fn(c->arg);
		}
		free(c);
	}
	t->running = false;
}

static inline void _testing_report(struct testing *t) {
	const char *status = "PASS";
	if (t->failed) {
		status = "FAIL";
	} else if (t->skipped) {
		status = "SKIP";
	}

	printf("%*s--- %s: %s\n", t->depth * 4, "", status, t->name);
	fflush(stdout);
}

static inline bool _testing_start(struct testing *parent, const char *name,
		testing_sub_fn fn, void *arg) {
	struct testing *t = _testing_create(parent, name);
	printf("%*s=== RUN   %s\n", t->depth * 4, "", t->name);
	fflush(stdout);

	if (parent) {
		parent->in_subtest = true;
	}
	_testing_invoke(t, fn, arg);
	if (parent) {
		parent->in_subtest = false;
	}
	_testing_report(t);

	bool ok = !t->failed;
	_testing_destroy(t);
	return ok;
}

/**
 * Runs fn(sub, arg) as the subtest "name". Returns false on failure.
 */
static inline bool t_run(struct testing *t, const char *name,
		testing_sub_fn fn, void *arg) {
	return _testing_start(t, name, fn, arg);
}

static inline void _testing_case_body(struct testing *t, void *arg) {
	const struct testing_case *tc = arg;
	tc->fn(t);
}

/**
 * Runs the tests, an array ending with a NULL name.
 *
 * Returns the exit status for main().
 */
static inline int t_main(const struct testing_case *tests) {
	bool ok = true;
	for (const struct testing_case *tc = tests; tc->name; tc++) {
		if (!_testing_start(NULL, tc->name, _testing_case_body, (void *)tc)) {
			ok = false;
		}
	}

	puts(ok ? "PASS" : "FAIL");
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif
