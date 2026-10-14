/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void host_mock_init(struct host_mock *mock)
{
	memset(mock, 0, sizeof(*mock));
	mock->next_device_address = UINT64_C(0x100000000);
}

size_t host_mock_cleanup(struct host_mock *mock)
{
	size_t i;
	size_t leaked = mock->live_allocations;

	for (i = 0; i < HOST_MOCK_DMA_SLOTS; i++)
		free(mock->dma[i].address);
	host_mock_init(mock);
	return leaked;
}

int host_fault_arm(struct host_mock *mock, enum host_fault_point point,
		   size_t fail_at, int error)
{
	if ((unsigned int)point >= HOST_FAULT_COUNT ||
	    (fail_at != 0 && error >= 0))
		return -EINVAL;
	mock->faults[point] = (struct host_fault){0, fail_at, error};
	return 0;
}

int host_fault_hit(struct host_mock *mock, enum host_fault_point point)
{
	struct host_fault *fault;

	if ((unsigned int)point >= HOST_FAULT_COUNT)
		return -EINVAL;
	fault = &mock->faults[point];
	if (fault->calls == SIZE_MAX)
		return -EOVERFLOW;
	fault->calls++;
	return fault->fail_at && fault->calls == fault->fail_at ?
		fault->error : 0;
}

int host_mock_alloc(struct host_mock *mock, size_t size, size_t alignment,
		    struct host_dma *result)
{
	size_t i;
	uint64_t address;
	void *memory;
	int error;

	if (!result || !size || alignment < sizeof(void *) ||
	    (alignment & (alignment - 1)) || size % alignment)
		return -EINVAL;
	error = host_fault_hit(mock, HOST_FAULT_ALLOC);
	if (error)
		return error;
	for (i = 0; i < HOST_MOCK_DMA_SLOTS; i++)
		if (!mock->dma[i].address)
			break;
	if (i == HOST_MOCK_DMA_SLOTS)
		return -ENOSPC;
	if (mock->next_device_address > UINT64_MAX - (alignment - 1))
		return -EOVERFLOW;
	address = (mock->next_device_address + alignment - 1) &
		~((uint64_t)alignment - 1);
	if (size > UINT64_MAX - address)
		return -EOVERFLOW;
	memory = aligned_alloc(alignment, size);
	if (!memory)
		return -ENOMEM;
	memset(memory, 0, size);
	mock->dma[i] = (struct host_dma){memory, size, address};
	mock->next_device_address = address + size;
	mock->live_allocations++;
	*result = mock->dma[i];
	return 0;
}

int host_mock_free(struct host_mock *mock, const struct host_dma *allocation)
{
	size_t i;

	if (!allocation || !allocation->address)
		return -EINVAL;
	for (i = 0; i < HOST_MOCK_DMA_SLOTS; i++) {
		struct host_dma *slot = &mock->dma[i];

		if (slot->address != allocation->address ||
		    slot->size != allocation->size ||
		    slot->device_address != allocation->device_address)
			continue;
		free(slot->address);
		memset(slot, 0, sizeof(*slot));
		mock->live_allocations--;
		return 0;
	}
	return -ENOENT;
}

static int valid_access(size_t offset, size_t width)
{
	return (width == 4 || width == 8) && offset % width == 0 &&
		offset <= HOST_MOCK_BAR_SIZE - width;
}

int host_mock_read(struct host_mock *mock, size_t offset, size_t width,
		   uint64_t *value)
{
	uint64_t data = 0;
	size_t i;
	int error;

	if (!value || !valid_access(offset, width))
		return -EINVAL;
	error = host_fault_hit(mock, HOST_FAULT_MMIO_READ);
	if (error)
		return error;
	for (i = 0; i < width; i++)
		data |= (uint64_t)mock->bar[offset + i] << (i * 8);
	*value = data;
	return 0;
}

int host_mock_write(struct host_mock *mock, size_t offset, size_t width,
		    uint64_t value)
{
	size_t i;
	int error;

	if (!valid_access(offset, width) || (width == 4 && value > UINT32_MAX))
		return -EINVAL;
	error = host_fault_hit(mock, HOST_FAULT_MMIO_WRITE);
	if (error)
		return error;
	for (i = 0; i < width; i++)
		mock->bar[offset + i] = (unsigned char)(value >> (i * 8));
	return 0;
}

int host_mock_advance(struct host_mock *mock, uint64_t elapsed_us)
{
	if (elapsed_us > UINT64_MAX - mock->now_us)
		return -EOVERFLOW;
	mock->now_us += elapsed_us;
	return 0;
}
