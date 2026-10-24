# Audio protocol hotload integration

[简体中文](AUDIO_HOTLOAD.md). No new target-build or device acceptance result is claimed.

`giulia-audio-proto` is a GH ABI 1 service with interface `0x47480001`, state ABI 1,
and no dependencies. It classifies packets on the actual SPF/APM receive paths.
ADSP, IRQs, DMA, firmware, graphs and amplifiers remain resident. The native and
PCM providers are still missing; loading this service does not enable playback.

| Pinned source | Actual call site | Responsibility |
| --- | --- | --- |
| OnePlus audio-kernel `453d8875...`, `gpr-lite.h/c`, `spf-core.c` | `gat_poll_one` → `gat_spf_receive` | Module classification; resident epoch/token validation and adoption |
| AudioReach `76d04ffb...`, `gsl_main.c`, GPR `gpr_packet.h` | `gar_start` readiness loop | Diagnostic BasicRsp; resident retries, deadlines and quarantine |
| Same revision, `gsl_common.c`, `gsl_spf_timeout.h` | `gag_receive`, `gag_submit` | Module ACK classification; resident deadlines, graph state and DSP errors |

SPF has a five-second budget separate from GLINK negotiation, a one-second query
deadline and a 50ms retry interval. Resident `gar_ops.now_ms` is monotonic; polling,
hotload and scheduling time count toward deadlines. GET_SPF_STATE BasicRsp,
including status zero, never proves readiness. Typed state other than one retries.
Retries use new tokens; expired queries are discarded before and after polling.
APM OPEN/PREPARE/START-STOP/other deadlines are 4/2/5.5/1 seconds, matching pinned
GSL defaults. Clock rollback, fatal/SSR, lease and transport failures quarantine.

## Value-only request

`giulia_audio_proto_wire.h` uses 17 u64 words: ABI, operation, token, local port,
remote port, original opcode, packet length, pending, and nine little-endian packet
words. It supports 24–60-byte headers and 4/8-byte payloads, at most 68 bytes.
Client data's low nibble is valid; reserved bits must be zero. The bridge copies
the request; modules receive an invocation-bound ticket rather than a pointer.

One response u64 contains a low32 verdict (0 IGNORE, 1 SPF_READY, 2 SPF_STATE,
3 SPF_BASIC, 4 APM_ACK, 5 APM_ERROR) and the raw32 DSP status in its upper half.
The resident accepts the baseline result or stricter IGNORE. Invalid output,
unavailable modules and update admission failures use the resident fallback.
Epoch, pending, token, opcode and deadline are checked again before adoption.

The hardware epoch comes from the native lease and is bound by `gan_prepare`.
Graph providers call `gag_bind_epoch` while CLOSED; unbound graphs cannot send.
NLH START/STOP only enable/disable private logic. EXPORT/IMPORT migrate 32 bytes
of counters. Hardware STOP still requires actual PA/PCM/IPC/PAS/IRQ/DMA completion
and checked resource release.

## Automated acceptance

`audio.status` returns cached service, firmware, worker, provider and primary/
cleanup errors even with audio mode disabled. Preflight rejection distinguishes
disabled mode, invalid firmware, failed/offline worker and busy states.
`local_quiesced` is not hardware-stop evidence.

`audio.proto` requires `giulia_debug=1` and works with audio mode disabled or no
provider. Independent fixture objects call the real receive functions: 20 scenarios,
expected bitmap `0xfffff`, 16 selections/adoptions per normal run. Exact counts
require mode0 or provider0 and `counter_exact=1`; live worker traffic can otherwise
contribute. Unloaded modules should produce 16 fallbacks. BAD_REPLY produces 16 rejected responses/fallbacks
while all scenarios still pass. Output always says `synthetic=1 hardware_verified=0`.
Fixtures perform no device writes or lease grants. They use minimum logical epoch 1
or the most recently published native epoch. Stale-epoch stress that raises the
domain high-water mark belongs at the end or requires a subsequent reboot before
real lower-epoch hardware use.

After GET_META is denied, generic CALL 0/1/2/3/4 may read private calls/imports/ignored/
APM-errors/version. Valid resident tickets, including small values, take the request path
first. These diagnostics grant no USB access to requests or hardware.

Dedicated fault macros are `GAPT_FAIL_START`, `GAPT_FAIL_IMPORT`, `GAPT_FAIL_STOP`,
and `GAPT_BAD_REPLY`: zero disables, one enables. START failure occurs before any
state change and is not partial hardware-start coverage. The coordinator runs
unified acceptance. Upload, lifecycle, synthetic protocol and real DSP/PCM/audible
output receive separate verdicts; missing providers mean hardware BLOCKED.
The suite uses `giulia_audio_hotload_module.c` and `GH_MODULE_VERSION/GH_FAIL_*`;
the standard failure macros map to the corresponding GAPT branches.
