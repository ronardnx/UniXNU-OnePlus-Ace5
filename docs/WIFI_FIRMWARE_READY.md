# WCN7850 firmware readiness evidence

The latest completed test on 2026-10-23 stops at QMP channel open, before BDF/M3 provisioning. It does not reach the older 40-second `FW_INIT_DONE` wait.

The 2026-10-24 [complete upstream mapping](WIFI_UPSTREAM_CHAIN.md) records the
platform-to-firmware call chain, applicable branches, remaining lifecycle gaps,
and new registration/pin-result diagnostics. `wifi.status` now reads a cached
worker snapshot, including the exact QMI boot substep and the final stopped state.
The 2026-10-24 13:06 control capture confirms only the pre-DMA wait
(`waiting=1 active=1 stage=3 error=0 quarantine=0`); it has no post-release
firmware result.

## Captures

- `out/ios16/wifi-qdss-test-1/20261023-033042-875184/device-000.log` stopped at `awaiting wifi.start`. Its command history contains no `wifi.start`; this capture did not test QDSS absence signaling.
- `out/ios16/wifi-network-start-1/device.log` explicitly starts Wi-Fi. PCIe, SMMU, MSI, MHI and QMI discovery succeed. HOST_CAP sends feature `0x8`. Firmware requests DDR type 1, 7,536,640 bytes at IOVA `0x21400000`, and CAL type 4, 8,454,144 bytes at IOVA `0x21b30000`.
- The same capture reports QMP `QPT_BUSY`, phase CLOSED, last register `0x14 = UP`. QMI stops with platform error 2 after about 16 ms, `provisioned=0`, `m3_ms=0`.
- Two read-only snapshots in `out/ios16/wifi-qmp-registers/device.log` and `out/ios16/wifi-qmp-registers-tail/device.log` show AOP channel UP, APSS's ACK of that channel DOWN, APSS channel/ACK both DOWN, and both mailbox lengths zero. No pending message was cancelled.

## Implemented corrections

`qpt_open` now recognizes this exact remote-open request. It first acquires the shared software lease and verifies geometry, live links, a closed APSS channel, both empty mailboxes and stable state twice. It then follows the vendor open order: APSS channel UP, observed AOP channel UP, APSS ACK UP, wait for AOP's APSS-channel ACK. It still refuses an inherited open APSS channel, nonempty mailbox, invalid state, changed geometry or changed ownership. Failed writes retain uncertain ownership.

This sequence follows [OnePlus qcom_aoss.c, pinned d763ed47](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/soc/qcom/qcom_aoss.c). Wi-Fi and the UEFI GPU transport share the same implementation; the GPU's native-provider and uncertain-handoff gates remain in force.

QMI memory provisioning now allocates all defined, nonsecure vendor memory segment types. It rejects malformed/unknown types and allocation failure before sending `RESPOND_MEM`; it does not advertise an unallocated segment at IOVA zero. This follows `cnss_pci_alloc_fw_mem` and `cnss_wlfw_respond_mem_send_sync` in [OnePlus CNSS PCI](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/wlan/platform/cnss2/pci.c) and [CNSS QMI](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/wlan/platform/cnss2/qmi.c). The latest firmware requests only types 1 and 4, so missing optional segment allocation is not the cause of that capture's failure.

CAP TLV `0x1c` is decoded as four static `{u64 start, u64 size}` entries, with exact length and overflow checks. The data path receives actual firmware CMEM metadata through `gwq_state.dev_mem`; these addresses are never treated as host physical RAM. Layout comes from the pinned [WLANFW service schema](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/wlan/platform/cnss_utils/wlan_firmware_service_v01.c).

## Verification boundary

### 2026-10-24: missing platform initialization before firmware readiness

The earlier `wifi-qmp-test-1` capture consumed all seven CAP PMU commands and
completed M3 provisioning, yet still timed out at 40 seconds. QMP success alone
does not establish that all board initialization has run. Comparing the current
driver with pinned CNSS and the captured Kiwi device-tree node exposed three
omissions, now corrected in source:

1. **Board PDC defaults.** `cnss_aop_interface_init` calls
   `cnss_aop_pdc_reconfig`, which sends `qcom,pdc_init_table`. The driver previously
   sent only the later CAP overrides. It now sends the ten captured board
   commands before XNU starts MHI, including `s3c=612`, `s1c=944` and `s6c=1820`
   mV retention defaults absent from this firmware's CAP table. Board values are
   sent literally; CAP rounding and margins apply only to subsequent overrides.
   UEFI has already powered the endpoint at handoff; this restores the missing
   pre-firmware configuration, not the complete original Android power sequence.
2. **WLAN_EN capability.** The captured node has `wlan-en-gpio=16`, and CNSS
   declares `CNSS_WLAN_EN_SUPPORT` for that property. HOST_CAP now sends feature
   mask `0x0c` (WLAN_EN plus QDSS_CFG_MISS), instead of `0x08`. This communicates
   the existing board wiring; it does not toggle the GPIO or select a new clock.
3. **SMMU address-range declaration.** CNSS advertises its IOVA aperture when
   stage-1 translation is enabled. HOST_CAP TLV `0x1f` now describes the actual
   XNU arena `[0x20000000, 0x24000000)`, using the schema's three fixed 16-byte
   entries with the unused two zeroed. This is separate from the narrower QMI
   allocation pool beginning at `0x21400000`. The Android IPA range and host
   physical RAM are not advertised. Missing, unaligned, overflowing or
   non-containing ranges are rejected before QMI startup.

Both PDC phases use the existing ownership-checked QMP transport, count only
consumed messages, and require a confirmed close. A failure stops bring-up;
uncertain channels remain owned. Defaults cannot be replayed over CAP calibration.
The ordering remains `defaults -> MHI -> HOST_CAP -> memory -> CAP/PMU ->
REGDB/BDF/M3 -> FW_INIT_DONE -> CE/WLAN_MODE/WMI`. Moving WLAN_MODE before
FW_INIT_DONE would contradict the vendor's ready-handler/probe sequence.

The source basis is [CNSS power.c](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/wlan/platform/cnss2/power.c),
[CNSS HOST_CAP](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/wlan/platform/cnss2/qmi.c),
and the captured DTS hash already recorded in `hardware/sm8650-wifi.json`.
The pinned source and captured DTS identities remain recorded for the startup audit.

Validation on 2026-10-24: the full Wi-Fi ASan/UBSan suite passes, including the
extracted production power adapter with failures at every default/override
command and close, and eight invalid IOVA-range cases. Driver-module checks pass;
repository unittest runs 316 tests with no failures and three skips. No new kernel
or UEFI image was built, and no device was booted. These are confirmed source
omissions, **not proof of the cause of the captured firmware timeout**.

The next device capture should show `board-defaults` consumed=10, HOST_CAP
features=0xc with the arena above, `cap-overrides` consumed=7 for the previously
observed CAP, then `provisioned=1` and an actual `FW_INIT_DONE (0x38)` indication.
If the last indication is still `0x37`, firmware readiness remains unresolved;
do not promote M3's successful QMI response or AOP consumption to readiness.

### Earlier QMP and QMI validation

The QMI suite passes with ASan/UBSan, including all 13 defined memory types, partial allocation failure, malformed CMEM metadata and address overflow. Both XNU and UEFI QMP headers pass the same 86-case fault model, including the captured remote-open state, dropped ACK writes, changed remote state, inherited ownership and unconsumed-message retention. Portable headers compile as C++14.

A new kernel/UEFI build and device run must still confirm successful QMP open, all seven PMU command consumption acknowledgements, M3 provisioning and `FW_INIT_DONE`. Passing these host tests does not establish firmware readiness or networking. Sleep-clock, TME and auxiliary-firmware policies are unchanged: the captured Kiwi node has no `qcom,sleep-clk-support`, and the reviewed CNSS TME/auxiliary paths apply to Peach rather than Kiwi.

### 2026-10-24: readiness validation and shutdown evidence

The next source revision validates optional `FW_INIT_DONE` TLVs `0x10/0x11/0x12`
as u32/u16/u64 before setting readiness or its success timestamp, following the
pinned WLANFW decoder schema. An empty indication remains valid. Memory request
and allocation-failure diagnostics are now logged before returning an error.
Neither correction establishes the cause of the older firmware timeout.

Shutdown now follows the pinned OnePlus `mhi_pm_disable_transition`: request
RESET, wait for it to clear, rewrite and read back BHI_INTVEC, check the execution
environment, wait for READY when it is outside PBL/PTHRU/EDL, then disable and
drain PCIe bus mastering. The 25000 argument is a 25 ms polling interval;
each wait retains the 10000 ms CNSS budget. A failed step retains its first
failure and quarantine even if PCIe subsequently drains; a second stop cannot
convert that failure into successful retirement. The implementation has no
fatal-link PM state or RDDM image permitting the vendor reset exceptions.
See [pinned MHI PM](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/bus/mhi/host/pm.c)
and [polling implementation](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/bus/mhi/host/main.c).

`wifi.start` now releases only the currently published pre-DMA wait once;
early, repeated and post-stop requests are rejected. Cached `wifi.status`
adds MHI/CE/DP/SMMU ownership and reset/READY/master-stop evidence. A done
phase alone does not prove successful shutdown. These source changes need the
coordinated kernel build and device capture; they do not make the built-in
Wi-Fi driver hot-reloadable or provide the missing cold-power/recovery cycle.
UEFI integration remains after successful terminal/device validation.

### Protocol module integration: source awaiting coordinated validation

`giulia_wifi_protocol_module.c` implements `giulia-wifi-proto`, a GH domain 2
NLH2 service with interface ABI `0x47480001` and state ABI 1. Real QMI responses
and indications, plus known core WMI events, reach
`giulia_wifi_hotload_decide → giulia_hotload_select`. The resident owner keeps
hardware, deadlines, state transitions, DMA, keys and all callbacks.

The 32-word request carries metadata and up to 29 TLV header shapes. No field
values, serial numbers, addresses, MAC/SSID/IE or keys enter the module. Larger
streams use the complete resident parser. Unknown WMI events retain their
existing dispatch; repeated WMI arrays remain valid. A native result is adopted
only in the same owner epoch when it matches the full resident validation.
This permits equivalent parser implementation replacement within the current
acceptance rules; it does not establish arbitrary protocol-behavior repair or
whole-driver replacement.

START runs six parser self-checks and opens private admission; STOP closes it
and checks that private calls drained. Only an 80-byte counter format migrates
from the 128-byte private state. `GWPR_FAIL_START/IMPORT/STOP` support explicit
lifecycle fault builds. Driver `wifi.status` reports real receive calls, native
adoptions, fallback, mismatch and per-QMI/WMI counts. Module scalar calls 0..6
read counters only and never grant request access or issue hardware operations.

The coordinated build and automation must validate linked incoming/outgoing
calls, lifecycle/rollback, and real protocol input separately from module upload.
Live scans can exercise WMI events after an upgrade; ordinary HTT traffic cannot.
Pre-QMI hardware failure blocks real protocol coverage even when synthetic
fixtures pass. Firmware readiness and networking retain their independent
device acceptance requirements.
