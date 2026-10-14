/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MANA_HOST_UK_ASSERT_H
#define MANA_HOST_UK_ASSERT_H
#include <stdio.h>
#include <stdlib.h>
#define UK_ASSERT(x) do { if (!(x)) { \
	fprintf(stderr, "Unikraft assertion: %s\n", #x); abort(); \
} } while (0)
#endif
