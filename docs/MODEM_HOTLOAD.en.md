# Modem resident control and hotload boundaries

[简体中文](MODEM_HOTLOAD.md)

2026-10-24: this page describes new source interfaces and validation still to be performed together.
No native MPSS provider is registered, and the built-in modem runtime cannot be replaced as a whole.
The pending/event/storage payloads in `gmr_runtime` already exceed NLH's 64 KiB state limit;
the runtime also contains callbacks and outstanding identities. Keep it, the worker, locks, IRQs,
DMA, SMMU, firmware and resource ledger resident.

See the [implementation map](MODEM_BRINGUP.md), [boot map](MODEM_BOOT_MAP.md) and
[hardware map](MODEM_HARDWARE_MAP.md) for pinned upstream sources, firmware, missing configuration
evidence and the complete execution chain. This change does not modify UEFI.

## Implemented resident entrypoints

`ios16/modem/giulia_modem_platform.h` declares these kernel interfaces to the same GiuliaModem service:

| Entrypoint | Behavior |
| --- | --- |
| `giulia_modem_get_snapshot(out)` | Copy one complete software snapshot under the service lock; no hardware access |
| `giulia_modem_submit(command)` | Typed kernel request queue; copy the command and capture epoch/lifecycle/slot/bearer generations |
| `giulia_modem_control(operation, expected_epoch)` | New scalar diagnostic control for `GMP_CONTROL_START/STOP/SUSPEND/RESUME`, sharing checks and queues with the terminal |
| `giulia_modem_terminal(command)` | `modem.status/start/stop/suspend/resume` and the pure-parser fixture command `modem.proto` |

`control` requires an online service, an available worker, `giulia_debug=1`, a nonzero modem mode
and the exact current epoch. START additionally requires a native provider, OFF state, no outstanding
resources or actions, and no earlier queued commands. A missing provider returns
`GMR_UNSUPPORTED=9` without advancing the epoch. `GMR_OK=0` means queue acceptance only;
completion still comes from the same worker and actual provider events. Results use `gmr_result`,
which is not the system errno namespace.

The worker checks the captured request identity before execution. RF_MODE captures the actual
selected slot, and START captures the epoch. STOP targets the current owner and cancels an unexecuted
START and mode 2 automatic startup. During SSR drain, STOP also cancels automatic restart.
STOP uses an independent barrier even when the ordinary queue is full. It cancels unexecuted
business/SSR commands and logs their count. The worker applies it before timers, poll completions
and RX delivery, preventing an SSR's final completion from restarting first. The public business
queue also rejects SSR without a provider at acceptance and execution. These entrypoints do not
create a second state machine or reinitialize gmr. An already issued startup with unresolved effects
still isolates and retains its resources.

A valid nonzero provider `submit` result means the action was not accepted and caused no hardware
operation. The worker retires its exact pending identity through `gmr_reject` without manufacturing
uncertain effects. Failures after submission arrive through `poll`; invalid provider result values
are treated as uncertain failures. Existing resource ownership remains isolated until resolved.

Both DATA_REMOVE and global DATA_DRAIN require NETWORK/RX/TX/DMA completion before clearing bearers
and advancing teardown. IPC_STOP retains the SMP2P/sysmon/SMEM/stop-ack infrastructure needed by
FIRMWARE_STOP. New RX requires ONLINE, and the worker revalidates identity immediately before delivery.
Slot switching, RF shutdown, SIM changes and SSR preserve outstanding SMS identity without automatic resend.

## Connected QMI parser boundary

The production reply path in `gmr_qmi_complete` calls the resident `qmi_check` adapter, then the
`extern "C"`, `noinline` entrypoint `giulia_modem_hotload_decide`, which selects `giulia-modem-proto`
through `giulia_hotload_select`. Target artifacts must prove actual ARM64 B/BL calls into the
external entrypoint and from it to select; a source definition or fixture call alone is insufficient.
With hotload disabled, the public header returns unavailable through the same path and the caller
uses its native baseline, without a select link dependency. Only complete QMI framing/result validation is
replaceable. QRTR source authentication, pending identity, epoch/generations, detailed UIM/NAS/WDS
parsing and state updates remain resident. Module rejection affects request handling; module success
cannot bypass detailed resident parsing or manufacture SIM/network success.

GH ABI 1 uses domain 4, operation 1, interface 0x47480001, state 1, service kind and no dependencies.
The first four request words carry kind/transaction, length/message, opid and origin; remaining words
contain a complete payload copy in little-endian order. Origin 0 is an actual provider reply and
origin 1 is a fixture. Frames above 224 bytes fall back in full, without truncation. One response word
contains `gmq_error` in bits 0..7 and `remote_error` in bits 8..23; other bits are zero. The module
cannot rewrite the authenticated source or generations.

`giulia_modem_proto_module.c` is built as a separate NLH2 module, never a resident translation unit.
Its private state is 56 bytes and its migration snapshot is 64 bytes, containing only counters and
running state. START/STOP enable and disable its actual private logic. EXPORT/IMPORT transfer no
hardware, pointers or requests. `NLH_FAIL_START/IMPORT/STOP` build lifecycle failure scenarios.
USB CALL arguments 0..5 read calls/native/fixture/rejected/remote/imports without changing them.

`modem.proto` runs 22 independent golden/boundary fixtures through the same adapter without injecting
gmr events. With a valid module, require passed=22, failed=0, module_delta=20 and fallback_delta=2;
both 225-byte inputs must fall back intact. Without a module or after unload, require module_delta=0,
fallback_delta=22 and the same correct verdicts. Both require native_delta=0, state_unchanged=1 and
hardware_event=0. Fixture success does not establish actual MPSS replies.

This module only consumes the public GH contract and does not modify shared NativeHotload,
platform, terminal, assembly or build files. The unified validation owner handles bridge registration,
builds, source binding, terminal capability lists and a temporary boot of the new kernel. Host-script
updates cannot add these entrypoints to the existing image. Target build and device results remain pending.

## Shared bridge and control permissions

The bridge must authorize the actual image/version/phase identity. A slot-name allowlist cannot
authorize hardware writes. Ordinary NLH_CALL integers remain values: never encode a `gmr_runtime*`,
snapshot, command, packet or file-buffer pointer. GH permits bounded scalar mailbox access only
for the current authenticated resident CALL ticket. USB/dependency CALLs, lifecycle callbacks and
expired tickets cannot access it. The resident adapter holds the service lock, verifies the same
epoch on return and validates every response value; a module cannot weaken its native baseline.

GH does not call `giulia_modem_control` in this phase; parser modules have no hardware control rights.
Typed kernel control still checks operation and expected epoch. PROBE/EXPORT must not trigger
hardware; IMPORT only updates private state. The current control ingress reports acceptance only,
without a per-call completion ticket. Concurrent request attribution and payload operations need
resident request/receipt objects first; do not infer completion from `last_opid`.

A replaceable module owns private computation/policy state only. Close admission and drain references
before EXPORT/IMPORT. Resident resources, generations and outstanding operations do not migrate.
Replacing a physical execution layer requires actual remote stop, persistence, network withdrawal,
DMA/IRQ drain and memory reclamation evidence. Unknown ownership remains isolated.

## Coordinator's terminal validation

The fifth hotload validation chat exclusively owns the serial port and unified automation.
This modem chat has not executed these commands.
First query the current image:

```text
features
modem.status
hl.status
```

The recorded modem state is OFF, epoch 0, provider 0 and mode 0. `hl.status provider=0` describes the
NLH platform bridge, while `modem.status provider=0` describes the MPSS executor; they are distinct.
Updating host scripts does not update the kernel.

1. After the complete source change, the unified validation owner runs necessary automation under
   the user's new authorization, module-boundary checks, a full target build and artifact verification.
2. Temporarily boot a diagnostic kernel containing the resident fixes with
   `giulia_debug=1 giulia_modem=1`. Do not flash partitions or erase userdata.
3. Without a provider, issue `modem.status`, `modem.start`, `modem.status`: START must return 9,
   leaving OFF, epoch and ownership unchanged.
4. Run `modem.proto` on the new kernel/modules, distinguishing resident fallback from actual module
   execution using all 22 verdicts and counter deltas. Verify stale epochs/tickets, v1-to-v2 migration,
   START/IMPORT rollback, unload fallback and STOP-failure quarantine. Separately verify resident STOP
   cancellation of unexecuted START/SSR. Attribute hardware control completion using actual events/logs.
5. With the complete native provider, validate cold boot/ready, QRTR, RMTFS, SIM/registration,
   WDS/IPA/GSI/SMMU traffic, SMS reconciliation, suspend/resume, SSR, shutdown and resource release,
   including concurrent scenarios with the other hardware modules.
6. Integrate UEFI only after the complete hardware chain succeeds through the terminal on the device.

No new kernel or device success record containing this change exists yet. Source corrections and
resident entrypoints do not establish working MPSS, iOS cellular service or modem hotload.
