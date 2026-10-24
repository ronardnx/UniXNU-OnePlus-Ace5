"""Verify linked NLH2 runtime, resident core calls and their exact kit inputs."""
import argparse
import ast
import bisect
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

from ios16_hotload import MARKERS, SOURCE_FILES, GATEWAY_MODULES, device_sources, source_digest


CORE_MODULES = {'xnu-sched': 1, 'xnu-vm': 2, 'xnu-vfs': 3}
CORE_INTERFACE = 0x434f0001
SYMBOLS = (
    '_nlh_command', '_nlh_disconnect',
    '_nlh_core_select', '_nlh_core_status', '_nlh_memory_create',
    '_nlh_memory_address', '_nlh_memory_destroy', '_nlh_memory_enabled',
    '_nlh_memory_status', '_nlh_memory_code_map',
    '_thread_quantum_init', '_cluster_max_io_size',
    '_nlh_register_platform_v2', '_nlh_dispatch_resident',
    '_giulia_hotload_init', '_giulia_hotload_select', '_giulia_hotload_status',
)
CORE_CALLERS = ('_thread_quantum_init', '_cluster_max_io_size')
DRIVER_SYMBOLS = tuple('_giulia_' + name + '_hotload_decide' for name in GATEWAY_MODULES)
BOOT_SYMBOLS = ('_kmem_init', '_nlh_memory_boot_reserve',
                '_nlh_memory_map_init', '_kmem_suballoc')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def check_source(source, manifest_path, device_root=None):
    raw = manifest_path.read_bytes()
    manifest = json.loads(raw)
    if not isinstance(manifest, dict):
        raise ValueError('Hotload manifest must be an object')
    expected = {
        'schema': 2, 'abi': 2, 'core_interface_abi': CORE_INTERFACE,
        'core_state_abi': 1, 'core_module_names': list(CORE_MODULES),
        'core_policy_adapters': ['sched_quantum', 'vm_sequential',
                                 'vfs_max_io', 'vfs_prefetch'],
        'core_policies_converted': True, 'resident_core_objects': True,
        'code_memory': 'private-static-submap', 'memory_diagnostics': True,
        'built_in_drivers_reloadable': False, 'xnu_core_converted': False,
        'platform_abi': 1, 'gateway_abi': 1, 'gateway_interface_abi': 0x47480001,
        'gateway_state_abi': 1,
    }
    for name, value in expected.items():
        if type(manifest.get(name)) is not type(value) or manifest[name] != value:
            raise ValueError('Unsupported hotload manifest field: ' + name)
    recorded = manifest.get('files')
    if not isinstance(recorded, dict) or set(recorded) != set(SOURCE_FILES):
        raise ValueError('Hotload manifest must hash every supported kit input exactly once')
    hashes = {}
    for name in SOURCE_FILES:
        data = (source / name).read_bytes()
        hashes[name] = sha256(data)
        if recorded[name] != hashes[name]:
            raise ValueError('Hotload source/manifest SHA256 mismatch: ' + name)

    # Read contract constants without executing the staged kit's Python code.
    constants = {}
    for node in ast.parse((source / 'nlh_format.py').read_text(encoding='utf-8')).body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id in (
                        'ABI', 'CORE_INTERFACE', 'CORE_STATE_ABI', 'CORE_MODULES'):
                    constants[target.id] = ast.literal_eval(node.value)
    if constants != {'ABI': 2, 'CORE_INTERFACE': CORE_INTERFACE,
                     'CORE_STATE_ABI': 1, 'CORE_MODULES': CORE_MODULES}:
        raise ValueError('Staged module builder disagrees with the supported core contract')
    for filename, name, expected_value in (
            ('native_hotload.h', 'NLH_ABI', 2),
            ('native_hotload.h', 'NLH_PLATFORM_ABI', 1),
            ('native_hotload_core.h', 'NLH_CORE_INTERFACE', CORE_INTERFACE),
            ('native_hotload_core.h', 'NLH_CORE_STATE_ABI', 1),
            ('native_hotload_core.h', 'NLH_CORE_SCHED', 1),
            ('native_hotload_core.h', 'NLH_CORE_VM', 2),
            ('native_hotload_core.h', 'NLH_CORE_VFS', 3)):
        text = (source / filename).read_text(encoding='utf-8')
        values = re.findall(r'^#define\s+' + name + r'\s+(0x[0-9a-fA-F]+|[0-9]+)[uU]?\s*$',
                            text, re.M)
        if len(values) != 1 or int(values[0], 0) != expected_value:
            raise ValueError('Staged C header disagrees with the core contract: ' + name)
    digest_input = json.dumps(hashes, sort_keys=True, separators=(',', ':')).encode('utf-8')
    selected = manifest.get('gateway_selected')
    if (not isinstance(selected, list) or selected != sorted(set(selected)) or
            set(selected) - {'gpu', 'wifi', 'audio', 'modem'}):
        raise ValueError('Invalid gateway driver selection')
    device_root = device_root or Path(__file__).resolve().parents[1]
    resident = device_sources(device_root, selected)
    if manifest.get('device_files') != resident:
        raise ValueError('Resident driver/composition source differs from the kernel manifest')
    binding = source_digest({'kit': hashes, 'device': resident, 'selected': selected})
    if manifest.get('gateway_source_sha256') != binding:
        raise ValueError('Invalid resident gateway source binding')
    if manifest.get('gateway_module_names') != {name: GATEWAY_MODULES[name] for name in selected}:
        raise ValueError('Resident protocol module names disagree with the fixed domain contract')
    if manifest.get('gateway_driver_callers') != {
            name: '_giulia_' + name + '_hotload_decide' for name in selected}:
        raise ValueError('Missing explicit per-driver resident call attribution')
    public_header = (Path(device_root) / 'ios16/common/giulia_hotload.h').read_text(encoding='utf-8')
    for name, value in (('GH_ABI', 1), ('GH_INTERFACE_ABI', 0x47480001), ('GH_STATE_ABI', 1)):
        found = re.findall(r'^#define\s+' + name + r'\s+(0x[0-9a-fA-F]+|[0-9]+)[uU]?\s*$',
                           public_header, re.M)
        if len(found) != 1 or int(found[0], 0) != value:
            raise ValueError('Device public header ABI differs from build contract: ' + name)
    return {'manifest_sha256': sha256(raw), 'source_sha256': sha256(digest_input),
            'source_sha256_format': 'SHA256 of sorted compact JSON source_files_sha256',
            'source_files_sha256': hashes, 'abi': 2,
            'core_interface_abi': CORE_INTERFACE, 'core_state_abi': 1,
            'core_modules': CORE_MODULES, 'gateway_selected': selected,
            'gateway_source_sha256': binding, 'device_files_sha256': resident,
            'platform_abi': 1, 'gateway_abi': 1}


class MachO:
    def __init__(self, data):
        self.data, self.sections, self.symbols = data, [], {}
        self.code_symbols = []
        self.function_starts, self.data_in_code = [], []
        if len(data) < 32 or struct.unpack_from('<4I', data) != (
                0xfeedfacf, 0x100000c, 0, 2):
            raise ValueError('Expected a thin ARM64 Mach-O executable (not ARM64e)')
        count, command_bytes = struct.unpack_from('<II', data, 16)
        if count > 4096 or command_bytes > len(data) - 32:
            raise ValueError('Invalid Mach-O load command bounds')
        at, end, symtab, starts, code_data, image_base = 32, 32 + command_bytes, None, None, None, None
        for _ in range(count):
            if at + 8 > end:
                raise ValueError('Truncated Mach-O load command')
            command, size = struct.unpack_from('<II', data, at)
            if size < 8 or size % 8 or size > end - at:
                raise ValueError('Invalid Mach-O load command size')
            if command == 0x19:  # LC_SEGMENT_64
                if size < 72:
                    raise ValueError('Truncated Mach-O segment')
                segment_name = data[at + 8:at + 24].split(b'\0', 1)[0]
                va, vm, offset, length, _, protection, nsects = struct.unpack_from('<QQQQiiI', data, at + 24)
                if (size != 72 + 80 * nsects or va + vm > 2**64 or
                        offset > len(data) or length > len(data) - offset):
                    raise ValueError('Invalid Mach-O segment bounds')
                if segment_name == b'__TEXT' and offset == 0 and length >= 32:
                    if image_base is not None:
                        raise ValueError('Ambiguous Mach-O image base')
                    image_base = va
                for i in range(nsects):
                    section_at = at + 72 + 80 * i
                    name = data[section_at:section_at + 16].split(b'\0', 1)[0]
                    address, amount, file_offset = struct.unpack_from('<QQI', data, section_at + 32)
                    flags = struct.unpack_from('<I', data, section_at + 64)[0]
                    executable = bool(protection & 4 and flags & 0x80000400)
                    if executable and (protection & 7 != 5 or address & 3 or amount & 3 or
                            address < va or address - va + amount > vm or
                            file_offset != offset + address - va or
                            address - va + amount > length):
                        raise ValueError('Executable section lacks valid file-backed RX bytes')
                    self.sections.append({'name': name, 'address': address,
                                          'size': amount, 'offset': file_offset,
                                          'executable': executable})
            elif command == 2:  # LC_SYMTAB
                if size != 24 or symtab is not None:
                    raise ValueError('Invalid or duplicate Mach-O symbol table')
                symtab = struct.unpack_from('<4I', data, at + 8)
            elif command in (0x26, 0x29):  # LC_FUNCTION_STARTS, LC_DATA_IN_CODE
                if size != 16:
                    raise ValueError('Invalid Mach-O linkedit data command')
                offset, length = struct.unpack_from('<II', data, at + 8)
                if offset > len(data) or length > len(data) - offset:
                    raise ValueError('Mach-O linkedit data outside kernel')
                if command == 0x26:
                    if starts is not None:
                        raise ValueError('Duplicate LC_FUNCTION_STARTS')
                    starts = (offset, length)
                else:
                    if code_data is not None:
                        raise ValueError('Duplicate LC_DATA_IN_CODE')
                    code_data = (offset, length)
            at += size
        if at != end or symtab is None:
            raise ValueError('Missing Mach-O symbol table or inconsistent load commands')
        self._symbols(symtab)
        if starts is not None and starts[1]:
            if image_base is None:
                raise ValueError('LC_FUNCTION_STARTS has no file-backed __TEXT image base')
            self._starts(starts, image_base)
        if code_data is not None:
            offset, length = code_data
            if length % 8:
                raise ValueError('Truncated LC_DATA_IN_CODE entries')
            for pos in range(offset, offset + length, 8):
                begin, amount, _ = struct.unpack_from('<IHH', data, pos)
                if begin > len(data) or amount > len(data) - begin:
                    raise ValueError('LC_DATA_IN_CODE entry outside kernel')
                self.data_in_code.append((begin, begin + amount))

    def _symbols(self, symtab):
        offset, count, strings, string_bytes = symtab
        if (offset > len(self.data) or count > (len(self.data) - offset) // 16 or
                strings > len(self.data) or string_bytes > len(self.data) - strings):
            raise ValueError('Mach-O symbol table outside kernel')
        for i in range(count):
            index, kind, section, _, value = struct.unpack_from('<IBBHQ', self.data, offset + 16 * i)
            if index >= string_bytes:
                raise ValueError('Mach-O symbol name outside string table')
            stop = self.data.find(b'\0', strings + index, strings + string_bytes)
            if stop < 0:
                raise ValueError('Unterminated Mach-O symbol name')
            # N_STAB, N_UNDF, N_ABS and indirect symbols are not code definitions.
            if kind & 0xe0 or kind & 0x0e != 0x0e:
                continue
            if not 1 <= section <= len(self.sections):
                raise ValueError('Mach-O N_SECT symbol has an invalid section index')
            text = self.sections[section - 1]
            if (not text['executable'] or text['name'] not in (b'__text', b'__initcode') or value & 3 or
                    not text['address'] <= value or value + 4 > text['address'] + text['size']):
                continue
            name = self.data[strings + index:stop].decode('utf-8', errors='replace')
            row = {'address': value, 'section': section, 'external': bool(kind & 1)}
            # Distinct translation units may legitimately reuse a static name.
            # Retain every definition as a boundary, but require unique public
            # names only for the implementation whose identity we verify.
            self.code_symbols.append(row)
            if name not in SYMBOLS + BOOT_SYMBOLS + DRIVER_SYMBOLS:
                continue
            if name in self.symbols and self.symbols[name] != row:
                raise ValueError('Ambiguous Mach-O code definition: ' + name)
            self.symbols[name] = row

    def _starts(self, command, image_base):
        offset, length = command
        at, end, address = offset, offset + length, image_base
        while at < end:
            delta, shift = 0, 0
            while True:
                if at >= end or shift >= 64:
                    raise ValueError('Invalid LC_FUNCTION_STARTS ULEB128')
                value = self.data[at]
                at += 1
                delta |= (value & 0x7f) << shift
                if not value & 0x80:
                    break
                shift += 7
            if not delta:
                if any(self.data[at:end]):
                    raise ValueError('Nonzero bytes after LC_FUNCTION_STARTS terminator')
                return
            address += delta
            if address >= 2**64 or address & 3 or not any(
                    s['executable'] and s['address'] <= address < s['address'] + s['size']
                    for s in self.sections):
                raise ValueError('LC_FUNCTION_STARTS entry is outside executable code')
            self.function_starts.append(address)
        raise ValueError('Unterminated LC_FUNCTION_STARTS')

    def function_range(self, name):
        symbol = self.symbols[name]
        start = symbol['address']
        section = self.sections[symbol['section'] - 1]
        section_end = section['address'] + section['size']
        if self.function_starts:
            index = bisect.bisect_left(self.function_starts, start)
            if index == len(self.function_starts) or self.function_starts[index] != start:
                raise ValueError('Cannot establish function range for ' + name +
                                 ': symbol is absent from LC_FUNCTION_STARTS')
            end = (self.function_starts[index + 1]
                   if index + 1 < len(self.function_starts) else section_end)
            return start, min(end, section_end), 'LC_FUNCTION_STARTS'
        # nlist has no size field. Only the next real code definition provides
        # an upper bound; never extend an unbounded symbol to the segment end.
        following = [s['address'] for s in self.code_symbols
                     if s['section'] == symbol['section'] and s['address'] > start]
        if not following:
            raise ValueError('Cannot establish function range for ' + name +
                             ': no LC_FUNCTION_STARTS or following __text code symbol; '
                             'retain the kernel function-starts/symbol tables')
        return start, min(following), 'next defined __text symbol'

    def gateway_calls(self, target):
        calls = []
        for section in self.sections:
            if not section['executable']:
                continue
            begin = section['offset']
            for delta in range(0, section['size'], 4):
                offset = begin + delta
                if any(low < offset + 4 and offset < high for low, high in self.data_in_code):
                    continue
                word = struct.unpack_from('<I', self.data, offset)[0]
                if word & 0x7c000000 != 0x14000000:  # B imm26 or BL imm26
                    continue
                immediate = word & 0x03ffffff
                if immediate & 0x02000000:
                    immediate -= 0x04000000
                address = section['address'] + delta
                if address + immediate * 4 == target:
                    calls.append({'address': address, 'file_offset': offset,
                                  'instruction': 'BL' if word & 0x80000000 else 'B'})
        return calls


def check_adapter_integration(macho, driver_calls, selected_drivers):
    """Check linked adapters without assigning an input or execution source.

    Audio/modem fixtures and native callbacks can share the same adapter.
    An arbitrary inbound branch therefore proves integration only. A future
    production-caller extension must consume exact source-bound declarations;
    symbol spelling or a call outside the adapter is not such a declaration.
    """
    selected_drivers = tuple(selected_drivers)
    if (any(not isinstance(domain, str) or domain not in GATEWAY_MODULES
            for domain in selected_drivers) or len(set(selected_drivers)) != len(selected_drivers)):
        raise ValueError('Unknown or duplicate selected protocol domain')
    attribution, ranges = {}, {}
    for domain in selected_drivers:
        name = '_giulia_' + domain + '_hotload_decide'
        if (name not in macho.symbols or not macho.symbols[name]['external'] or
            macho.sections[macho.symbols[name]['section'] - 1]['name'] != b'__text'):
            raise ValueError('Selected driver has no permanent external protocol adapter: ' + domain)
        address = macho.symbols[name]['address']
        if any(other != name and other in macho.symbols and
               macho.symbols[other]['address'] == address for other in DRIVER_SYMBOLS):
            raise ValueError('Selected protocol adapter aliases another domain: ' + domain)
        start, end, evidence = macho.function_range(name)
        if any(start < previous_end and previous_start < end
               for previous_start, previous_end in ranges.values()):
            raise ValueError('Selected protocol adapter ranges overlap across domains: ' + domain)
        ranges[domain] = (start, end)
        selected = [call for call in driver_calls if start <= call['address'] < end]
        inbound = [call for call in macho.gateway_calls(address)
                   if not start <= call['address'] < end]
        if not selected or not inbound:
            raise ValueError('Selected protocol adapter lacks its own inbound/gateway integration: ' + domain)
        blocked = {
            'production_path': 'No exact source-bound production caller declaration was verified; inbound source is unclassified.',
            'native_input': 'Static B/BL references do not establish a native input or runtime execution.',
            'output_adoption': 'Static adapter calls do not prove that a production consumer adopts the module response.',
            'hardware_function': 'No native provider/resource/firmware epoch or complete hardware function is verified here.',
        }
        attribution[domain] = {
            'scope': 'adapter-integration-only', 'status': 'pass', 'adapter_integration': True,
            'symbol': name, 'start': start, 'end': end, 'range_evidence': evidence,
            'gateway_calls': selected, 'inbound_calls': inbound,
            'inbound_source': 'unclassified', 'production_callers': [],
            'production_path': False, 'native_input': False,
            'output_adoption': False, 'hardware_function': False,
            'evidence': {field: {'status': 'blocked', 'verified': False, 'reason': reason}
                         for field, reason in blocked.items()},
        }
    return attribution


def check_kernel(kernel, binding=None, selected_drivers=()):
    macho = MachO(kernel)
    missing = [m.decode('utf-8') for m in MARKERS if m not in kernel]
    missing += [name for name in SYMBOLS if name not in macho.symbols or
                not macho.symbols[name]['external'] or
                macho.sections[macho.symbols[name]['section']-1]['name'] != b'__text']
    # Startup code is deliberately hidden and may live in a distinct RX
    # instruction section. It is not a public, permanently callable gateway.
    missing += [name for name in BOOT_SYMBOLS if name not in macho.symbols]
    if missing:
        raise ValueError('Hotload kernel lacks linked implementation: ' + ', '.join(missing))
    if binding and ('NLH gateway source_sha256=' + binding).encode('ascii') not in kernel:
        raise ValueError('Kernel resident gateway source binding differs from this source snapshot')
    calls = macho.gateway_calls(macho.symbols['_nlh_core_select']['address'])
    if len(calls) < 4:
        raise ValueError('Expected at least four direct ARM64 B/BL references to '
                         '_nlh_core_select; found ' + str(len(calls)) +
                         '. Check all scheduler, VM and VFS adapter paths and compiler inlining/LTO.')
    callers = {}
    for name in CORE_CALLERS:
        start, end, evidence = macho.function_range(name)
        selected = [call for call in calls if start <= call['address'] < end]
        if not selected:
            raise ValueError('No direct ARM64 B/BL to _nlh_core_select within ' + name +
                             '; inspect the linked function and inlining/LTO before accepting it')
        callers[name] = {'start': start, 'end': end, 'range_evidence': evidence,
                         'gateway_calls': selected}
    bootstrap = {}
    for caller, callee in (('_kmem_init', '_nlh_memory_boot_reserve'),
                           ('_nlh_memory_map_init', '_kmem_suballoc')):
        start, end, evidence = macho.function_range(caller)
        branches = macho.gateway_calls(macho.symbols[callee]['address'])
        selected = [call for call in branches if start <= call['address'] < end]
        if not selected:
            raise ValueError('Missing linked VM bootstrap call: ' + caller + ' -> ' + callee)
        bootstrap[caller] = {'callee': callee, 'range_evidence': evidence,
                             'calls': selected}
    resident_calls = {}
    for caller, callee in (('_giulia_hotload_init', '_nlh_register_platform_v2'),
                           ('_giulia_hotload_select', '_nlh_dispatch_resident')):
        start, end, evidence = macho.function_range(caller)
        branches = macho.gateway_calls(macho.symbols[callee]['address'])
        selected = [call for call in branches if start <= call['address'] < end]
        if not selected:
            raise ValueError('Missing real resident gateway call: ' + caller + ' -> ' + callee)
        resident_calls[caller] = {'callee': callee, 'range_evidence': evidence, 'calls': selected}
    driver_calls = macho.gateway_calls(macho.symbols['_giulia_hotload_select']['address'])
    driver_attribution = check_adapter_integration(macho, driver_calls, selected_drivers)
    domain_evidence = {}
    for domain in GATEWAY_MODULES:
        adapter = driver_attribution.get(domain)
        domain_evidence[domain] = {
            'scope': 'adapter-integration-only', 'status': 'blocked',
            'selected': adapter is not None, 'adapter_integration': adapter is not None,
            'production_path': False, 'native_input': False,
            'output_adoption': False, 'hardware_function': False,
            'evidence': adapter['evidence'] if adapter else {
                field: {'status': 'blocked', 'verified': False,
                        'reason': 'Domain not selected; no production/native/adoption/hardware evidence gathered.'}
                for field in ('production_path', 'native_input', 'output_adoption', 'hardware_function')},
        }
    return {'kernel_sha256': sha256(kernel), 'kernel_format': 'ARM64 Mach-O executable',
            'linked_symbols': {name: macho.symbols[name]['address'] for name in SYMBOLS},
            'markers': [marker.decode('utf-8') for marker in MARKERS],
            'gateway': '_nlh_core_select', 'direct_gateway_calls': calls,
            'direct_gateway_call_count': len(calls), 'required_callers': callers,
            'vm_bootstrap_calls': bootstrap,
            'resident_gateway_calls': resident_calls,
            'hardware_protocol_calls': driver_calls,
            'hardware_protocol_scope': 'adapter-integration-only',
            'hardware_protocol_call_attribution': driver_attribution,
            'hardware_protocol_domain_evidence': domain_evidence,
            'hardware_protocol_conversion': 'adapter integration only; production provenance, native execution, output adoption and hardware remain unverified',
            'hardware_tested': False, 'hardware_verified': False,
            'production_path_verified': False, 'native_input_verified': False,
            'output_adoption_verified': False,
            'scope': 'linked-code-verification'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--device-source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    report = {'schema': 1, 'status': 'fail', 'verified': False, 'device_verified': False}
    try:
        report.update(check_source(args.source, args.manifest, args.device_source))
        kernel = args.kernel.read_bytes()
        report['kernel_sha256'] = sha256(kernel)
        report.update(check_kernel(kernel, report['gateway_source_sha256'], report['gateway_selected']))
        report['verified'] = True
        report['status'] = 'pass'
    except (ValueError, OSError, SyntaxError, struct.error) as error:
        report['error'] = str(error)
    rendered = json.dumps(report, indent=2) + '\n'
    if args.report:
        try:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(rendered, encoding='utf-8', newline='\n')
        except OSError as error:
            report['verified'], report['status'], report['error'] = False, 'fail', 'Cannot write report: ' + str(error)
            rendered = json.dumps(report, indent=2) + '\n'
    print(rendered, end='')
    return 0 if report['verified'] else 1


if __name__ == '__main__':
    sys.exit(main())
