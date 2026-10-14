/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef TEST_UK_ARCH_UTIL_H
#define TEST_UK_ARCH_UTIL_H
#include <uk/essentials.h>
__u8 uk_arch_x86_64_inb(__u16 port);
void uk_arch_x86_64_outb(__u16 port, __u8 value);
#endif
