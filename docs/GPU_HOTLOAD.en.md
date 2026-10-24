# GPU hotload integration and corrections

[简体中文](GPU_HOTLOAD.md)

2026-10-24: NLH2 core-policy acceptance does not establish GPU driver replacement.
The controller's current capture reports `gpu.status service=unavailable mode=0`
and NLH `provider=0`. This boot has no active GPU instance or GPU hardware bridge.
The source changes below are not in that image and have no new device acceptance.

## Power blocker

The previous mode-3 capture is
`out/ios16/gui-touch-keys-gpu-device-40002797063-followup1/07-gpu.status.log`.
The 200 MHz HUB request reports `phase=7 code=6 op=4 resource=3`, native status
`0x8000000000000013`, no valid observed frequency, and `confirmed=0x1002`.
Before `SetClockFreqHz`, the SET_RATE callback in `GpuPower.c` reads the previous
configuration. The matching binary's error path points to `GetClockFreqHz`
returning `EFI_NOT_STARTED` for an empty previous plan, before the actual setter.
This inferred call boundary still needs a new terminal-backend trace. The pinned upstream and matching ClockDxe
support 200 MHz; this error does not establish an unsupported target frequency.

The shared backend must distinguish an uninitialized previous plan from a real
set-rate failure, while retaining exact setter-result and post-read checks.
`GGPH_PREPARED` and the power lease must remain strict. ClockDxe depends on its
frequency-plan, parent-clock and voltage-corner bookkeeping; its UEFI protocol
pointer is not callable from XNU runtime. An owned terminal resource backend
needs device validation before subsequent UEFI integration.

## Corrections within the GPU module

- The GMU service passes the entire GGFWPK1 container to the digest-checking
  loader, rather than a raw GMU member. The AHB fence value now matches the
  vendor expression, `0x80c808a0`.
- GMU load/start share the stage machine's device-write grant, prerequisite and
  quarantine checks. USB cannot bypass the disabled preparation policy; refusal
  occurs before state-machine or hardware changes. The current register subset
  is not a complete HWSCHED startup backend.
- The SMMU journal preserves the native library's one-way readback rules only
  for this bank's 32-bit registers: TCR2 TBI may read 1 after writing 0, and
  SCTLR ASIDPNE may read 0 after writing 1. Other bits remain exact, and the
  library still validates the complete context image.
- MEM_ALLOC replies preserve the logical request size. Legacy requests also
  receive the full 11-dword reply, with align=0. Large allocations are cleared
  and published across independent 4 MiB CPU mappings, without assuming that
  adjacent GPU virtual addresses imply contiguous CPU mappings.
- The allocator pump requires an entered, authorized native HFI client; manual
  terminal control retains queue ownership otherwise. It validates packet and
  ring boundaries, preserves ACK/error packets for their explicit consumer, and
  latches a refusal. Reply publication is followed by the real barrier and
  HOST2GMU doorbell. Notification failure quarantines without replay. `replied`
  means published and notified, not firmware consumption or acknowledgement.
- Retained share, GMU, SMMU, fence or HFI state yields an unconfirmed stage exit.
  The framework quarantines and stops reverse release instead of interpreting
  retention-until-reboot logging as a successful STOP.

Sources remain OnePlus graphics/device-tree commit
`453d8875f815bf9dfad9035982861226a4823a6c` and SCM kernel commit
`d763ed47deec55451366c1187f813f2f309d88a7`. The target sequence is
`adreno_gen7_hwsched.c::gen7_hwsched_gmu_first_boot`; the legacy path has different
PDC/RSC timing. See the [secure startup audit](GPU_SECURE_START_AUDIT.md).

## Replacement boundary and remaining work

The first GH ABI 1 call path is
`hfiPumpOnce → ggpp_pump → gga_handle → hfiPoolDecode → giulia_gpu_hotload_decide → giulia_hotload_select`.
The extern-C, noinline production entry directly calls the bridge, and the HFI
boundary directly calls that entry. Link evidence does not depend on function
pointer devirtualization. The portable callback helper shares the same request
preparation and response-commit implementation.
The source is `ios16/gpu/hotload/giulia_gpu_proto_module.c`, named
`giulia-gpu-proto`, service kind, interface `0x47480001`, state ABI 1. Thirteen
u64 request words carry a schema, actual packet length and eleven copied u32
words. One u64 response encodes the error, effective permissions, required
mapping domains, reserved-resource flag and rounded size. The module uses an
invocation ticket; the resident adapter checks the complete upstream envelope,
then decodes the response into an internal plan. Ordinary SYSRAM reserve size,
effective permissions, GPU/GMU mapping branches and backing checks consume that
plan. Dedicated resources still use their original wire kind and owner. Replies
retain the wire flags, logical size and alignment. Unavailability, update admission, stale epochs or invalid
responses retain the resident parser. `GIULIA GPU PROTO` in `gpu.status` exposes
calls/selected/fallback/rejected from this HFI allocation boundary. `selected`
only counts validated protocol results, including protocol refusals. Separate
module_allocated/module_reused counters advance after successful allocation or
cache reuse; they do not prove DMA, GMU execution or acceleration. Plans must
remain equivalent to the pinned upstream; hardware parameters are not changed
merely to produce a changed-result counter.

Private module state is 32 bytes; its migration snapshot is 40 bytes. START/STOP
manage private running/admission state, while EXPORT/IMPORT transfer counters.
`GG_PROTO_FAIL_START`, `GG_PROTO_FAIL_IMPORT` and `GG_PROTO_FAIL_STOP` support
unified failure acceptance. This module owns no hardware: its successful STOP
does not stop the GPU. The native HFI client remains incomplete and mode 0 has no
service; real firmware-packet coverage is pending device validation.

The IOService, locks, workers, firmware, DMA backing, SMMU tables and leases,
secure ownership, HFI queues and pending ACKs, IRQ/fault owner, Gunyah epoch,
share state and fence provider must remain resident. NLH_CALL scalars cannot
become kernel pointers. A resident bridge needs loader-verified image identity,
version, generation and lifecycle phase, typed requests and resident opaque
handles. A module name cannot authorize hardware writes.

Native startup still needs the complete ACD/power/clock dependencies, AHB
timeout/timer, PDC/RSC, secure CX, register/version initialization, IRQ/fault
handling, DDR bandwidth, HFI feature/performance/START, controller and fence
backends. Shutdown requires idle/slumber, RSC sleep, GBIF halt, IRQ/worker drain,
final HFI drain, and DMA/fence confirmation before stream/context detach, TLB
synchronization and reclamation. `platformIdle()` requires banks already stopped;
it cannot prove a running master's stop or DMA drain.

The controller owns terminal access and the next unified build. This round ran
no local host test suite, device operation, UEFI integration, commit or push.
Full kernel compilation, artifact checks and new-image device validation remain
pending. GPU submission, production fences, rendering, Metal and whole-driver
replacement have not passed acceptance.
