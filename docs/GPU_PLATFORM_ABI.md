# GPU platform development ABI 3

The existing `gpu.dev TOKEN OP A B C D` worker protocol is extended without
changing the initial ABI 1 pool/status line. `OP=11,A=0` identifies platform ABI 3.
Every operation still requires `giulia_debug`, ready service, and explicitly pinned
memory. The USB parser never executes SCM. No accelerator is published by this ABI.

| OP | Arguments | Behavior |
|---|---|---|
| 11 | action,0,0,0 | Fixed platform actions below |
| 12 | bytes,alignment,flags,kind | Allocate an already owned/mapped secure slot; only flags 0x1007/kind 5 |
| 13 | staged HFI word count,timeout ms,0,0 | RAW 131 dispatch with bounded kernel polling and snapshots; maximum 1000 ms |
| 14 | 0,0,0,0 | CX/AO/SMMU snapshot without modifying faults or interrupts |
| 15 | log word offset,count,0,0 | Read at most 64 words from owned HFI log RAM |
| 16 | 0,0,0,0 | Audit all normal-pool GPU/GMU leaves and unused aliases against owned allocation records |

Platform actions: 0 reports state; 1 prepares isolated resources and probes SCM64,
secure-CX, memory assignment and aperture availability; 2 initializes fixed CX request
0xB; 3 configures GPU bank-0 aperture; 4 assigns secure ownership; 5 activates bank 3 /
SID 2; 6 removes that stream, disables its context and synchronizes TLB after held CM3
reset; 7 loads/authenticates the fixed ZAP using PAS 13; 8 resets the SCM epoch only
before any ownership call and with all streams/banks inactive; 9 enables the
per-allocation mapping policy while all banks/streams are inactive.

Action 9 invalidates every unused normal-pool GPU/GMU leaf and both possible GMU
aliases, while preserving GMU firmware/HFI/log/VRB mappings. Each allocation then
enables only its requested masters and the selected GMU address region. GPU data
permissions follow GFX_ACC/PRIV/WRITEABLE and are NX. GMU permissions follow the
requested GMU access bits, remain Normal NC, and retain executable permission as
in the pinned GMU import path. No GFX access means no GPU leaf; no GMU access means
neither GMU alias is valid. CPU pool access remains a diagnostic facility rather
than a per-allocation HOST_ACC security boundary.

Copying a verified SQE/AQE asset into one owned allocation through OP7 marks that
allocation GPU read-only, executable and accessible to privileged or unprivileged
fetches, with no GMU alias. Dynamic changes use break-before-make, table cache
publication and owned CB0/CB2 TLB synchronization. Active contexts must still use
the expected roots and have no valid fault; inactive reset TTBR values are ignored.
Errors quarantine mappings and preserve backing memory. OP16 verifies actual
software PTEs for all 33,792 normal GPU/GMU/cache-alias pages, not hardware ATOS.

The secure pool has eight 4 MiB virtual slots backed by 512 separately allocated
64 KiB physical segments, outside the ordinary 64 MiB GPU/GMU pool. GPU secure VA
begins at 0xf0000000. This avoids demanding 4 MiB contiguous RAM after userspace
has fragmented the allocator. A 128 KiB independent
page-table allocation is shared HLOS RW + CP_PIXEL R. Data is transferred in batches
of 32 regions (2 MiB total) from HLOS to CP_PIXEL RW. Data is zeroed before assignment and
never CPU-read, CPU-written or exposed through development offsets afterward. No GMU
or ordinary GPU mapping is created. Secure GPU data is privileged RW and NX.
Table/data ownership calls use separate coherent
control memory and exact little-endian 16-byte region / 24-byte permission records.

All resources are pinned before a call that might change ownership. Any ambiguous
failure quarantines the pool; it is not returned to normal allocation. This diagnostic
retains secure ownership until reboot and does not implement production reclamation.
The secure context is disabled/TLB-synchronized before host PRR marker restoration.

ZAP metadata is validated from the digest-checked embedded firmware, copied into
separate NC RAM, and passed to PAS_INIT_IMAGE. The fixed DT carveout 0x9b09a000..0x9b09bfff
is used for the relocatable load after PAS_MEM_SETUP and before PAS_AUTH_AND_RESET.
No CPU verification read is issued after authentication.

SCM preserves full x0–x7, original extended arguments and interrupted x6 cookies.
Returned BUSY/INTERRUPTED retries are bounded. WAITQ is quarantined because this
diagnostic has no SCM waitqueue owner. A nonreturning SMC cannot be timed out by the
kernel helper. Host timeout or disconnect stops all further device I/O for the run.
The user requires immediate conversation pause on USB 900E: no recovery or replay.

Fault capture is bounded **polling**, not an installed GPU/GMU interrupt handler.
Host interrupts remain masked. Explicit snapshots avoid RBBM_INT_0_STATUS, use its
AO mirror, and read FAR64/syndrome only for a valid FSR; they do not read GX. RAW
watch logs preflight, queue publication and doorbell boundaries, then polls only
owned HFI RAM. A packet arrival is not an ACK until host validation. A timeout or
corrupt response quarantines the run without automatic MMIO or cleanup. RAW caller
must retain its acknowledged OOB power hold throughout submission. Asynchronous
console breadcrumbs cannot guarantee persistence through an abrupt SoC failure.
CP follows stock first-RAW
CP_THREAD_CONTROL+CP_ME_INIT, followed by ZAP, secure-mode exit and an owned memory write.
No stage's ACK alone proves GPU rendering, a production fence, or Metal support.

Sources are the fixed revisions and permissions described in
[the source audit](GPU_SECURE_START_AUDIT.md). Host verification covers actual methods,
ownership argument layouts, independent table walks, permission refusal, resource
exhaustion and quarantine. Device outcomes belong in the integration/evidence report.
