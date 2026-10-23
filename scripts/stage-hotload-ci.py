"""Stage an auditable source-only copy of the common kit for GitHub Actions.

The canonical implementation remains in UniXNU/kits/NativeHotload. A new
snapshot directory and manifest are required; no Git or remote writes occur.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
from ios16_modules import emit_hotload_module


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--manifest', type=Path, required=True)
    a = p.parse_args()
    if a.out.exists() or a.manifest.exists():
        p.error('Snapshot output and manifest must be new paths')
    a.out.parent.mkdir(parents=True, exist_ok=True)
    # Validate the same source set and patch generation used by prepare-ios16.
    with tempfile.TemporaryDirectory(prefix='hotload-stage-', dir=a.out.parent) as temporary:
        declaration = emit_hotload_module(Path(temporary), a.source)
        files = declaration['files']
        a.out.mkdir()
        for name, digest in files.items():
            relative = Path(name)
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('Invalid kit source path: ' + name)
            source = Path(temporary) / 'native-hotload' / name
            if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                raise ValueError('Kit changed during staging: ' + name)
            target = a.out / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
    report = {
        'schema': 1,
        'canonical_source': 'UniXNU/kits/NativeHotload',
        'kernel_pin': 'xnu-8796.141.3',
        'files': files,
        'abi': declaration['abi'],
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
