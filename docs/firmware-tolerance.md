# Firmware tolerance inventory

This document lists every place where this driver rejects firmware behavior
that the pinned Linux reference tolerates. Each case records the driver rule,
the pin rule with file and line evidence, the decision, and the risk.

The pin is Linux commit `0fcb05b72344f40806cbe810de8872f1698bf053`. Line
references are from that pin.

Decision policy:

- TOLERATE: the pin demonstrably accepts the variant and tolerance does not
  weaken memory safety, buffer ownership, or DMA lifetime rules.
- KEEP-AND-DOCUMENT: the strict rule protects a contract this library
  depends on, or tolerance requires behavior the pin does not show. Each
  kept rule is an Azure validation item on ticket `50b980feba`.

The driver never tolerates a variant the pin also rejects.

## 1. HWC response acceptance (control.c command, gdma.c complete_rx)

| Item | Driver rule now | Pin rule | Decision |
|---|---|---|---|
| Response length | Accept length from 32 to the caller buffer. Fail a command whose length is below the expected struct. | `mana_hwc_verify_resp_msg` accepts any length in 32..buffer (`hw_channel.c:43-54`). `mana_verify_resp_hdr` fails `msg_size` below the expected struct as a command error (`mana_en.c:1136-1138`). | TOLERATE |
| Response length above the expected struct | Reject. | Reject. The pin copies at most the caller buffer and the MANA callers pass a struct-sized buffer (`mana_gd_send_request` through `hw_channel.c:861-888`). | KEEP (pin-equivalent) |
| Short response action | Fail the command with `-EPROTO`. Control is not quarantined. | The MANA layer returns the error and keeps the channel (`mana_en.c:1575-1585` and the caller paths). | TOLERATE |
| Response version ceiling | Accept any version of at least 1. The old code rejected a version above the requested version. | `mana_verify_resp_hdr` checks only `msg_version >= GDMA_MESSAGE_V1` (`mana_en.c:1133-1135`). There is no ceiling. | TOLERATE |
| `msg_size` echo equality | Removed. The MANA layer now requires `msg_size >= expected` only, like the pin. | `mana_verify_resp_hdr` requires `msg_size >= min_size` (`mana_en.c:1136-1138`). | TOLERATE |
| Response `hdr_type` word | Unchecked. The old code required 0. | Never read. `mana_verify_resp_hdr` starts at `msg_type` (`mana_en.c:1126-1140`). No response reader touches the field. | TOLERATE |
| Response `reserved` word | Unchecked. The old code required 0. | Never read (`gdma.h:169-174` defines it; no consumer). | TOLERATE |
| Status handling | `0xffffffff` maps to `-EOPNOTSUPP`, other nonzero to `-EPROTO`. | Same mapping (`hw_channel.c:923-933`, `mana_en.c:1100-1107`). | KEEP (pin-equivalent) |
| `GDMA_STATUS_MORE_ENTRIES` (0x105) | Rejected for every command. | The HWC layer treats it as success (`hw_channel.c:923`). The MANA layer still rejects any nonzero status (`mana_en.c:1100`). Only GDMA-level commands tolerate it. | KEEP-AND-DOCUMENT |
| Malformed HWC completion (length below 32, bad message id, bad queue) | Latch the channel error and poison control. | The pin fails one request or drops the message and keeps the channel (`hw_channel.c:79-110`). | KEEP-AND-DOCUMENT |

More entries: if Azure firmware ever answers a GDMA-level query with status
0x105, this driver fails while Linux succeeds. This is an Azure validation
item on `50b980feba`.

Malformed completion: this library treats a transport-level parse failure as
an uncertain channel state. The latch keeps a late or corrupt completion
from reaching a caller buffer. Changing the latch split is outside this
ticket. Azure gate: `50b980feba`.

## 2. CONFIG_TX short form byte (control.c start)

Byte 34 of the CONFIGURE_VPORT_TX response above 1 no longer poisons control.
The driver stores 1 for any nonzero byte, like the pin bool field.

- Pin: `apc->tx_shortform_allowed = resp.short_form_allowed;` stores the raw
  byte into a bool without sanitizing (`mana_en.c:1450`, `mana.h:579`,
  `mana.h:842`). The TX submit path never reads the stored value
  (`mana_en.c:395-404` switches on the offset alone).
- Old driver rule: a byte above 1 poisoned control forever. That blocked
  stop and pinned registration, PD, and the IRQ. It contradicted the contract
  that a failed start still owns registration and stop still runs.
- Decision: TOLERATE. The byte only selects short or long OOB. Long OOB
  remains legal for every packet, so no safety rule weakens.
- Test: "TX short-form byte above one is stored as one" in
  `tests/test_control.c`.

## 3. QUERY_DEVICE bm_hostmode (control.c start)

`bm_hostmode` no longer rejects start. The driver records the byte in
`mana_control.bm_hostmode` and continues.

- Pin: the value is stored without validation (`mana_en.c:1323`,
  `mana_en.c:4140`). It only gates PF-only vPort commands on a PF
  (`mana_en.c:3283`, `mana_en.c:3295`). A VF probe succeeds with either
  value. This library drives the VF (`include/uk/mana.h`, 1414:00ba).
- Decision: TOLERATE. This library is VF-only, so the value cannot change
  any command this driver sends.
- Test: "Pinned tolerant fields reach ready with values recorded" in
  `tests/test_control.c`.

## 4. QUERY_PORT MAC (control.c start)

A zero or multicast MAC no longer rejects start. The driver keeps the MAC as
reported and reports the anomaly once with `-EOPNOTSUPP`.

- Pin: `ether_addr_copy(apc->mac_addr, resp.mac_addr)` copies the MAC without
  any validation (`mana_en.c:1375`). Linux binds and probes with any value.
- Decision: TOLERATE. The MAC is a filtering attribute, not a memory or DMA
  resource. The caller still sees the value through the control context and
  the netdev MAC.
- The zero `max_sq`, zero `max_rq`, and invalid vport handle rejections
  remain. The library gates queue creation on `max_sq`/`max_rq`
  (`src/data_queue.c`) and treats `UINT64_MAX` as the no-object sentinel in
  every handle. The pin defers these to firmware rejection, so a real device
  that works on Linux does not hit this rule with sane values. If Azure
  firmware ever reports them, it is an Azure gate item on `50b980feba`.
- Test: "Pinned tolerant fields reach ready with values recorded".

## 5. QUERY_LINK firmware status (control.c query_link and start)

Every non-transport link query failure is now tolerated by start. The error
stays recorded in `mana_control.link_query_error` and the speeds stay zero.

- Pin: `mana_query_link_cfg` maps any firmware status to `-EOPNOTSUPP` and
  stores the error (`mana_en.c:1586-1593`). Its caller treats any failure as
  an unknown speed and continues (`mana_en.c:994-995`). The query is a run
  time query only. It is not part of Linux probe.
- Old driver rule: only `-EOPNOTSUPP` (status `0xffffffff`) was tolerated at
  start. Any other status, including a transient status, failed start with
  `-EPROTO`.
- Decision: TOLERATE. Speed and QoS are informational. A transport failure
  still poisons control and fails start, so ownership rules do not change.
- Tests: "Source-defined old firmware MTU and tolerated link status" and
  "Link re-query recovers and pre-configuration sends nothing".

## 6. Bootstrap HWC EQ events (gdma.c mana_bootstrap_poll)

| EQE type | Old rule | Pin rule | Decision |
|---|---|---|---|
| 132 FPGA reconfig | Sticky `-ENOTSUP`. | Device service work: log, then schedule the service procedure (`gdma_main.c:956-971`). | KEEP-AND-DOCUMENT |
| 133 unknown reconfig subfield | Sticky `-ENOTSUP`. | Log a warning and ignore (`hw_channel.c:209-211`). | TOLERATE |
| 134 SOC service | Sticky `-ENOTSUP`. | RDMA suspend/resume return success when no RDMA client is attached (`mana_en.c:4053-4061`). Unknown service types log and ignore (`hw_channel.c:227-230`). | TOLERATE |
| 135 reset request | Sticky `-ENOTSUP`. | Device service work, same as 132 (`gdma_main.c:956-971`). | KEEP-AND-DOCUMENT |
| 176 RNIC QP fatal | Sticky `-EPROTO` (default case). | The HWC handler warns and ignores (`hw_channel.c:233-236`). The adapter EQ has no callback and drops it (`mana_en.c:1812`, `gdma_main.c:942-949`). | TOLERATE |
| Undefined types | Sticky `-EPROTO` (default case). | The EQ dispatcher default case ignores silently (`gdma_main.c:973-974`). | TOLERATE |
| 129/130/131 after INIT_DONE | Sticky `-EPROTO`. | Re-applied silently (id overwrite, `complete()` on a done completion). | KEEP-AND-DOCUMENT |
| 3 completion with wrong CQ id | Sticky `-EPROTO`. | `WARN_ON_ONCE` then skip the event (`gdma_main.c:925-930`). | KEEP-AND-DOCUMENT |

Tolerated EQEs increment `mana_bootstrap.ignored_events`. The driver still
advances the EQ head and rings the doorbell for them.

Keep rationale for 132 and 135: the pin does not ignore these events. It
runs a device service and reset procedure that this library does not have.
Silently continuing across a device reset would put DMA lifetimes in an
undefined state. Azure gate: `50b980feba` must prove that these events do
not occur during normal operation, or a future service/recovery feature must
land.

Keep rationale for 129/130/131 and 3 with wrong ids: these break the queue
identity contract that every later command depends on. The pin only warns
about them. A device that emits them is not in a state this driver can
trust. Azure gate: `50b980feba`.

## 7. Data EQ events (data_queue.c mana_data_poll)

| EQE type | Old rule | Pin rule | Decision |
|---|---|---|---|
| 129, 130, 131, 133, 134, 176 | Poison the shared context (`-ENOTSUP`). | The adapter EQ sets no service callback (`mana_en.c:1812`), and the dispatcher drops service types without a callback (`gdma_main.c:942-949`). | TOLERATE |
| 132, 135 | Poison the shared context (`-ENOTSUP`). | Device service work (`gdma_main.c:956-971`). | KEEP-AND-DOCUMENT |
| Undefined types | Already ignored and counted in `unknown_events`. | Ignored (`gdma_main.c:973-974`). | KEEP (pin-equivalent) |
| EQE info reserved bits | Poison. | Never checked. | KEEP-AND-DOCUMENT |

Tolerated service EQEs increment `mana_data.service_events`.

The 132/135 rationale matches case 6. The pin reacts with device recovery,
not silent continuation.

Keep rationale for the EQE info reserved bits: the shared EQ routes
completions by these words. The pin ignores the bits, so a future revision
may relax this check. This ticket keeps the reviewed data-queue contract and
records it as an Azure validation item on `50b980feba`.

