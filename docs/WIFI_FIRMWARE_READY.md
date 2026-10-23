# WCN7850 firmware readiness evidence

The latest completed test on 2026-10-23 stops at QMP channel open, before BDF/M3 provisioning. It does not reach the older 40-second `FW_INIT_DONE` wait.

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

The QMI suite passes with ASan/UBSan, including all 13 defined memory types, partial allocation failure, malformed CMEM metadata and address overflow. Both XNU and UEFI QMP headers pass the same 86-case fault model, including the captured remote-open state, dropped ACK writes, changed remote state, inherited ownership and unconsumed-message retention. Portable headers compile as C++14.

A new kernel/UEFI build and device run must still confirm successful QMP open, all seven PMU command consumption acknowledgements, M3 provisioning and `FW_INIT_DONE`. Passing these host tests does not establish firmware readiness or networking. Sleep-clock, TME and auxiliary-firmware policies are unchanged: the captured Kiwi node has no `qcom,sleep-clk-support`, and the reviewed CNSS TME/auxiliary paths apply to Peach rather than Kiwi.
