"""Stage an auditable source-only copy of the common kit for GitHub Actions.

The canonical implementation remains in UniXNU/kits/NativeHotload. Refresh
only replaces files proven to match the previous manifest; no Git/remote writes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import os
from ios16_modules import emit_hotload_module


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--refresh', action='store_true',
                   help='Refresh an intact owned snapshot; refuse unrecorded/modified files')
    a = p.parse_args()
    previous = None
    if a.refresh:
        if not a.out.is_dir() or not a.manifest.is_file():
            p.error('--refresh requires an existing snapshot and manifest')
        previous = json.loads(a.manifest.read_text(encoding='utf-8'))
        if previous.get('canonical_source') != 'UniXNU/kits/NativeHotload':
            p.error('Not a NativeHotload-owned snapshot')
        recorded = previous.get('files', {})
        actual = {path.relative_to(a.out).as_posix() for path in a.out.rglob('*')
                  if path.is_file() and '__pycache__' not in path.parts}
        if actual != set(recorded):
            p.error('Snapshot contains missing or unrecorded files; preserve them before refresh')
        for name, digest in recorded.items():
            relative = Path(name)
            if (relative.is_absolute() or '..' in relative.parts or
                    hashlib.sha256((a.out / relative).read_bytes()).hexdigest() != digest):
                p.error('Snapshot changed since its recorded identity: ' + name)
    elif a.out.exists() or a.manifest.exists():
        p.error('Snapshot output and manifest must be new paths')
    a.out.parent.mkdir(parents=True, exist_ok=True)
    # Validate the same source set and patch generation used by prepare-ios16.
    with tempfile.TemporaryDirectory(prefix='hotload-stage-', dir=a.out.parent) as temporary:
        declaration = emit_hotload_module(Path(temporary), a.source)
        files = declaration['files']
        if previous and set(previous['files']) - set(files):
            p.error('Refresh cannot silently remove previously recorded files')
        a.out.mkdir(exist_ok=a.refresh)
        for name, digest in files.items():
            relative = Path(name)
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('Invalid kit source path: ' + name)
            source = Path(temporary) / 'native-hotload' / name
            if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                raise ValueError('Kit changed during staging: ' + name)
            target = a.out / name
            target.parent.mkdir(parents=True, exist_ok=True)
            staged = target.with_name(target.name + '.stage-hotload.tmp')
            with staged.open('xb') as stream:
                stream.write(source.read_bytes())
            os.replace(staged, target)
    report = {
        'schema': 1,
        'canonical_source': 'UniXNU/kits/NativeHotload',
        'kernel_pin': 'xnu-8796.141.3',
        'files': files,
        'abi': declaration['abi'],
        'platform_abi': declaration['platform_abi'],
        'gateway_abi': declaration['gateway_abi'],
        'core_interface_abi': declaration['core_interface_abi'],
        'core_state_abi': declaration['core_state_abi'],
        'core_module_names': declaration['core_module_names'],
        'whole_xnu_core_converted': False,
        'hardware_verified': False,
    }
    a.manifest.parent.mkdir(parents=True, exist_ok=True)
    a.manifest.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(f'{a.out}: {len(files)} source files; manifest={a.manifest}')


if __name__ == '__main__':
    main()
