/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MANA_HOST_MOCK_H
#define MANA_HOST_MOCK_H

#include <stddef.h>
#include <stdint.h>

#define HOST_MOCK_BAR_SIZE 8192U
#define HOST_MOCK_DMA_SLOTS 16U

enum host_fault_point {
	HOST_FAULT_ALLOC,
	HOST_FAULT_MMIO_READ,
	HOST_FAULT_MMIO_WRITE,
	HOST_FAULT_COUNT
};

struct host_fault {
	size_t calls;
	size_t fail_at;
	int error;
};

struct host_dma {
	void *address;
	size_t size;
	uint64_t device_address;
};

struct host_mock {
	struct host_fault faults[HOST_FAULT_COUNT];
	unsigned char bar[HOST_MOCK_BAR_SIZE];
	struct host_dma dma[HOST_MOCK_DMA_SLOTS];
	size_t live_allocations;
	uint64_t next_device_address;
	uint64_t now_us;
};

void host_mock_init(struct host_mock *mock);
size_t host_mock_cleanup(struct host_mock *mock);
int host_fault_arm(struct host_mock *mock, enum host_fault_point point,
		   size_t fail_at, int error);
int host_fault_hit(struct host_mock *mock, enum host_fault_point point);
int host_mock_alloc(struct host_mock *mock, size_t size, size_t alignment,
		    struct host_dma *result);
int host_mock_free(struct host_mock *mock, const struct host_dma *allocation);
int host_mock_read(struct host_mock *mock, size_t offset, size_t width,
		   uint64_t *value);
int host_mock_write(struct host_mock *mock, size_t offset, size_t width,
		    uint64_t value);
int host_mock_advance(struct host_mock *mock, uint64_t elapsed_us);

#endif
