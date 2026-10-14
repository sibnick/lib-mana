# Host test foundation

This slice supports ticket `4c2b1925b2`.
It supplies test-only primitives and a reusable PCI, SMC, and HWC bootstrap model.
It does not supply complete MANA control, TX, or RX responses.
It does not change the production platform API.

## Run the tests

```sh
make test
make test-sanitize
```

Both targets run the scaffold checks, foundation self-tests, and bootstrap regression cases.
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
The primitives do not encode BAR registers, doorbells, GDMA requests, or HWC events.
It does not emulate DMA coherency, interrupt delivery, or hardware memory ordering.
It supplies no guessed command formats.

## Reusable bootstrap model

`tests/host/bootstrap.h` exposes caller-owned `struct host_bootstrap`.
`tests/host/bootstrap.c` supplies the production `mana_platform` adapter and synchronous device transport.
The root Makefile links both host support files into each host suite.
Initialize fresh storage with `host_bootstrap_init(model, HOST_BOOTSTRAP_NORMAL)`.
Use `model.b` and `model.platform` with the real bootstrap and HWC functions.
Keep the model at the same address while its platform callbacks remain active.
Do not initialize a model with live resources.
Stop the bootstrap before calling `host_mock_cleanup(&model.host)`.
Require zero from cleanup after successful driver teardown.

The model preserves synthetic DMA addresses and checks SMC address packing, WQEs, and SQ doorbells.
It supplies the four source-grounded bootstrap GDMA responses.
It records resource counts, protocol mismatches, and platform error reports.
The public fault controls match the bootstrap regression cases.
`host_bootstrap_config` reads the byte-backed PCI configuration without consuming adapter fault counters.
Use byte offsets and widths of 1, 2, or 4 within its 256-byte region.
MMIO and DMA-free errors abort with no success fallback.
Recoverable platform callbacks return negative errno values.

Set `model.respond` and `model.respond_ctx` to extend request-response behavior.
The hook runs after transport checks and before response completions and interrupts.
It receives the request bytes and their header length.
The response buffer has 4096 bytes and a zeroed body.
The model prefills the requested response header, device ID, and activity ID.
It preserves the requested response version, not the request version.
The hook can change header fields to test malformed responses.
The initial response length equals the requested response size.
Return zero after writing a response body, status, and actual response length.
The model writes that length into the header and RX completion.
The hook must keep the length between 32 and 4096 bytes.
Transport fault injection still applies after the hook.
The hook must not retain either buffer or issue a nested HWC request.

Return `-ENOTSUP` without changing the buffer or length to select a built-in response.
Built-in responses require their original command and device IDs.
An unsupported request without a handler aborts with a diagnostic.
Any other hook error or invalid response length also aborts with a diagnostic.
These failures cannot return through the platform's void doorbell callback.
They must not create a successful firmware response.
Tests can write a source-defined firmware status to test driver error handling.
The hook does not supply MANA command definitions or asynchronous event scheduling.
Add only responses that follow the pinned protocol source.
Control, TX, and RX coverage belongs to later slices.
The full ticket remains open.

Use the authoritative Linux definitions for each added protocol behavior.
`docs/bootstrap.md` records the source pin.

- [GDMA definitions](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/include/net/mana/gdma.h)
- [GDMA implementation](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/net/ethernet/microsoft/mana/gdma_main.c)
- [HWC implementation](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/net/ethernet/microsoft/mana/hw_channel.c)

The Fossil wiki page **MANA and GDMA Protocol Research** records the checked protocol facts.
Do not treat foundation self-tests as proof of HWC startup or Azure network operation.
These original test sources use the project's BSD-3-Clause license.
They contain no copied Linux source.
