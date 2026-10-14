/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MANA_HOST_UK_PRINT_H
#define MANA_HOST_UK_PRINT_H
#include <stdio.h>
#define uk_pr_info(...) printf(__VA_ARGS__)
#define uk_pr_warn(...) fprintf(stderr, __VA_ARGS__)
#define uk_pr_err(...) fprintf(stderr, __VA_ARGS__)
#define uk_pr_debug(...) do { } while (0)
#endif
