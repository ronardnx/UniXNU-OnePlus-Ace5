"""Vendor-order HWSCHED cold start and the first observable GPU commands.

Mirrors the pinned OnePlus SM8650 graphics kernel, not an invented protocol:

  gen7_hwsched_hfi_start()   feature controls in vendor order, CORE_FW_START,
                             context-queue query, hw fence / preemption / LPAC /
                             AQE, then H2F_MSG_START
  gen7_hwsched_gpu_boot()    microcode bases, CP_INIT as an *asynchronous*
                             H2F_MSG_ISSUE_CMD_RAW message
  submit_raw_cmds()          completion is the busy-bit poll, never an HFI ACK;
                             message 131 has no reply in the firmware protocol
  gen7_hwsched_cp_init()     ZAP, then CP_SET_SECURE_MODE(0)
  gen7_cp_init_cmds()        CP_ME_INIT ordinal mask 0x14b for this GPU:
                             bit 11 means "DMS unsupported" and gen7_9_1
                             declares ADRENO_DMS, so 0x94b is wrong here

Everything is bounded and every step records what the hardware actually
answered. A rejected or unconfirmed step stops the run.
"""
import time

from gpu_terminal_cp_secure import packet, init_commands

FEATURE = 11
GET_VALUE = 12
CORE_FW_START = 14
START = 10
ISSUE_CMD_RAW = 131

# gmu_ret_type: only GMU_SUCCESS is success. NOT_SUPPORTED/NO_ENTRY may be
# tolerated for optional features exactly like the vendor's -ENOENT paths.
GMU_SUCCESS = 0
GMU_NOT_SUPPORTED = 7
GMU_NO_ENTRY = 8
OPTIONAL_ERRORS = (GMU_NOT_SUPPORTED, GMU_NO_ENTRY)

CONTEXT_QUEUE = 121


def _preempt_data(report):
    """enable_preemption(): bits[1:0] level, bit2 gmem, bit3 skipsaverestore."""
    level = int(report.get('preempt_level', 1))
    return (level & 3) | (int(bool(report.get('preempt_usesgmem', False))) << 2) \
        | (int(bool(report.get('preempt_skipsaverestore', False))) << 3)


def feature_plan(report):
    """(name, feature id, enable, data, optional) in gen7_hwsched_hfi_start order."""
    return [
        ('acd', 12, 1, 0, True),
        ('bcl', 11, 1, int(report.get('bcl_data', 1)), True),
        ('clx', 21, 1, 0, True),
        # IFPC is only enabled when the platform supplies its hysteresis value.
        ('ifpc', 9, 1, int(report.get('ifpc_hyst', 0)), True),
        ('hwsched', 1, 1, 0, False),
        ('kprof', 16, 1, 0, True),
        ('dms', 27, 1, 0, True),
        ('perf_noretain', 26, 0, 0, True),
        ('bail_out_timer', 17, 1, 0, True),
    ]


def post_core_plan(report):
    """HW_FENCE and LPAC are only enabled when the platform can back them.

    Stock enables HW_FENCE only after adreno_hwsched_register_hw_fence() has
    published a queue, and enables LPAC only when the LPAC soft fuse is set.
    Enabling either without that backing asks the GMU for resources nobody
    provided, so both stay off unless the caller states the prerequisite.
    """
    plan = []
    if report.get('hw_fence_provider_registered'):
        plan.append(('hw_fence', 25, 1, 0, True))
    plan.append(('preemption', 2, 1, _preempt_data(report), True))
    if report.get('lpac_enabled'):
        plan.append(('lpac', 24, 1, 0, True))
    plan.append(('aqe', 29, 1, 0, False))
    return plan


def send_feature(transport, report, name, feature, enable, data, optional):
    reply = transport.send(FEATURE, [feature, enable, data])
    value, status = reply['value'], reply['status']
    entry = {'feature': name, 'id': feature, 'enable': enable, 'data': data,
             'value': value, 'status': status}
    if value != GMU_SUCCESS:
        if optional and value in OPTIONAL_ERRORS:
            entry['outcome'] = 'unsupported'
            report.setdefault('feature_rejections', []).append(entry)
            return entry
        entry['outcome'] = 'rejected'
        report.setdefault('feature_rejections', []).append(entry)
        raise RuntimeError(f'HFI feature {name} rejected: {reply}')
    entry['outcome'] = 'enabled'
    report.setdefault('features_accepted', []).append(entry)
    return entry


def start(transport, report):
    """The full vendor start sequence. Returns the context-queue capability."""
    if getattr(transport, 'started_by_stock_sequence', False):
        raise RuntimeError('HWSCHED start already attempted on this transport')
    transport.started_by_stock_sequence = True
    report['hwsched_start_order'] = 'vendor-gen7_hwsched_hfi_start'
    for args in feature_plan(report):
        send_feature(transport, report, *args)
    reply = transport.send(CORE_FW_START, [0])
    report['core_fw_start_reply'] = reply
    if reply['value'] != GMU_SUCCESS:
        raise RuntimeError('CORE_FW_START rejected: ' + str(reply))
    report['core_fw_start_accepted'] = True
    reply = transport.send(GET_VALUE, [CONTEXT_QUEUE, 0])
    report['context_queue_reply'] = reply
    report['context_queue'] = reply['value'] if reply['status'] in (0, None) else None
    if reply['status'] not in (0, None):
        raise RuntimeError('Context-queue query failed: ' + str(reply))
    for args in post_core_plan(report):
        send_feature(transport, report, *args)
    reply = transport.send(START, [])
    report['start_reply'] = reply
    if reply['value'] != GMU_SUCCESS:
        raise RuntimeError('HWSCHED START rejected: ' + str(reply))
    report['hwsched_start_accepted'] = True
    return report['context_queue']


def _busy(config):
    return bool(config.rd('GEN7_GPU_GMU_AO_GPU_CX_BUSY_STATUS') & (1 << 23))


def wait_idle(config, report, label, limit_us=200):
    """submit_raw_cmds(): poll the busy bit; the vendor bound is 200 us."""
    waited, polls = 0, 0
    while True:
        polls += 1
        if not _busy(config):
            report.setdefault('idle_polls', {})[label] = polls
            return True
        if waited >= limit_us:
            report.setdefault('idle_polls', {})[label] = polls
            return False
        time.sleep(0.00001)
        waited += 10


def _faults(live):
    return {
        'cp_hw_fault': live.read(0x3d00000 + 4 * 0x821),
        'cp_interrupt_status': live.read(0x3d00000 + 4 * 0x823),
        'cp_protect_status': live.read(0x3d00000 + 4 * 0x824),
        'rbbm_status': live.read(0x3d00000 + 4 * 0x210),
        'rbbm_status3': live.read(0x3d00000 + 4 * 0x213),
    }


def _publish_raw(live, transport, report, label, words):
    """No HFI ACK exists for message 131; the header is all the firmware returns."""
    report['pending_cp_operation'] = label
    (live.capture.root / 'cp-pending.txt').write_text(label + '\n' + repr(words) + '\n')
    header = transport.packet(ISSUE_CMD_RAW, words)
    report.setdefault('raw_messages', []).append({'label': label, 'header': hex(header)})
    return header


def boot(live, transport, config, report, powerup_gpu, dms=True, fence=False):
    """CP_INIT, ZAP, secure-mode exit, one proven GPU write and an optional fence.

    Preconditions: mapping policy applied, SQE/AQE copied, register window
    configured, OOB/GX held. Each step below is bounded and recorded.
    """
    commands = init_commands(powerup_gpu, False, dms=dms)
    report['cp_init_mask'] = hex(commands[3])
    report['cp_init_dms'] = bool(dms)
    _publish_raw(live, transport, report, 'cp_init', commands)
    report['cp_init_idle'] = wait_idle(config, report, 'cp_init')
    if not report['cp_init_idle']:
        report['cp_init_faults'] = _faults(live)
        raise RuntimeError('CP_INIT did not reach idle within the bound')
    report['cp_init_faults'] = _faults(live)
    if any(report['cp_init_faults'][k] for k in
           ('cp_hw_fault', 'cp_interrupt_status', 'cp_protect_status')):
        raise RuntimeError('CP reported a fault during CP_INIT: ' + str(report['cp_init_faults']))
    report['cp_init_complete'] = True

    # ZAP loads/authenticates the secure firmware through SCM/PAS. The vendor
    # then leaves secure mode with a CP command, not a register poke.
    report['zap_reply'] = transport.platform(7)
    if 'authenticated=1' not in report['zap_reply']:
        raise RuntimeError('ZAP was not authenticated: ' + report['zap_reply'])
    _publish_raw(live, transport, report, 'switch_unsecure', [packet(0x66, 1), 0])
    report['switch_unsecure_idle'] = wait_idle(config, report, 'switch_unsecure')
    report['secvid_trust_control'] = config.rd('GEN7_RBBM_SECVID_TRUST_CNTL')
    if report['secvid_trust_control'] != 0:
        raise RuntimeError('GPU still in secure mode: ' + hex(report['secvid_trust_control']))
    report['secure_mode_exit'] = True

    target = transport.allocate(4096, 4096, 0x33f, 2)
    transport.write(target['offset'], [0] * 4)
    if transport.read(target['offset'])[0] != 0:
        raise RuntimeError('Owned write target is not clear')
    _publish_raw(live, transport, report, 'cp_mem_write',
                 [packet(0x3d, 3), target['gpu'], 0, 0xa750cafe])
    report['cp_mem_write_idle'] = wait_idle(config, report, 'cp_mem_write')
    report['cp_mem_write_value'] = transport.read(target['offset'])[0]
    if report['cp_mem_write_value'] != 0xa750cafe:
        report['post_write_faults'] = _faults(live)
        raise RuntimeError('No GPU memory write observed: ' + hex(report['cp_mem_write_value']))
    report['gpu_memory_write_verified'] = True

    if fence:
        transport.write(target['offset'] + 4, [0])
        _publish_raw(live, transport, report, 'cp_event_write',
                     [packet(0x46, 3), 4, target['gpu'] + 4, 0])
        deadline = time.monotonic() + 0.2
        value = 0
        while time.monotonic() < deadline:
            value = transport.read(target['offset'] + 4)[0]
            if value:
                break
            time.sleep(0.00001)
        report['cp_fence_value'] = value
        if not value:
            raise RuntimeError('No GPU timestamp was written for the fence')
        report['gpu_fence_verified'] = True
    return report
