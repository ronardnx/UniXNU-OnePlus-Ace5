"""Composition root for optional drivers. Leaf builders never inspect peers."""
import importlib
import json
import shutil
import difflib
from pathlib import Path
from ios16_module_registry import consolidate_modules
from ios16_hotload import SOURCE_FILES, GATEWAY_MODULES, device_sources, source_digest
from ios16_sources import kernel_source

DRIVERS = ('wifi', 'gpu', 'audio', 'modem')


def emit_hotload_module(out, source, *, root=None, selected=()):
    """Common implementation stays in UniXNU; giulia owns CDC composition."""
    import hashlib
    import runpy
    source = source.resolve()
    root = Path(root) if root else Path(__file__).resolve().parents[1]
    selected = sorted(set(selected))
    if set(selected) - set(DRIVERS):
        raise ValueError('Unknown hotload driver selection')
    required = SOURCE_FILES
    for name in required:
        if not (source / name).is_file():
            raise ValueError('Incomplete NativeHotload kit: ' + name)
    tools = out / 'native-hotload'
    shutil.copytree(source, tools, ignore=shutil.ignore_patterns('__pycache__', 'out', '.cache'))
    # Emit and hash the same frozen copy, never a concurrently edited original.
    hashes = {name: hashlib.sha256((tools/name).read_bytes()).hexdigest() for name in required}
    if any(hashlib.sha256((source/name).read_bytes()).hexdigest() != digest
           for name, digest in hashes.items()):
        raise ValueError('Common kit changed during snapshot construction')
    result = runpy.run_path(str(tools / 'emit_patch.py'))['emit'](out)
    manifest = {'schema': 2, 'abi': 2, 'patch': result.name,
                'files': hashes,
                'max_modules': 16, 'dependency_calls': True, 'state_transfer': True,
                'built_in_drivers_reloadable': False, 'xnu_core_converted': False,
                'core_policies_converted': True, 'core_interface_abi': 0x434f0001,
                'core_state_abi': 1, 'resident_core_objects': True,
                'code_memory': 'private-static-submap', 'memory_diagnostics': True,
                'core_module_names': ['xnu-sched', 'xnu-vm', 'xnu-vfs'],
                'core_policy_adapters': ['sched_quantum', 'vm_sequential',
                                         'vfs_max_io', 'vfs_prefetch'],
                'hardware_tested': False}
    manifest.update(emit_hotload_bridge(out, root, selected, manifest['files']))
    (out / 'hotload-module.json').write_text(json.dumps(manifest, indent=2) + '\n',
                                             encoding='utf-8', newline='\n')
    return manifest


def emit_hotload_bridge(out, root, selected, kit_files):
    """One resident bridge; never let a loaded module install a kernel entry."""
    header = kernel_source((root / 'ios16/common/giulia_hotload.h').read_text(encoding='utf-8'))
    enabled = '#define GIULIA_HOTLOAD_ENABLED 1\n' + header
    patch = ''
    # Optional drivers share a single disabled header in the platform patch.
    # The hotload patch enables that exact contract only for this profile.
    patch += ''.join(difflib.unified_diff(header.splitlines(True) if selected else [],
        enabled.splitlines(True), fromfile='a/iokit/Kernel/arm/giulia_hotload.h' if selected else '/dev/null',
        tofile='b/iokit/Kernel/arm/giulia_hotload.h'))
    patch += ''.join(difflib.unified_diff([], enabled.splitlines(True),
        fromfile='/dev/null', tofile='b/bsd/dev/giulia_hotload.h'))
    device_files = device_sources(root, selected)
    binding = source_digest({'kit': kit_files, 'device': device_files, 'selected': selected})
    bridge = (f'#define GIULIA_HOTLOAD_SOURCE_ID "{binding}"\n' +
              kernel_source((root / 'ios16/terminal/giulia_hotload_bridge.c').read_text(encoding='utf-8')))
    patch += ''.join(difflib.unified_diff([], bridge.splitlines(True),
        fromfile='/dev/null', tofile='b/bsd/dev/giulia_hotload_bridge.c'))
    patch += ('--- a/bsd/conf/files\n+++ b/bsd/conf/files\n'
              '@@ -9,2 +9,3 @@\n bsd/dev/native_hotload.c standard\n'
              '+bsd/dev/giulia_hotload_bridge.c standard\n'
              ' OPTIONS/mach_assert\t\t\toptional mach_assert\n')
    target = out / '0093-giulia-hotload-bridge.patch'
    if target.exists():
        raise ValueError('Hotload bridge patch already exists')
    target.write_text(patch, encoding='utf-8', newline='\n')
    return {'platform_abi': 1, 'gateway_abi': 1, 'gateway_interface_abi': 0x47480001,
            'gateway_state_abi': 1, 'gateway_patch': target.name,
            'gateway_selected': selected, 'gateway_source_sha256': binding,
            'gateway_driver_callers': {name: '_giulia_' + name + '_hotload_decide' for name in selected},
            'device_files': device_files,
            'gateway_module_names': {name: GATEWAY_MODULES[name] for name in selected},
            'hardware_driver_conversion': 'protocol-only; resident resources; device validation required'}


def emit_input_modules(out, root, *, touch=False):
    """Wire physical input workers once; no input driver knows its peers."""
    selected = (['touch'] if touch else []) + ['keys', 'slider']
    for name in selected:
        builder = importlib.import_module('ios16_' + name)
        getattr(builder, name + '_patch')(out, root)
    files = ['bsd/dev/giulia_' + name + '.c\t\t\tstandard\n' for name in selected]
    starts = ['\textern void giulia_' + name + '_start(void);\n\tgiulia_' + name + '_start();\n'
              for name in selected]
    patch = ('--- a/bsd/conf/files\n+++ b/bsd/conf/files\n'
             f'@@ -109,4 +109,{4 + len(files)} @@\n'
             ' bsd/dev/vermeer_usb_debug.c\t\tstandard\n'
             ' bsd/dev/sweet_sample.c\t\t\tstandard\n'
             + ''.join('+' + line for line in files)
             + ' bsd/dev/memdev.c\t\t\tstandard\n bsd/dev/mem.c\t\t\t\tstandard\n'
             '--- a/bsd/kern/bsd_init.c\n+++ b/bsd/kern/bsd_init.c\n'
             f'@@ -1106,5 +1106,{5 + 2 * len(starts)} @@\n'
             ' \tvermeer_usb_debug_start();\n \textern void sweet_sample_start(void);\n'
             ' \tsweet_sample_start();\n'
             + ''.join('+' + line for start in starts for line in start.splitlines(True))
             + ' #endif\n \n')
    (out / '0091-giulia-input-modules.patch').write_text(patch, encoding='utf-8', newline='\n')
    registry = {'schema': 1, 'modules': selected,
                'translation_units': [line.split()[0] for line in files],
                'registration_patch': '0091-giulia-input-modules.patch'}
    (out / 'input-module-registry.json').write_text(json.dumps(registry, indent=2) + '\n',
                                                   encoding='utf-8', newline='\n')
    return registry


def emit_optional_modules(out, root, selected):
    selected = set(selected)
    if selected - set(DRIVERS):
        raise ValueError('Unknown drivers: ' + ', '.join(sorted(selected - set(DRIVERS))))
    if not selected:
        return None
    for name in DRIVERS:
        if name in selected:
            builder = importlib.import_module('ios16_' + name + '_patch')
            getattr(builder, name + '_patch')(out, root)
    if 'gpu' in selected:
        for name in ('0085-giulia-gunyah-irq.patch', '0086-giulia-gpu-pmap.patch'):
            shutil.copyfile(root / 'ios16' / name, out / name)
    registry = consolidate_modules(out)
    registry['modules'] = ['common', 'platform', *[name for name in DRIVERS if name in selected]]
    (out / 'module-registry.json').write_text(json.dumps(registry, indent=2) + '\n',
                                             encoding='utf-8', newline='\n')
    return registry
