# Ace 5 GMU clock and SMMU terminal validation

Tested 2026-10-23 with the GUI/touch/physical-key/GPU kernel from build
39932744814 and a temporary boot image with `giulia_gpu=3 giulia_mmio=1`.
No partitions were flashed.

## Automatic integration and subsequent CP boundary

The later r4 image uses kernel build
[39942035365](https://github.com/tycykp/UniXNU-OnePlus-Ace5/actions/runs/39942035365)
at `d0c6e92`. `GpuColdClocks.h` now runs in UEFI after native CX/AOP
preparation. The matching kernel recognizes `GPP_DIRECT_CLOCKS` and checks
the 260/200 MHz configuration and the observed always-on branch semantics.
The running kernel reported `firmware_power_prepared=1 code=0`; a separate
read-only terminal check verified the PLL/RCG/gate registers without writes.

The secure-pool failure was an unused CB3 SCTLR retaining values such as
`0x35fd6114` with M=0. The kernel now proves idle contexts, fault-routed
streams and an idle TLB, initializes SCTLR to zero and checks its readback
before publishing secure tables. The r4 test passed this without terminal
normalization, including all SCM grants for the 32 MiB pool.

GX power/OOB, HWSCHED START, SQE/AQE full readback, mapping audits and OP14
pre-dispatch snapshot passed in `20261023-190250-397191`; cleanup completed.
On a fresh boot, the CP-init test `20261023-190601-004014` lost USB during
OP13 and the device enumerated as Qualcomm 900E on COM15. No OP13 completion,
queue publication, doorbell confirmation, CP-init ACK or GPU memory-write
result was observed. This does not establish whether the failure precedes
actual hardware submission. No recovery or replay was attempted. `cp-mapped`
is now also blocked before any device access; `--prepare-cp-only` remains a
separate no-RAW diagnostic mode.

Full GPU rendering and Metal remain unverified. The next hardware run needs
a physical reboot to Fastboot and isolation of OP13 dispatch/publication;
passing OP14 alone is insufficient. The machine-readable evidence and image
digest are in [gpu-uefi-auto-20261023.json](../hardware/gpu-uefi-auto-20261023.json).

The remaining sections describe the earlier terminal bring-up, before this
automatic integration.

## Upstream comparison

[UniXNU-sheng at 251b291](https://github.com/UniXNU/UniXNU-sheng/tree/251b291adf941b9953fe3ae66a808b9b626e164d)
reports working native Metal on SM8550/Adreno 740. Its
`package/GpuPowerBoard.h` implements the missing cold PLL/RCG sequence;
`package/ShengGpuSmmu.c` and `package/ShengGmu.c` separate GMU translation
and firmware startup from later GPU rendering work.

Ace 5 uses SM8650/Adreno 750. This port uses the pinned SM8650
`gpucc-pineapple.c` PLL1 default L=0x1b, alpha=0x1555 (520 MHz), divided
by two for GMU (260 MHz). HUB selects GPLL0 /3 (200 MHz). These are
configured rates, not physical frequency measurements. No voltage changes
or sheng PLL frequency constants are used.

## Findings

* Inherited GMU CBCR is 0x2001, but its RCG CFG is zero: the branch is on XO,
  not on the expected PLL source. Rejecting all inherited enabled branches
  stopped preparation before reaching SMMU clocks.
* Allowing that state exposed ClockDxe `EFI_NOT_STARTED` while querying the
  old HUB frequency. The UEFI patch is only a diagnostic improvement; it
  does **not** complete automatic clock initialization.
* Terminal preparation enables the SMMU vote (0x3d97000: 0x80000000 -> 1),
  reads ID0=0x4c017e18, ID1=0x40000016, ID2=0x5511, global fault=0, and
  verifies GMU reset is held before programming the cold PLL.
* PLL1 locks; its MODE status changes asynchronously to 0xe0000005.
  Readback checks must mask status bits rather than require whole-word equality.
* GMU/HUB RCG configuration reads back 0x303/0x505. The always-on/HALT_DELAY
  branches do not all retain an enable-bit write; these are not counted as votes.

## Reproduction

After a fresh temporary boot of the MMIO-enabled image, run:

```powershell
python scripts/gpu-gmu-smoke.py --execute
```

Required local inputs (not committed): `out/ios16/gpu-stock-firmware/gmu_gen70900.bin`,
`.cache/stock-boot-16.0.3.500/aop.mbn`, and `../ace5-analysis/running-device.dts`.
Firmware and AOP hashes are checked by the existing transport helpers.
The script refuses already-configured clock state; reboot before another run.

The smoke test pins kernel-owned RAM, enables only GMU bank 2/SID5, verifies
TCM writes, releases reset, and checks stock firmware/HFI responses. It does
not initialize GPU SID0, secure pools, GX, CP, or submit graphics commands.
On confirmed completion it holds reset, faults the stream, disables the
context, synchronizes TLBs, restores saved configuration and closes the test
bus gates. PLL/RCG configuration and pinned memory remain until reboot.
Unconfirmed completion stops further device writes.

## Observed result and limits

Capture: `out/ios16/gpu-gmu-clock-smmu-smoke/20261023-183105-112593/`.
Stock firmware-ready, HFI-ready, query ACK, performance/bandwidth table ACKs
and cleanup all passed. Global fault was zero. Context FSR was 0x400 with
no fault-valid bits under mask 0xc00001fe; the recorded FAR in that first run
is stale and must not be interpreted as a fault address. The diagnostic now
reads FAR only when a fault-valid bit is present. That first result also
retained the old harness's `clock_rates_changed=false` field; the clock
preparation subreport and raw register log establish that clocks were changed,
and the harness now sets that field correctly.

The combined native-GPU experiment stopped earlier at platform action 4
(secure pool preparation), code 8, after secure-CX initialization succeeded.
It did not release GMU reset. No speculative retry followed that refusal.
The isolated GMU test used a fresh boot. This result establishes GMU execution
and its SMMU-backed HFI DMA path, **not** full GPU rendering or Metal support.

Host checks: 108 power-policy cases, handoff-consumer checks, and four terminal
clock tests covering cold-state refusal, asynchronous status, stuck SMMU
clock and released-reset refusal.
