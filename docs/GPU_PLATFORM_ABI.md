# GPU platform development ABI 3

The existing `gpu.dev TOKEN OP A B C D` worker protocol is extended without
changing the initial ABI 1 pool/status fields. `OP=0` additionally advertises
`resources_abi=1 memstore=1 secure_allocation_mappings=1`.
IPC-capable builds also advertise a separate
`ipc_core=1 ipc_host_arbitration=1 ipc_retained_until_reboot=1` line.
The separate `fence_import_abi=1 fence_completion=0` line advertises the import
interface, not a registered controller or working hardware fences.
`OP=11,A=0` identifies platform ABI 3.
Every operation still requires `giulia_debug`, ready service, and explicitly pinned
memory. The USB parser never executes SCM. No accelerator is published by this ABI.

| OP | Arguments | Behavior |
|---|---|---|
| 11 | action,0,0,0 | Fixed platform actions below |
| 12 | bytes,alignment,flags,kind | Map the requested pages in an already owned secure slot; only flags 0x1007/kind 5 |
| 13 | staged HFI word count,timeout ms,0,0 | RAW 131 dispatch with bounded kernel polling and snapshots; maximum 1000 ms |
| 14 | 0,0,0,0 | CX/AO/SMMU snapshot without modifying faults or interrupts |
| 15 | log word offset,count,0,0 | Read at most 64 words from owned HFI log RAM |
| 16 | 0,0,0,0 | Audit normal-pool GPU/GMU leaves, unused aliases, memstore, secure allocations and IPC_CORE mappings/owner |
| 17 | bytes,wire alignment,flags,22 | Import the pinned 32 KiB memstore singleton; return fixed GPU/optional GMU IOVA and no development-pool offset |
| 18 | context,0,0,0 | Observe memstore SOP/EOP twice with read barriers; report sample stability and completion_verified=0 |
| 19 | bytes,wire alignment,flags,19 | Import fixed IPC_CORE into GMU only after excluding host IPCC clients; no CPU offset and no writes to resource contents |
| 20 | bytes,wire alignment,flags,23 | Import a real kernel-registered CTX0 descriptor into GMU 0x68000000 after live epoch/ownership/queue validation; absent provider returns 7 |
| 21 | 0,0,0,0 | Read provider registration/readiness/mapping state and valid queue indices; completion_verified=0 |
| 22 | action,b,c,0 | Native Gunyah owner: 0 historical status; 1 full query probe with b=timeout ms; 2 poll with b=frames/c=timeout ms; 3 compatibility/channel bootstrap only with b=timeout ms; 4 invalidate |

Platform actions: 0 reports state; 1 prepares isolated resources and probes SCM64,
secure-CX, memory assignment and aperture availability; 2 initializes fixed CX request
0xB; 3 configures GPU bank-0 aperture; 4 assigns secure ownership; 5 activates bank 3 /
SID 2; 6 removes that stream, disables its context and synchronizes TLB after held CM3
reset; 7 loads/authenticates the fixed ZAP using PAS 13; 8 resets the SCM epoch only
before any ownership call and with all streams/banks inactive; 9 enables the
per-allocation mapping policy while all banks/streams are inactive.

Before the first root-table publication, all normal-pool GPU/GMU leaves, both
possible GMU address regions and dedicated memstore leaves have their valid bits
cleared. Topology and owned physical-address tombstones remain. Action 9 audits
and applies per-allocation policy while preserving GMU firmware/HFI/log/VRB
mappings. Each allocation then enables only its requested masters and the
selected GMU address region. GPU data
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
software PTEs for all 33,792 normal GPU/GMU/cache-alias pages, both memstore
views and all secure data pages, not hardware ATOS.

The ordinary allocation pool rejects overlapping/corrupt records and reserved
resource kinds. Memstore and IPC_CORE use dedicated providers. Hardware-fence
queues require the kernel-only registered-provider contract described in
[GPU_FENCE_PROVIDER.md](GPU_FENCE_PROVIDER.md); no actual Gunyah producer is
included, so default kind23 requests remain unavailable.
The ordinary gen7 SYSRAM path also handles kinds11/12/13/20 despite their
MMIO enum names. For HFI requests without GFX_ACC, the host normalizes the
effective mapping flags with 0x38, matching the source's forced GMU privileged
RW branch, while preserving the original reply flags and request signature.
CPU mutations in managed mode require a single allocation owner. SQE/AQE copy
revokes device access before changing bytes, clears padding, then seals the
allocation; UCODE identity is also retained when loaded before action 9. Sealed
allocations reject subsequent CPU writes, zeroing and copies. Mapping failure
retains its record and quarantines subsequent mutations and HFI submission.
Active normal and secure contexts are checked against their programmed
translation/stream profiles, including high TTBR and ASID bits. Host HFI requests
reuse matching `(kind, GMU handle)` records without reinitializing live memory.
The native HWSCHED controller requires explicit `--platform`, ABI3 policy and
the dedicated-resource capability line before programming device state. Failed
secure or memstore publication, and incomplete allocation success records, stop
host device I/O rather than entering raw-register cleanup.
See [the mapping coverage and limits](GPU_MAPPING.md).

IPC_CORE binds the captured qcom,ipc-core physical range 0x400000/0x140000 to
GMU IOVA 0x67000000. All 320 leaves start invalid. OP19 accepts only kind19,
nonempty logical size at most 0x140000, flags within 0x78 with GMU_ACC set,
VA alignment exponent at most 22 and size exponent at most 18. It maps the
complete fixed resource using vendor Normal NC attributes and requested GMU
permissions, without a GPU alias, CPU mapping, RAM allocation or zeroing.
Flags cannot change after import; matching imports reuse the same resource.

The broker permits simultaneous Wi-Fi/audio host membership but excludes GMU
aperture ownership while either is held. Wi-Fi membership covers actual IPCC
map/unmap lifetime, including after QMP close; audio membership lasts from
successful provider registration until reboot. GPU import requires a clean
UEFI QMP handoff and retains its broker token until reboot even after failed
publication. Claim checks acquire matching release state; teardown rollback
uses an exact-token CAS and cannot fail merely because another claim is busy.
This is local driver arbitration, not a grant of power or remote-VM authority.
GPU-enabled terminal builds reject raw reg.map ranges overlapping IPC_CORE,
so raw debug aliases cannot bypass broker membership.
Malformed or unconfirmed OP19 responses stop subsequent host device I/O.

Fence import reserves one empty GMU L3 at boot, with no placeholder PA. A
registered provider's copied identity, epoch, layout and callbacks remain
pinned. It pairs with the existing GPU IPC owner, excluding QMP/audio and
preventing premature GPU lease release. OP20 maps only the validated queue
descriptor bytes, with all remaining 2 MiB aperture leaves zero and no GPU
alias. Accepted flags are within 0x178 with GMU_ACC required; GFX, HOST_INIT,
CACHEABLE and secure flags are rejected. Repeat HFI requests revalidate the
kernel provider even when their request key matches a host cache entry.

Once mapped, epoch/READY loss blocks ordinary memory/resource mutation and
HFI submission/consumption, as well as fence import/audit. All code8 kernel
responses stop host I/O, including raw-register cleanup. Mapping and stable
index observations never establish completion; feature25 is not enabled.

Memstore has independent, contiguous Normal NC backing at GPU IOVA 0x66000000.
GMU_ACC enables the same IOVA in the GMU domain with independently requested GMU
PRIV/WRITEABLE permissions; otherwise that leaf is invalid. GPU ACC/PRIV/WRITEABLE
must all be set, matching the pinned gen7-9-1 preallocated privileged, writable
device memstore. GPU data remains NX. GMU_CACHEABLE, secure and unknown flag bits
are rejected. A subsequent import cannot change the singleton's flags, and
HOST_INIT never clears live timestamps. OP17 requires managed mappings, validates
the owned descriptor and actual PTEs, then publishes/synchronizes the mapping
before reporting success. A matching import returns the same backing without
clearing it or consuming normal-pool space.

OP17 preserves the HFI alignment encoding: bits 0–7 are the VA alignment exponent
and bits 8–15 the size alignment exponent. The logical request must be nonempty,
both its size and aligned size must fit the fixed backing, and the IOVA must meet
the VA alignment. The kernel response reports the full 32 KiB provider size;
the HFI reply preserves the original logical request size. No development offset
is returned. A different GMU handle can refer to this same device-wide singleton
only with compatible flags and bounds; host request-key reuse still requires an
exact `(kind, GMU handle)` size/flags/alignment match.

The pinned vendor layout is ten u32 words per record: 40 bytes with SOP at +0 and
EOP at +8. Vendor allocation uses eight host pages. This provider explicitly
chooses eight 4 KiB pages, without asserting the stock kernel's configured
PAGE_SIZE or inheriting XNU PAGE_SIZE. Its context limit is 813: IDs 0–812 are
observable, global context 812 is reserved, and user IDs are 1–811. Ringbuffer
records and trailing incomplete storage are not context IDs. OP18 reports the
second of two ordered samples; `stable=1` means the sampled pairs matched. It
does not prove an atomic device snapshot, registered context, submission or
fence completion. It never advances a timestamp and always reports
`completion_verified=0`. Exact source revisions, hashes and supporting line
ranges are recorded in [the memstore source manifest](../hardware/gpu-memstore-source.json).

The secure pool has eight 4 MiB virtual slots backed by 512 separately allocated
64 KiB physical segments, outside the ordinary 64 MiB GPU/GMU pool. GPU secure VA
begins at 0xf0000000. This avoids demanding 4 MiB contiguous RAM after userspace
has fragmented the allocator. A 128 KiB independent
page-table allocation is shared HLOS RW + CP_PIXEL R. Data is transferred in batches
of 32 regions (2 MiB total) from HLOS to CP_PIXEL RW. Data is zeroed before assignment and
never CPU-read, CPU-written or exposed through development offsets afterward. No GMU
or ordinary GPU mapping is created. Secure GPU data is privileged RW and NX.
The complete 32 MiB page-table topology is built before ownership transfer, with
every data leaf initially invalid. OP12 enables only the requested length rounded
up to 4 KiB; unused slots and the remainder of each occupied 4 MiB slot stay
invalid. Allocation and OP16 audit compare actual descriptor PA/length metadata,
all slot records and all leaf descriptors without accessing CP_PIXEL-only data.
Publication and CB3 TLB synchronization must complete before `mapped=1` is
reported. Ambiguous failures preserve the backing and quarantine the pool.
Table/data ownership calls use separate coherent control memory and exact
little-endian 16-byte region / 24-byte permission records.

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
