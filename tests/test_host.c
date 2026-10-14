/* SPDX-License-Identifier: BSD-3-Clause */
#include "host/mock.h"
#include "host/test.h"

#include <errno.h>
#include <string.h>

static int faults(void)
{
	struct host_mock mock;
	size_t i;

	host_mock_init(&mock);
	TEST_CHECK(host_fault_arm(&mock, HOST_FAULT_ALLOC, 2, -ENOMEM) == 0);
	TEST_CHECK(host_fault_hit(&mock, HOST_FAULT_ALLOC) == 0);
	TEST_CHECK(host_fault_hit(&mock, HOST_FAULT_MMIO_READ) == 0);
	TEST_CHECK(host_fault_hit(&mock, HOST_FAULT_ALLOC) == -ENOMEM);
	TEST_CHECK(host_fault_hit(&mock, HOST_FAULT_ALLOC) == 0);
	TEST_CHECK(host_fault_arm(&mock, HOST_FAULT_ALLOC, 0, 0) == 0);
	for (i = 0; i < 3; i++)
		TEST_CHECK(host_fault_hit(&mock, HOST_FAULT_ALLOC) == 0);
	TEST_CHECK(host_fault_arm(&mock, HOST_FAULT_COUNT, 1, -EIO) == -EINVAL);
	TEST_CHECK(host_fault_hit(&mock, (enum host_fault_point)-1) == -EINVAL);
	TEST_CHECK(host_fault_arm(&mock, HOST_FAULT_ALLOC, 1, 0) == -EINVAL);
	TEST_CHECK(host_fault_arm(&mock, HOST_FAULT_ALLOC, 1, 1) == -EINVAL);
	mock.faults[HOST_FAULT_ALLOC].calls = SIZE_MAX;
	TEST_CHECK(host_fault_hit(&mock, HOST_FAULT_ALLOC) == -EOVERFLOW);
	TEST_CHECK(host_mock_cleanup(&mock) == 0);
	return 0;
}

static int allocations(void)
{
	struct host_mock mock;
	struct host_dma slots[HOST_MOCK_DMA_SLOTS];
	struct host_dma result = {0};
	struct host_dma invalid;
	size_t i;

	host_mock_init(&mock);
	TEST_CHECK(host_mock_alloc(&mock, 0, 4096, &result) == -EINVAL);
	TEST_CHECK(host_mock_alloc(&mock, 4096, 3, &result) == -EINVAL);
	TEST_CHECK(host_mock_alloc(&mock, 4095, 4096, &result) == -EINVAL);
	TEST_CHECK(host_mock_alloc(&mock, 4096, 4096, NULL) == -EINVAL);
	TEST_CHECK(host_fault_arm(&mock, HOST_FAULT_ALLOC, 1, -ENOMEM) == 0);
	TEST_CHECK(host_mock_alloc(&mock, 4096, 4096, &result) == -ENOMEM);
	TEST_CHECK(result.address == NULL && mock.live_allocations == 0);
	for (i = 0; i < HOST_MOCK_DMA_SLOTS; i++) {
		size_t j;

		TEST_CHECK(host_mock_alloc(&mock, 4096, 4096, &slots[i]) == 0);
		TEST_CHECK((uintptr_t)slots[i].address % 4096 == 0);
		TEST_CHECK(slots[i].device_address ==
			   UINT64_C(0x100000000) + i * 4096);
		for (j = 0; j < slots[i].size; j++)
			TEST_CHECK(((unsigned char *)slots[i].address)[j] == 0);
	}
	TEST_CHECK(host_mock_alloc(&mock, 4096, 4096, &result) == -ENOSPC);
	invalid = slots[0];
	invalid.device_address++;
	TEST_CHECK(host_mock_free(&mock, &invalid) == -ENOENT);
	TEST_CHECK(mock.live_allocations == HOST_MOCK_DMA_SLOTS);
	TEST_CHECK(host_mock_free(&mock, NULL) == -EINVAL);
	TEST_CHECK(host_mock_free(&mock, &slots[0]) == 0);
	TEST_CHECK(host_mock_free(&mock, &slots[0]) == -ENOENT);
	TEST_CHECK(host_mock_alloc(&mock, 4096, 4096, &result) == 0);
	TEST_CHECK(result.device_address ==
		   UINT64_C(0x100000000) + HOST_MOCK_DMA_SLOTS * 4096);
	TEST_CHECK(host_mock_free(&mock, &result) == 0);
	for (i = 1; i < HOST_MOCK_DMA_SLOTS; i++)
		TEST_CHECK(host_mock_free(&mock, &slots[i]) == 0);
	TEST_CHECK(host_mock_cleanup(&mock) == 0);
	mock.next_device_address = UINT64_MAX;
	TEST_CHECK(host_mock_alloc(&mock, 4096, 4096, &result) == -EOVERFLOW);
	mock.next_device_address = UINT64_MAX - 4095;
	TEST_CHECK(host_mock_alloc(&mock, 4096, 4096, &result) == -EOVERFLOW);
	TEST_CHECK(host_mock_cleanup(&mock) == 0);
	TEST_CHECK(host_mock_alloc(&mock, 4096, 4096, &result) == 0);
	TEST_CHECK(host_mock_cleanup(&mock) == 1);
	TEST_CHECK(mock.live_allocations == 0);
	TEST_CHECK(host_mock_cleanup(&mock) == 0);
	return 0;
}

static int mmio(void)
{
	struct host_mock mock;
	uint64_t value = UINT64_MAX;
	unsigned char saved[HOST_MOCK_BAR_SIZE];

	host_mock_init(&mock);
	TEST_CHECK(host_mock_read(&mock, 0, 8, &value) == 0 && value == 0);
	TEST_CHECK(host_mock_write(&mock, 0, 8, UINT64_C(0x8877665544332211)) == 0);
	TEST_CHECK(mock.bar[0] == 0x11 && mock.bar[7] == 0x88);
	TEST_CHECK(host_mock_read(&mock, 4, 4, &value) == 0);
	TEST_CHECK(value == UINT64_C(0x88776655));
	TEST_CHECK(host_mock_write(&mock, HOST_MOCK_BAR_SIZE - 4, 4, UINT32_MAX) == 0);
	TEST_CHECK(host_mock_read(&mock, HOST_MOCK_BAR_SIZE - 4, 4, &value) == 0);
	TEST_CHECK(value == UINT32_MAX);
	memcpy(saved, mock.bar, sizeof(saved));
	TEST_CHECK(host_mock_write(&mock, 1, 4, 0) == -EINVAL);
	TEST_CHECK(host_mock_write(&mock, 0, 4, UINT64_MAX) == -EINVAL);
	TEST_CHECK(host_mock_write(&mock, SIZE_MAX, 8, 0) == -EINVAL);
	TEST_CHECK(host_mock_write(&mock, HOST_MOCK_BAR_SIZE, 4, 0) == -EINVAL);
	TEST_CHECK(host_mock_read(&mock, 0, SIZE_MAX, &value) == -EINVAL);
	TEST_CHECK(host_mock_read(&mock, 0, 8, NULL) == -EINVAL);
	TEST_CHECK(host_fault_arm(&mock, HOST_FAULT_MMIO_WRITE, 1, -EIO) == 0);
	TEST_CHECK(host_mock_write(&mock, 0, 8, 0) == -EIO);
	TEST_CHECK(memcmp(saved, mock.bar, sizeof(saved)) == 0);
	TEST_CHECK(host_fault_arm(&mock, HOST_FAULT_MMIO_READ, 1, -EIO) == 0);
	value = 123;
	TEST_CHECK(host_mock_read(&mock, 0, 8, &value) == -EIO && value == 123);
	TEST_CHECK(host_mock_write(&mock, 0, 8, 0) == 0);
	TEST_CHECK(host_mock_read(&mock, 0, 8, &value) == 0 && value == 0);
	TEST_CHECK(host_mock_cleanup(&mock) == 0);
	return 0;
}

static int allocation_unwind(void)
{
	size_t fail_at;

	for (fail_at = 1; fail_at <= HOST_MOCK_DMA_SLOTS; fail_at++) {
		struct host_mock mock;
		struct host_dma slots[HOST_MOCK_DMA_SLOTS];
		size_t allocated = 0;
		int error = 0;

		host_mock_init(&mock);
		TEST_CHECK(host_fault_arm(&mock, HOST_FAULT_ALLOC,
					  fail_at, -ENOMEM) == 0);
		while (allocated < HOST_MOCK_DMA_SLOTS) {
			error = host_mock_alloc(&mock, 4096, 4096,
						&slots[allocated]);
			if (error)
				break;
			allocated++;
		}
		TEST_CHECK(error == -ENOMEM);
		TEST_CHECK(allocated == fail_at - 1);
		TEST_CHECK(mock.live_allocations == allocated);
		while (allocated)
			TEST_CHECK(host_mock_free(&mock, &slots[--allocated]) == 0);
		TEST_CHECK(host_mock_cleanup(&mock) == 0);
	}
	return 0;
}

static int clock_test(void)
{
	struct host_mock mock;

	host_mock_init(&mock);
	TEST_CHECK(host_mock_advance(&mock, 0) == 0 && mock.now_us == 0);
	TEST_CHECK(host_mock_advance(&mock, 100) == 0 && mock.now_us == 100);
	TEST_CHECK(host_mock_advance(&mock, UINT64_MAX) == -EOVERFLOW);
	TEST_CHECK(mock.now_us == 100);
	TEST_CHECK(host_mock_advance(&mock, UINT64_MAX - 100) == 0);
	TEST_CHECK(host_mock_advance(&mock, 1) == -EOVERFLOW);
	TEST_CHECK(mock.now_us == UINT64_MAX);
	TEST_CHECK(host_mock_cleanup(&mock) == 0);
	return 0;
}

static int expected_failure(void)
{
	TEST_CHECK(0);
	return 0;
}

static int after_failure(void)
{
	puts("runner continued after failure");
	return 0;
}

int main(int argc, char **argv)
{
	const struct host_test tests[] = {
		{"fault points", faults},
		{"tracked allocations", allocations},
		{"allocation failure unwind", allocation_unwind},
		{"bounded MMIO", mmio},
		{"virtual clock", clock_test}
	};

	if (argc == 2 && strcmp(argv[1], "--runner-failure") == 0) {
		const struct host_test failure_tests[] = {
			{"expected failure", expected_failure},
			{"after failure", after_failure}
		};

		return host_test_run(failure_tests, 2);
	}
	if (argc != 1) {
		fprintf(stderr, "usage: %s [--runner-failure]\n", argv[0]);
		return 2;
	}
	return host_test_run(tests, sizeof(tests) / sizeof(tests[0]));
}
