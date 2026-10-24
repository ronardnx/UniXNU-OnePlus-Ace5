"""Shared source inventory and kernel contract for the resident hotload build."""
import hashlib
import json
from pathlib import Path
import re
import runpy

SOURCE_FILES = (
    'native_hotload.c', 'native_hotload.h', 'native_hotload_core.h',
    'native_hotload_memory.inc', 'native_hotload_vm.h', 'native_hotload_boot.inc',
    'emit_patch.py', 'hotload.py', 'serial_mutex.py',
    'build-module.py', 'build-core.py', 'module.ld', 'nlh_format.py',
    'core/native_core_algorithms.h', 'core/policy.c', 'core/emit_core_patch.py',
    'core/emit_vm_patch.py', 'core/LICENSE', 'core/UPSTREAM.md',
    'examples/counter.c', 'examples/client.c',
)
GATEWAY_MODULES = {
    'gpu': 'giulia-gpu-proto', 'wifi': 'giulia-wifi-proto',
    'audio': 'giulia-audio-proto', 'modem': 'giulia-modem-proto',
}
GATEWAY_MARKER = b'NLH gateway abi=1'
MARKERS = (b'NLH status abi=2 nonce=', b'NLH done token=',
           b'NLH disabled unixnu_hotload=1 development-arm64-required',
           b'GIULIA TERM: hotload=NLH2', b'NLH module slot=',
           b'NLH memory abi=2 executable=1',
           b'NLH core abi=1 domains=sched,vm,vfs resident_objects=1',
           b'NLH memory abi=2 nonce=', GATEWAY_MARKER)


def source_digest(files):
    return hashlib.sha256(json.dumps(files, sort_keys=True,
        separators=(',', ':')).encode('utf-8')).hexdigest()


def device_sources(root, selected):
    """Bind the actual resident drivers, public dependencies and composition."""
    root = Path(root)
    paths = set()
    for module in ('common', 'platform', 'terminal', 'vendor', *selected):
        paths.update(path for path in (root / 'ios16' / module).rglob('*')
                     if path.suffix in ('.h', '.c', '.cpp', '.inc'))
    paths.add(root / 'ios16/terminal/giulia_hotload_bridge.c')
    paths.add(root / 'sources.json')
    paths.add(root / 'hardware/hotload-protocols.json')
    # Shared resource/upstream/firmware records do not all carry a driver
    # prefix. Bind the complete public hardware metadata set.
    paths.update((root / 'hardware').glob('*.json'))
    for name in ('ios16_modules.py', 'ios16_sources.py', 'ios16_terminal.py',
                 'ios16_platform_patch.py', 'ios16_module_registry.py', 'ios16_hotload.py', 'prepare-ios16.py',
                 'build-hotload-protocols.py'):
        paths.add(root / 'scripts' / name)
    for module in selected:
        paths.update((root / 'scripts').glob('ios16_' + module + '*.py'))
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(paths)}


def check_hotload_kernel(kernel, expected=None, *, manifest=None, source=None, device_root=None):
    """Recheck real source and linked code; never infer selected domains from a report."""
    if any(marker not in kernel for marker in MARKERS):
        raise ValueError('Hotload requires --hotload-source and a supported development ARM64 kernel')
    identities = set(re.findall(rb'NLH gateway source_sha256=([0-9a-f]{64})\x00', kernel))
    if len(identities) != 1:
        raise ValueError('Hotload kernel requires one embedded resident source identity')
    binding = identities.pop().decode('ascii')
    if (not isinstance(expected, dict) or type(expected.get('schema')) is not int or
            expected['schema'] != 1 or expected.get('status') != 'pass' or
            expected.get('verified') is not True or not manifest or not source):
        raise ValueError('Hotload requires the exact passing schema1 linked report, source manifest and kit')
    manifest, source = Path(manifest), Path(source)
    device_root = Path(device_root) if device_root else Path(__file__).resolve().parents[1]
    verifier_path = Path(__file__).with_name('check-ios16-hotload-kernel.py')
    verifier = runpy.run_path(str(verifier_path))
    # The manifest, not a caller-supplied gateway_selected default, determines
    # every required driver and its real inbound/gateway call attribution.
    sources = verifier['check_source'](source, manifest, device_root)
    selected = sources['gateway_selected']
    if sources['gateway_source_sha256'] != binding:
        raise ValueError('Embedded gateway does not match the actual reviewed source manifest')
    for key, value in sources.items():
        if type(expected.get(key)) is not type(value) or expected[key] != value:
            raise ValueError('Hotload linked report differs from real source verification: ' + key)
    linked = verifier['check_kernel'](kernel, binding, selected)
    for key, value in linked.items():
        if type(expected.get(key)) is not type(value) or expected[key] != value:
            raise ValueError('Hotload linked report differs from this exact final kernel: ' + key)
    return {'schema': 1, 'status': 'pass', 'verified': True,
            'kernel_sha256': linked['kernel_sha256'], 'gateway_source_sha256': binding,
            'gateway_selected': list(selected), 'platform_abi': 1, 'gateway_abi': 1,
            'source_manifest_sha256': sources['manifest_sha256'],
            'kit_source_sha256': sources['source_sha256'],
            'device_source_sha256': source_digest(sources['device_files_sha256']),
            'verifier_sha256': hashlib.sha256(verifier_path.read_bytes()).hexdigest(),
            'source_verification': True, 'linked_call_verification': True,
            'hardware_protocol_call_attribution': linked['hardware_protocol_call_attribution'],
            'hardware_tested': False}
