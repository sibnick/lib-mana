# Host test foundation

This slice supports ticket `4c2b1925b2`.
It supplies test-only primitives, not a complete MANA device model.
It does not change the production platform API.

## Run the tests

```sh
make test
make test-sanitize
```

Both targets run the scaffold checks and foundation self-tests.
The sanitizer target uses AddressSanitizer and UndefinedBehaviorSanitizer.
Add host suites to `HOST_TESTS` in the root Makefile.
Name each suite `tests/test_<name>.c`.
Both targets then build and run that suite.
The runner stops the make target when a suite returns a nonzero status.

Use `TEST_CHECK` from `tests/host/test.h`.
It prints the failed expression, source file, and line.
It returns failure even when the compiler defines `NDEBUG`.
Use `host_test_run` to run named cases and report the failure count.
The runner runs remaining cases after a case fails.
Each case must return zero for success.

Check the runner's failure path with:

```sh
./build/test_host --runner-failure
```

This command must return status 1.
It must report one failed case and run the next case.

## Mock primitives

`tests/host/mock.h` exposes a caller-owned `host_mock`.
Initialize each fresh mock with `host_mock_init`.
Do not initialize a mock with live allocations.
Use `host_mock_cleanup` to free all allocations and reset its state.
It returns the allocation count before cleanup.
Require zero at successful teardown to detect missing driver cleanup.

| Primitive | Contract |
|---|---|
| Fault points | Separate allocation, MMIO-read, and MMIO-write counters |
| Fault schedule | Fail only the selected one-based call with a negative error |
| Fault reset | Arming a point resets its counter. Call zero disables failure. |
| Allocations | Zeroed C11 aligned memory, with 16 tracked live slots |
| Device addresses | Synthetic addresses start at `0x100000000` and never use CPU pointer values |
| Free | Require the recorded CPU address, size, and device address |
| MMIO | Byte-backed 8 KiB test region with aligned 32-bit and 64-bit little-endian access |
| Virtual time | Microsecond counter advances only on explicit calls |

Allocation sizes must be nonzero multiples of a power-of-two alignment.
Alignment must be at least `sizeof(void *)`.
Synthetic address allocation checks alignment and address overflow.
Invalid calls and injected failures leave output values and resources unchanged.
Invalid calls do not consume fault counters.
Valid resource requests consume counters even if capacity prevents success.
MMIO failures leave the backing region unchanged.
The virtual clock rejects overflow and leaves its value unchanged.
All mock errors use negative errno values.
Callers must check each result.
The mock has no threads, wall-clock waits, or random input.

## Protocol boundary and limits

The mock region size and allocation capacity are test limits, not MANA device limits.
The mock does not encode BAR registers, doorbells, GDMA requests, or HWC events.
It does not emulate DMA coherency, interrupt delivery, or hardware memory ordering.
It supplies no guessed command formats.

The bootstrap worker owns `include/uk/mana/platform.h`.
An adapter must connect these primitives to that checked production interface.
PCI configuration, MSI-X, barriers, and scheduled device events still need that adapter.
Bootstrap tests must then check valid startup, malformed resources, timeouts, and partial cleanup.
Control, TX, and RX device responses belong to later slices.
The full ticket remains open.

Use the authoritative Linux definitions for each added protocol behavior:

- [GDMA definitions](https://github.com/torvalds/linux/blob/master/include/net/mana/gdma.h)
- [GDMA implementation](https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/microsoft/mana/gdma_main.c)
- [HWC implementation](https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/microsoft/mana/hw_channel.c)

The Fossil wiki page **MANA and GDMA Protocol Research** records the checked protocol facts.
Do not treat foundation self-tests as proof of HWC startup or Azure network operation.
These original test sources use the project's BSD-3-Clause license.
They contain no copied Linux source.
