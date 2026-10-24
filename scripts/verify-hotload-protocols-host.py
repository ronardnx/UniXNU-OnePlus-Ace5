"""Execute the four real protocol modules with a by-value host simulation.

Linux/WSL: python3 scripts/verify-hotload-protocols-host.py --kit KIT --out NEW
Compiles each real source and all five reviewed macro variants as host shared
libraries. This does not execute ARM64 images, the trusted NLH runtime, device
providers, IRQ/DMA/firmware or physical hardware. No serial/USB port is opened.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time
import shutil
import struct

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
DOMAINS = {'gpu': 1, 'wifi': 2, 'audio': 3, 'modem': 4}
SOURCES = {
    'gpu': 'ios16/gpu/hotload/giulia_gpu_proto_module.c',
    'wifi': 'ios16/wifi/giulia_wifi_protocol_module.c',
    'audio': 'ios16/audio/giulia_audio_hotload_module.c',
    'modem': 'ios16/modem/giulia_modem_proto_module.c',
}
VARIANTS = ('baseline', 'upgrade', 'fail-start', 'fail-import', 'fail-stop')
U64 = C.c_uint64
PLATFORM = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32, U64, U64, C.POINTER(U64))


class Host(C.Structure):
    _fields_ = [('abi', C.c_uint32), ('bytes', C.c_uint32), ('context', C.c_void_p),
                ('log', C.c_void_p), ('now_ns', C.c_void_p),
                ('platform', PLATFORM), ('call', C.c_void_p)]


class Transfer(C.Structure):
    _fields_ = [('abi', C.c_uint32), ('bytes', C.c_uint32), ('capacity', C.c_uint32),
                ('reserved', C.c_uint32), ('data', C.c_void_p)]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def json_sha(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True,
        separators=(',', ':')).encode()).hexdigest()


def check(value, message):
    if not value:
        raise AssertionError(message)


def words(data):
    return [int.from_bytes(data[at:at + 8].ljust(8, b'\0'), 'little')
            for at in range(0, len(data), 8)]


def vector(name, request, expected, operation=1, error=0, **options):
    return {'id': name, 'request': request, 'expected_response': expected,
            'operation': operation, 'expected_error': error, **options}


def gpu_request(*, flags=0, kind=0, size=24, align=0, legacy=False):
    count = 10 if legacy else 11
    packet = [20 | count << 8, 0 if legacy else 1, 0, 0, flags, kind, 0, 7, 0, size, align]
    return [1, count, *packet]


def gpu_vectors():
    # Literal protocol plans: 4096-byte backing, GMU-only folded 0x38 flags;
    # alternate GFX/GMU access and reserved kinds have independent bit goldens.
    values = [vector('modern-size24-backing4096', gpu_request(), 0x100000403800),
        vector('legacy10-size24', gpu_request(legacy=True), 0x100000403800),
        vector('gfx-only', gpu_request(flags=1), 0x100000200100),
        vector('gfx-and-gmu', gpu_request(flags=9), 0x100000600900),
        vector('reserved-memstore-no-allocation', gpu_request(kind=22), 0xc00000),
        vector('reserved-hw-fence', gpu_request(kind=23), 0xe00000),
        vector('secure-refused', gpu_request(flags=0x1000), 5),
        vector('unknown-flags', gpu_request(flags=0x400), 4),
        vector('kind26-out-of-range', gpu_request(kind=26), 3),
        vector('zero-size', gpu_request(size=0), 6),
        vector('rounding-overflow', gpu_request(size=0xffffffff), 6),
        vector('alignment-exponent31', gpu_request(align=31), 7)]
    short = [1, 1, 0x114, *([0] * 10)]
    values.append(vector('one-word-short-packet', short, 1))
    bad = gpu_request(); bad[2] = 0xa14
    values.append(vector('header-count-mismatch', bad, 2))
    bad = gpu_request(); bad[0] = 2
    values.append(vector('wire-schema-mismatch', bad, None, error=22))
    bad = gpu_request(); bad[6] |= 1 << 32
    values.append(vector('wire-high32-refused', bad, None, error=22))
    return values


def shape(tag, length, offset, header=3):
    return tag | length << 16 | offset << 32 | header << 48


def wifi_request(operation, message, flag, length, descriptors):
    return [1 | operation << 32, message | flag << 32,
            length | len(descriptors) << 32, *descriptors]


def wifi_verdict(status, count=0, offset=0xffff, present=0):
    # Public wire encoding, not a call to the production parser.
    return status | count << 8 | offset << 24 | present << 40


def wifi_vectors():
    q = lambda message, flag, length, ds: wifi_request(1, message, flag, length, ds)
    values = [vector('fw-ready-three-widths', q(0x38, 4, 23,
        [shape(0x10, 4, 0), shape(0x11, 2, 7), shape(0x12, 8, 12)]), wifi_verdict(0, 3, present=7)),
        vector('empty-fw-ready', q(0x38, 4, 0, []), wifi_verdict(0)),
        vector('unknown-unique-tlv', q(0x38, 4, 3, [shape(0x80, 0, 0)]), wifi_verdict(0, 1)),
        vector('fw-ready-wrong-u16-width', q(0x38, 4, 6, [shape(0x11, 3, 0)]), wifi_verdict(4, offset=0)),
        vector('duplicate-qmi-tag', q(0x37, 4, 6, [shape(0x20, 0, 0), shape(0x20, 0, 3)]), wifi_verdict(3, 1, 3)),
        vector('truncated-tlv-header', q(0x37, 4, 2, [shape(0, 0, 0, 2)]), wifi_verdict(2, offset=0)),
        vector('oversized-tlv-value', q(0x37, 4, 5, [shape(0x20, 7, 0)]), wifi_verdict(2, offset=0)),
        vector('shape-offset-gap', q(0x37, 4, 3, [shape(0x20, 0, 1)]), wifi_verdict(2, offset=0)),
        vector('invalid-qmi-flags', q(0x37, 1, 0, []), wifi_verdict(1, offset=0)),
        vector('wmi-repeat-tag-permitted', wifi_request(2, 0x3001, 0, 8,
            [shape(18, 0, 0, 4), shape(18, 0, 4, 4)]), wifi_verdict(0, 2), operation=2),
        vector('unknown-wmi-bypass', wifi_request(2, 0xffffff, 0, 0, []), wifi_verdict(5), operation=2),
        vector('maximum29-tlv-shapes', q(0x37, 4, 87,
            [shape(i, 0, i * 3) for i in range(29)]), wifi_verdict(0, 29)),
        vector('too-many-request-words', q(0x37, 4, 90,
            [shape(i, 0, i * 3) for i in range(30)]), None, error=22)]
    bad = q(0x38, 4, 0, []); bad[0] &= ~0xffffffff
    values.append(vector('wire-schema-mismatch', bad, wifi_verdict(1)))
    bad = q(0x38, 4, 0, []); bad[2] = 1 << 32
    values.append(vector('wire-shape-count-mismatch', bad, wifi_verdict(1)))
    return values


def audio_request(*, kind=1, status=1, basic=False, header_bytes=24, client=0):
    token, local, remote, original = 0x11223344, 0x2000, 0x3000, 0x01001000
    payload = [original, status] if basic else [status]
    size = header_bytes + 4 * len(payload)
    packet = [(size << 8) | (header_bytes // 4 << 4), 3 | 2 << 8 | client << 16,
              remote, local, token, 0x02001005 if basic else 0x02001007]
    packet += [0] * (header_bytes // 4 - 6)
    packet += payload
    raw = struct.pack('<' + 'I' * len(packet), *packet)
    return [1, kind, token, local, remote, original, size, 1,
            *words(raw), *([0] * (9 - len(words(raw))))]


def audio_vectors():
    values = [vector('spf-ready-status1', audio_request(), 0x100000001),
        vector('spf-state0', audio_request(status=0), 2),
        vector('spf-state2', audio_request(status=2), 0x200000002),
        vector('spf-basic-is-not-ready', audio_request(basic=True, status=0), 3),
        vector('apm-success-ack', audio_request(kind=2, basic=True, status=0), 4, operation=2),
        vector('apm-remote-status42', audio_request(kind=2, basic=True, status=42), 0x2a00000005, operation=2),
        vector('optional28-byte-header', audio_request(kind=2, basic=True, status=0, header_bytes=28), 4, operation=2),
        vector('maximum68-byte-packet', audio_request(kind=2, basic=True, status=0, header_bytes=60), 4, operation=2),
        vector('valid-client-low-nibble', audio_request(client=15), 0x100000001)]
    for name, index, value in (('wrong-token', 2, 0x11223345), ('wrong-local-port', 3, 0x2001),
            ('wrong-remote-port', 4, 0x3001), ('not-pending', 7, 0),
            ('short24-byte-packet', 6, 24), ('oversized69-byte-packet', 6, 69),
            ('wire-schema-mismatch', 0, 2)):
        request = audio_request(); request[index] = value
        values.append(vector(name, request, 0))
    request = audio_request(); request[8] |= 1
    values.append(vector('reserved-gpr-version', request, 0))
    request = audio_request(); request[8] |= 1 << (32 + 20)
    values.append(vector('reserved-domain-bits', request, 0))
    request = audio_request(kind=2, basic=True, status=0); request[11] ^= 1
    values.append(vector('wrong-original-opcode', request, 0, operation=2))
    return values


def tlv(tag, payload):
    return bytes([tag]) + struct.pack('<H', len(payload)) + payload


def qmi_packet(body, txn=0x1234, message=0x2f, flag=2):
    return bytes([flag]) + struct.pack('<HHH', txn, message, len(body)) + body


def modem_request(payload, *, kind=1, message=0x2f, txn=0x1234, origin=1):
    return [kind | txn << 32, len(payload) | message << 32, 0x12345678, origin, *words(payload)]


def modem_vectors():
    success = tlv(2, bytes(4))
    remote = tlv(2, struct.pack('<HH', 1, 42))
    values = [vector('qmi-result-success', modem_request(qmi_packet(success)), 0),
        vector('qmi-remote-error42', modem_request(qmi_packet(remote)), 0x2a04),
        vector('wrong-transaction', modem_request(qmi_packet(success, txn=0x1235)), 3),
        vector('wrong-message', modem_request(qmi_packet(success, message=0x24)), 3),
        vector('short-six-byte-header', modem_request(bytes(6)), 2),
        vector('duplicate-result', modem_request(qmi_packet(success + success)), 2),
        vector('missing-result', modem_request(qmi_packet(tlv(0x80, b''))), 5),
        vector('result-width3', modem_request(qmi_packet(tlv(2, bytes(3)))), 2),
        vector('invalid-status2', modem_request(qmi_packet(tlv(2, struct.pack('<HH', 2, 0)))), 2),
        vector('success-with-remote-error', modem_request(qmi_packet(tlv(2, struct.pack('<HH', 0, 1)))), 2),
        vector('unknown-unique-tlv', modem_request(qmi_packet(tlv(0x80, b'') + success)), 0),
        vector('unknown-duplicate-tlv', modem_request(qmi_packet(tlv(0x80, b'') * 2 + success)), 2),
        vector('invalid-qmi-flags', modem_request(qmi_packet(success, flag=0)), 2),
        vector('unsupported-kind', modem_request(qmi_packet(success), kind=0, message=0), 7)]
    payload = bytearray(qmi_packet(success)); payload[5] += 1
    values.append(vector('body-length-mismatch', modem_request(bytes(payload)), 2))
    for length in (223, 224):
        payload = qmi_packet(success + tlv(0x80, bytes(length - 17)))
        check(len(payload) == length, 'Independent QMI boundary fixture length')
        values.append(vector('payload-boundary' + str(length), modem_request(payload), 0))
    payload = qmi_packet(success + tlv(0x80, bytes(208)))
    check(len(payload) == 225, 'Independent oversized QMI fixture length')
    values.append(vector('oversized225-byte-wire', modem_request(payload), None, error=22))
    request = modem_request(qmi_packet(success)); request[-1] |= 1 << 63
    values.append(vector('nonzero-tail-padding', request, None, error=22))
    request = modem_request(qmi_packet(success)); request[0] |= 1 << 48
    values.append(vector('reserved-wire-bits', request, None, error=22))
    request = modem_request(qmi_packet(success)); request[3] = 2
    values.append(vector('unknown-source-label', request, None, error=22))
    return values


GOLDENS = {'gpu': gpu_vectors, 'wifi': wifi_vectors, 'audio': audio_vectors, 'modem': modem_vectors}


class Module:
    """Host-only META/request/response simulation; no runtime attestation."""
    def __init__(self, suite, domain, variant, path):
        self.suite, self.domain, self.variant = suite, domain, variant
        self.library = C.CDLL(str(path))
        self.state_bytes = C.c_uint32.in_dll(self.library, 'nlh_state_bytes').value
        check(0 < self.state_bytes <= 65536, 'Invalid module state size')
        self.state = (U64 * ((self.state_bytes + 7) // 8))()
        self.platform = PLATFORM(self.gateway)
        self.host = Host(2, C.sizeof(Host), 0x73696d31, None, None, self.platform, None)
        self.entry = self.library.nlh_entry
        self.entry.argtypes = [C.POINTER(Host), C.c_void_p, C.c_uint32, U64, C.POINTER(U64)]
        self.entry.restype = C.c_int
        self.mailbox = None
        self.serial, self.gateway_calls, self.callback_errors = 0, 0, []

    def gateway(self, context, operation, a, b, result):
        self.gateway_calls += 1
        error, value = 0, 0
        try:
            box = self.mailbox
            if context != self.host.context or not box or a != box['ticket']:
                error = 13
            elif operation == 0x4700 and b < 6:
                value = box['meta'][b]
            elif 0x4720 <= operation < 0x4720 + len(box['request']) and not b:
                index = operation - 0x4720
                value = box['request'][index]
                box['reads'].add(index)
            elif operation == 0x4760:
                error = box.get('write_error', 0)
                if not error:
                    box['response'] = b
                    box['writes'] += 1
            else:
                error = 22
            if not error:
                result[0] = value
        except Exception as problem:
            self.callback_errors.append(str(problem))
            error = 22
        self.suite.log({'type': 'simulated-platform', 'domain': self.domain,
            'op': operation, 'ticket': a, 'selector_or_value': b, 'error': error, 'result': value})
        return error

    def invoke(self, operation, argument=0):
        result = U64(0xabcdef)
        code = self.entry(C.byref(self.host), self.state, operation, argument, C.byref(result))
        self.suite.log({'type': 'actual-nlh-entry', 'domain': self.domain, 'variant': self.variant,
            'operation': operation, 'argument_kind': 'borrowed-transfer' if operation in (5, 6) else 'scalar',
            'argument': None if operation in (5, 6) else argument, 'error': code, 'result': result.value,
            'private_state_sha256': hashlib.sha256(bytes(self.state)).hexdigest()})
        check(not self.callback_errors, 'Host simulation callback failed: ' + str(self.callback_errors))
        return code, result.value

    def request(self, case, ticket=None):
        self.serial += 1
        ticket = ticket if ticket is not None else 0x4748000000000000 | self.serial
        meta = [DOMAINS[self.domain], 17, case['operation'], len(case['request']), 1, 1]
        for key, value in case.get('meta', {}).items():
            meta[int(key)] = value
        self.mailbox = {'ticket': ticket, 'request': case['request'], 'meta': meta,
            'response': None, 'writes': 0, 'reads': set(), 'write_error': case.get('write_error', 0)}
        box = self.mailbox
        self.suite.log({'type': 'independent-golden', 'case': case, 'ticket': ticket,
                        'meta': meta, 'source': 'host-synthetic-fixture'})
        try:
            code, result = self.invoke(4, ticket)
        finally:
            self.mailbox = None
        check(code == case['expected_error'], 'Entry error differs from independent expectation')
        if not code:
            check(box['response'] == case['expected_response'] and box['writes'] == 1,
                  'Actual response differs from independent protocol golden')
            check(box['reads'] == set(range(len(case['request']))), 'Module did not read the complete request')
        else:
            check(box['writes'] == 0 and box['response'] is None, 'Failed call published a response')
        return {'actual_error': code, 'actual_response': box['response'], 'entry_result': result,
                'request_reads': len(box['reads'])}

    def transfer(self, operation, payload=None, *, abi=1, reserved=0, byte_delta=0):
        capacity = 256 if payload is None else len(payload)
        buffer = (U64 * ((capacity + 7) // 8))()
        if payload:
            C.memmove(buffer, payload, len(payload))
        transfer = Transfer(abi, (0 if payload is None else len(payload)) + byte_delta,
                            capacity, reserved, C.addressof(buffer))
        before = self.gateway_calls
        code, unused = self.invoke(operation, C.addressof(transfer))
        check(before == self.gateway_calls, 'Lifecycle unexpectedly called the simulated platform')
        return code, bytes(buffer)[:transfer.bytes]

    def stopped_snapshot(self):
        check(self.invoke(3)[0] == 0, 'Normal STOP failed')
        code, payload = self.transfer(5)
        check(code == 0, 'Normal EXPORT failed')
        return payload, decode_snapshot(self.domain, payload)


def decode_snapshot(domain, payload):
    if domain == 'gpu':
        magic, size, number, reserved, calls, errors, imports = struct.unpack('<IIIIQQQ', payload)
        check((magic, size, number, reserved) == (0x31504747, 40, 1, 0), 'GPU snapshot framing')
        return dict(calls=calls, errors=errors, imports=imports)
    if domain == 'wifi':
        fields = struct.unpack('<IIIIQQQQQQQQ', payload)
        check(fields[:4] == (0x31535747, 1, 80, 1) and fields[-1] == 0, 'Wi-Fi snapshot framing')
        return dict(zip(('calls', 'accepts', 'rejects', 'bridge_errors', 'starts', 'stops', 'imports'), fields[4:11]))
    if domain == 'audio':
        return dict(zip(('calls', 'ignored', 'remote_errors', 'imports'), struct.unpack('<QQQQ', payload)))
    fields = struct.unpack('<IIIIQQQQQQ', payload)
    check(fields[:4] == (0x31504d47, 64, 4, 1), 'Modem snapshot framing')
    return dict(zip(('calls', 'native', 'fixture', 'rejected', 'remote', 'imports'), fields[4:]))


def deltas(domain, case):
    error, response = case['expected_error'], case['expected_response']
    if domain == 'gpu':
        return dict(calls=1, errors=int(bool(error or (response is not None and response & 255))))
    if domain == 'wifi':
        if error and not case.get('write_error'):
            return dict(bridge_errors=1)
        status = response & 255
        return dict(calls=1, accepts=int(status == 0), rejects=int(status not in (0, 5)),
                    bridge_errors=int(bool(error)))
    if domain == 'audio':
        return {} if error else dict(calls=1, ignored=int(response == 0), remote_errors=int(response & 0xffffffff == 5))
    if error:
        return dict(rejected=1)
    return dict(calls=1, fixture=1, rejected=int(response & 255 not in (0, 4)), remote=int(response & 255 == 4))


class Suite:
    def __init__(self, args):
        self.args, self.records, self.modules, self.baselines, self.expected = args, [], {}, {}, {}
        self.raw = None
        self.inputs = {str(Path(__file__).resolve()): sha(__file__),
                       str(args.spec): sha(args.spec), str(ROOT / 'sources.json'): sha(ROOT / 'sources.json')}
        self.result = {'schema': 'giulia-hotload-protocol-host-v1', 'scope': 'actual-protocol-host-simulation',
            'status': 'blocked', 'steps': self.records, 'device_accessed': False,
            'trusted_runtime_exercised': False, 'native_arm64_executed': False,
            'hardware_status': 'blocked', 'hardware': {name: {'status': 'blocked',
                'reason': 'Host META/requests are simulated; real provider, firmware, IRQ/DMA and device execution are unverified'}
                for name in DOMAINS}}

    def log(self, value):
        if self.raw:
            self.raw.write(json.dumps(value, sort_keys=True) + '\n')
            self.raw.flush()

    def flush(self):
        statuses = [row['status'] for row in self.records]
        self.result['status'] = ('fail' if 'fail' in statuses else 'blocked'
            if not statuses or 'blocked' in statuses else 'pass')
        temporary = self.args.out / 'result.json.tmp'
        temporary.write_text(json.dumps(self.result, indent=2) + '\n', encoding='utf-8')
        temporary.replace(self.args.out / 'result.json')

    def case(self, name, run, domain=None):
        print('@BEGIN', name, flush=True)
        path = self.args.out / (name + '.log')
        with path.open('x', encoding='utf-8') as raw:
            self.raw = raw
            try:
                evidence = run()
                row = {'id': name, 'status': 'pass', 'domain': domain, **(evidence or {})}
            except Exception as error:
                row = {'id': name, 'status': 'fail', 'domain': domain,
                       'reason': str(error), 'error_type': type(error).__name__}
            self.log({'type': 'case-result', **row})
            self.raw = None
        row['raw_log'] = str(path)
        self.records.append(row)
        self.flush()
        print('@RESULT', json.dumps({key: row[key] for key in ('id','domain','status','raw_log') if key in row}, sort_keys=True), flush=True)

    def process(self, name, command):
        path = self.args.out / (name + '-compiler.log')
        self.log({'type': 'compiler-command', 'argv': command, 'raw_log': str(path)})
        with path.open('xb') as raw:
            raw.write(('ARGV ' + json.dumps(command) + '\n').encode()); raw.flush()
            result = subprocess.run(command, stdout=raw, stderr=subprocess.STDOUT, timeout=self.args.timeout)
        check(result.returncode == 0, 'Actual host compiler failed; inspect ' + str(path))

    def build(self, domain, variant, declaration):
        source = ROOT / SOURCES[domain]
        directory = self.args.out / 'libraries' / domain / variant
        directory.mkdir(parents=True)
        flags = ['-std=c11', '-O1', '-fPIC', '-Wall', '-Wextra', '-Werror',
                 '-I', str(self.args.kit), '-I', str(ROOT / 'ios16'),
                 '-I', str(ROOT / 'ios16/common'), '-I', str(source.parent),
                 *['-D' + name + '=' + value for name, value in declaration['defines'].items()]]
        before_file, after_file = directory / 'module-before.d', directory / 'module.d'
        preflight = [self.args.cc, *flags, '-MM', '-MT', 'protocol-module', '-MF', str(before_file), str(source)]
        self.process(domain + '-' + variant + '-preflight', preflight)
        def dependencies(path):
            text = path.read_text().replace('\\\n', '').replace('$$', '$')
            check(text.startswith('protocol-module:'), 'Compiler dependency target mismatch')
            return {Path(value).resolve() for value in shlex.split(text[len('protocol-module:'):])}
        paths = dependencies(before_file)
        check(source.resolve() in paths, 'Compiler dependency closure omitted actual module source')
        hashes = {str(path): sha(path) for path in sorted(paths)}
        library = directory / 'module.so'
        command = [self.args.cc, *flags, '-shared', '-MMD', '-MT', 'protocol-module',
                   '-MF', str(after_file), str(source), '-o', str(library)]
        self.process(domain + '-' + variant + '-build', command)
        check(paths == dependencies(after_file) and hashes == {str(path): sha(path) for path in sorted(paths)},
              'Protocol source/header changed across actual host compilation')
        for path, value in hashes.items():
            check(path not in self.inputs or self.inputs[path] == value, 'Protocol source changed between variants')
            self.inputs[path] = value
        self.modules[domain, variant] = Module(self, domain, variant, library)
        record = {'source': str(source), 'defines': declaration['defines'], 'compile_command': command,
                  'dependency_command': preflight, 'source_inputs': hashes,
                  'source_inputs_sha256': json_sha(hashes), 'shared_library_sha256': sha(library),
                  'state_bytes': self.modules[domain, variant].state_bytes,
                  'compiler_identity': self.result['compiler_identity']}
        (directory / 'build.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        return record

    def begin_domain(self, domain):
        module = self.modules[domain, 'baseline']
        before, callbacks = bytes(module.state), module.gateway_calls
        check(module.invoke(1)[0] == 0 and bytes(module.state) == before, 'PROBE mutated private state or failed')
        check(module.gateway_calls == callbacks, 'PROBE called external platform')
        check(module.invoke(4, 0x4748000000000001)[0] != 0, 'Stopped instance accepted a request')
        check(module.invoke(2)[0] == 0, 'Normal START failed')
        check(module.transfer(5)[0] != 0, 'Running state was exported')
        self.expected[domain] = dict(imports=0, **({'starts': 1, 'stops': 1} if domain == 'wifi' else {}))
        return {'probe_side_effects': 0, 'stopped_call_rejected': True, 'running_export_rejected': True}

    def golden(self, domain, case):
        module = self.modules[domain, 'baseline']
        evidence = module.request(case)
        for field, delta in deltas(domain, case).items():
            self.expected[domain][field] = self.expected[domain].get(field, 0) + delta
        return {'oracle': 'independent-handwritten-wire-golden', **evidence}

    def telemetry(self, module, expected, version):
        if module.domain == 'gpu':
            return {'counter_source': 'documented-export-snapshot; no scalar telemetry ABI'}
        fields = {'wifi': ('calls', 'accepts', 'rejects', 'imports', 'starts', 'stops', 'bridge_errors'),
                  'audio': ('calls', 'imports', 'ignored', 'remote_errors', 'version'),
                  'modem': ('calls', 'native', 'fixture', 'rejected', 'remote', 'imports')}[module.domain]
        actual = {}
        for index, field in enumerate(fields):
            error, value = module.invoke(4, index)
            check(error == 0 and value == (version if field == 'version' else expected.get(field, 0)),
                  'Scalar telemetry differs from independently counted cases: ' + field)
            actual[field] = value
        return {'telemetry': actual, 'simulated_native_inputs': 0}

    def migration(self, domain):
        old = self.modules[domain, 'baseline']
        expected = self.expected[domain]
        # Wi-Fi STOP has not run yet; telemetry reports its current count.
        telemetry_expected = dict(expected)
        if domain == 'wifi': telemetry_expected['stops'] = 0
        self.telemetry(old, telemetry_expected, 1)
        payload, before = old.stopped_snapshot()
        check(all(before[field] == expected.get(field, 0) for field in before),
              'Serialized counters differ from independent per-vector accounting')
        check(before['calls'] > 0, 'Migration fixture did not establish nonzero state')
        new = self.modules[domain, 'upgrade']
        check(new.invoke(1)[0] == 0, 'Upgrade PROBE failed')
        untouched = bytes(new.state)
        check(new.transfer(6, payload, byte_delta=-1)[0] == 22, 'Short IMPORT snapshot accepted')
        check(bytes(new.state) == untouched, 'Rejected IMPORT modified private state')
        check(new.transfer(6, payload, abi=2)[0] == 22, 'Wrong state ABI IMPORT accepted')
        check(new.transfer(6, payload, reserved=1)[0] == 22, 'Reserved transfer bits accepted')
        check(new.transfer(6, payload)[0] == 0, 'Valid upgrade IMPORT failed')
        check(new.invoke(2)[0] == 0, 'Upgrade START failed')
        migrated = dict(before); migrated['imports'] += 1
        if domain == 'wifi': migrated['starts'] += 1
        self.telemetry(new, migrated, 2)
        primary = GOLDENS[domain]()[0]
        new.request(primary)
        for field, delta in deltas(domain, primary).items():
            migrated[field] = migrated.get(field, 0) + delta
        if domain == 'wifi': migrated['stops'] += 1
        final_payload, after = new.stopped_snapshot()
        check(after == {key: migrated.get(key, 0) for key in after}, 'Upgrade lost or reset migrated state')
        self.baselines[domain] = payload
        return {'before': before, 'after': after, 'private_snapshot_sha256': hashlib.sha256(payload).hexdigest(),
                'upgrade_snapshot_sha256': hashlib.sha256(final_payload).hexdigest(),
                'malformed_transfer_rejected': True, 'hardware_objects_transferred': False}

    def failure(self, domain, phase):
        module = self.modules[domain, 'fail-' + phase]
        check(module.invoke(1)[0] == 0, 'Fault variant PROBE failed before intended phase')
        if phase == 'start':
            check(module.invoke(2)[0] == 5, 'START fault macro did not produce its actual failure')
            check(module.invoke(3)[0] == 0, 'Partial START could not be stopped')
            check(module.invoke(4, 0x4748000000000001)[0] != 0, 'Cleanup did not close module requests')
        elif phase == 'import':
            before = bytes(module.state)
            check(module.transfer(6, self.baselines[domain])[0] == 5,
                  'IMPORT fault macro did not reject a valid nonzero snapshot')
            check(before == bytes(module.state), 'Failed IMPORT modified private state')
        else:
            check(module.invoke(2)[0] == 0, 'STOP-fault variant could not START')
            module.request(GOLDENS[domain]()[0])
            expected = 16 if domain == 'wifi' else 5
            check(module.invoke(3)[0] == expected, 'STOP fault macro did not produce documented failure')
        return {'fault_macro_effective': True, 'phase': phase,
                'framework_rollback_or_quarantine_exercised': False,
                'scope': 'actual-module-return-and-private-state-only'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kit', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--spec', type=Path, default=ROOT / 'hardware/hotload-protocols.json')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    args.kit, args.out, args.spec = args.kit.resolve(), args.out.resolve(), args.spec.resolve()
    if not 0 < args.timeout <= 300: parser.error('Timeout must be in 0..300 seconds')
    if not (args.kit / 'native_hotload.h').is_file(): parser.error('NativeHotload public header is missing')
    args.out.mkdir(parents=True, exist_ok=False)
    suite = Suite(args)
    compiler = shutil.which(args.cc)
    if os.name != 'posix' or not compiler or C.sizeof(C.c_void_p) != 8:
        suite.records.append({'id': 'host-toolchain', 'status': 'blocked',
            'reason': 'Linux/WSL and a 64-bit host C compiler with shared-library support are required'})
        suite.flush(); print(json.dumps(suite.result, indent=2)); return 2
    args.cc = compiler
    suite.result['compiler_identity'] = subprocess.check_output([compiler, '--version'], text=True).strip()
    spec = json.loads(args.spec.read_text(encoding='utf-8'))
    modules = {item['driver']: item for item in spec['modules']}
    check(set(modules) == set(DOMAINS), 'Specification must contain all four actual module sources')
    declarations = {item['variant']: item for item in spec['variants']}
    check(set(declarations) == set(VARIANTS), 'Specification must contain all five actual variants')
    for domain in DOMAINS:
        check(modules[domain]['source'] == SOURCES[domain] and modules[domain]['domain'] == DOMAINS[domain],
              'Protocol specification source/domain drift')
        for variant in VARIANTS:
            definitions = declarations[variant]['defines']
            for prefix in ('GH', 'GG_PROTO', 'GWPR', 'NLH'):
                for phase in ('START', 'IMPORT', 'STOP'):
                    check(definitions[prefix + '_FAIL_' + phase] == str(int(variant == 'fail-' + phase.lower())),
                          'Failure macro alias did not match its variant')
            suite.case(domain + '-' + variant + '-host-build',
                lambda d=domain, v=variant: suite.build(d, v, declarations[v]), domain)
    for domain in DOMAINS:
        if (domain, 'baseline') not in suite.modules:
            suite.records.append({'id': domain + '-logic', 'domain': domain, 'status': 'blocked',
                                  'reason': 'Actual baseline module did not compile'})
            continue
        suite.case(domain + '-probe-start-and-running-boundary', lambda d=domain: suite.begin_domain(d), domain)
        for case in GOLDENS[domain]():
            suite.case(domain + '-' + case['id'], lambda d=domain, c=case: suite.golden(d, c), domain)
        meta_bad = vector('wrong-meta-domain', GOLDENS[domain]()[0]['request'], None, error=22, meta={0: 99})
        suite.case(domain + '-wrong-meta-domain', lambda d=domain, c=meta_bad: suite.golden(d, c), domain)
        rejected_write = dict(GOLDENS[domain]()[0], id='write-denied', expected_error=13, write_error=13)
        suite.case(domain + '-response-staging-error', lambda d=domain, c=rejected_write: suite.golden(d, c), domain)
        if (domain, 'upgrade') in suite.modules:
            suite.case(domain + '-nonzero-state-migration-and-telemetry', lambda d=domain: suite.migration(d), domain)
        else:
            suite.records.append({'id': domain + '-nonzero-state-migration-and-telemetry',
                'domain': domain, 'status': 'blocked', 'reason': 'Actual upgrade module did not compile'})
        for phase in ('start', 'import'):
            if (domain, 'fail-' + phase) in suite.modules and (phase != 'import' or domain in suite.baselines):
                suite.case(domain + '-actual-fail-' + phase, lambda d=domain, p=phase: suite.failure(d, p), domain)
            else:
                suite.records.append({'id': domain + '-actual-fail-' + phase, 'domain': domain,
                    'status': 'blocked', 'reason': 'Actual fault variant or validated nonzero snapshot is missing'})
    # All deliberately uncertain STOP variants are last. No attempt is made to
    # restart/force-unload these states or to claim runtime/hardware isolation.
    for domain in DOMAINS:
        if (domain, 'fail-stop') in suite.modules:
            suite.case(domain + '-actual-fail-stop', lambda d=domain: suite.failure(d, 'stop'), domain)
        else:
            suite.records.append({'id': domain + '-actual-fail-stop', 'domain': domain,
                'status': 'blocked', 'reason': 'Actual STOP-fault variant did not compile'})
    def frozen():
        after = {path: sha(path) for path in suite.inputs}
        check(after == suite.inputs, 'Actual protocol dependencies changed during the complete suite')
        check(subprocess.check_output([compiler, '--version'], text=True).strip() == suite.result['compiler_identity'],
              'Host compiler identity changed during the suite')
        suite.result['source_inputs'] = after
        suite.result['source_inputs_sha256'] = json_sha(after)
        return {'source_files': len(after), 'source_inputs_sha256': json_sha(after)}
    suite.case('whole-suite-source-and-toolchain-freeze', frozen)
    suite.flush()
    print(json.dumps({'status':suite.result['status'], 'steps':len(suite.records),
                      'result':str(args.out/'result.json'), 'hardware_status':'blocked'},sort_keys=True))
    return {'pass': 0, 'fail': 1, 'blocked': 2}[suite.result['status']]


if __name__ == '__main__':
    raise SystemExit(main())
