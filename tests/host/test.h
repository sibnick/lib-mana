/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MANA_HOST_TEST_H
#define MANA_HOST_TEST_H

#include <stdio.h>

#define TEST_CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
			__FILE__, __LINE__, #condition); \
		return 1; \
	} \
} while (0)

struct host_test {
	const char *name;
	int (*run)(void);
};

static inline int host_test_run(const struct host_test *tests, size_t count)
{
	size_t i;
	size_t failed = 0;

	for (i = 0; i < count; i++) {
		int result = tests[i].run();

		printf("%s: %s\n", tests[i].name, result ? "FAIL" : "PASS");
		failed += result != 0;
	}
	printf("%zu tests, %zu failures\n", count, failed);
	return failed ? 1 : 0;
}

#endif
