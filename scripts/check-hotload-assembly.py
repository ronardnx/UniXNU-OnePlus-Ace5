"""Verify all 16 driver compositions and optionally apply the complete profile.

This is source/patch evidence, not a target kernel build or device execution.
Each external command has its own raw log; outputs must use a new directory.
"""
import argparse
import itertools
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

from ios16_modules import DRIVERS, emit_hotload_module, emit_optional_modules
from ios16_hotload import SOURCE_FILES, device_sources
import hashlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kit', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--xnu-base', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.kit, args.out = args.kit.resolve(), args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    steps = []
    report = {'schema': 'giulia-hotload-assembly-v1', 'status': 'blocked', 'reason': 'validation_in_progress', 'steps': steps,
              'target_kernel_built': False, 'hardware_verified': False}

    def save():
        (args.out / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    def command(name, argv, **kwargs):
        path = args.out / (name + '.log')
        with path.open('xb') as log:
            log.write(('ARGV ' + json.dumps([str(value) for value in argv]) + '\n').encode())
            log.flush()
            process = subprocess.run([str(value) for value in argv], stdout=log,
                                     stderr=subprocess.STDOUT, **kwargs)
        steps.append({'id': name, 'status': 'pass' if process.returncode == 0 else 'fail',
                      'exit_code': process.returncode, 'raw_log': str(path)})
        save()
        if process.returncode:
            raise ValueError('Command failed: ' + name + '; see ' + str(path))

    try:
        canonical_root, canonical_kit = root, args.kit
        before = device_sources(canonical_root, sorted(DRIVERS))
        kit_before = {name: hashlib.sha256((args.kit/name).read_bytes()).hexdigest() for name in SOURCE_FILES}
        # The four hardware chats may continue independently. Verify one
        # immutable, reviewable input set rather than mix source generations.
        root = args.out/'device-source'
        root.mkdir()
        for directory in ('scripts', 'ios16', 'hardware', 'uefi-overlay'):
            shutil.copytree(canonical_root/directory, root/directory,
                            ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        shutil.copyfile(canonical_root/'sources.json', root/'sources.json')
        kit_snapshot = args.out/'kit-source'
        shutil.copytree(args.kit, kit_snapshot,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc', 'out', '.cache'))
        args.kit = kit_snapshot
        sys.path.insert(0, str(root/'scripts'))
        frozen = device_sources(root, sorted(DRIVERS))
        if before != frozen or before != device_sources(canonical_root, sorted(DRIVERS)):
            raise ValueError('Device sources changed while freezing the verification snapshot')
        if kit_before != {name: hashlib.sha256((args.kit/name).read_bytes()).hexdigest() for name in SOURCE_FILES}:
            raise ValueError('Common kit changed while freezing the verification snapshot')
        report.update(device_source=str(root), kit_source=str(args.kit),
                      device_files_sha256=frozen, kit_files_sha256=kit_before)
        command('reference-pin', ['git', '-C', args.reference.resolve(), 'rev-parse', 'HEAD'])
        if (args.out/'reference-pin.log').read_text().splitlines()[-1] != 'd68e05df683f863f054b0f9c063c54d2b1342b2a':
            raise ValueError('Unpinned reference checkout')
        command('module-boundaries', [sys.executable, root/'scripts/check-driver-modules.py'])
        for index, mask in enumerate(itertools.product((False, True), repeat=4)):
            selected = sorted(name for name, enabled in zip(DRIVERS, mask) if enabled)
            target = args.out / ('composition-' + str(index).zfill(2))
            target.mkdir()
            if 'wifi' in selected:
                shutil.copyfile(args.reference/'xnu/patches/0020-stage-bearssl.sh',
                                target/'0020-stage-bearssl.sh')
            emit_optional_modules(target, root, selected)
            declaration = emit_hotload_module(target, args.kit, root=root, selected=selected)
            bridge = (target / declaration['gateway_patch']).read_text(encoding='utf-8')
            if bridge.count('+bsd/dev/giulia_hotload_bridge.c standard') != 1:
                raise ValueError('Bridge translation unit not registered exactly once')
            if bridge.count('+#define GIULIA_HOTLOAD_ENABLED 1') != 2:
                raise ValueError('Both resident include locations must use the enabled contract')
            if declaration['gateway_selected'] != selected:
                raise ValueError('Driver selection drift')
            if selected:
                platform = (target/'0088-giulia-platform-runtime.patch').read_text(encoding='utf-8')
                if platform.count('+++ b/iokit/Kernel/arm/giulia_hotload.h') != 1:
                    raise ValueError('Shared public contract was duplicated or omitted')
            steps.append({'id': 'composition-' + str(index), 'status': 'pass',
                          'selected': selected, 'manifest': str(target/'hotload-module.json'),
                          'gateway_source_sha256': declaration['gateway_source_sha256']})
            save()
        if before != device_sources(root, sorted(DRIVERS)):
            raise ValueError('Device sources changed during composition verification')
        if kit_before != {name: hashlib.sha256((args.kit/name).read_bytes()).hexdigest() for name in SOURCE_FILES}:
            raise ValueError('Common kit changed during composition verification')
        if args.xnu_base:
            profile, work = args.out/'profile', args.out/'profile-source'
            command('profile-generation', [sys.executable, root/'scripts/prepare-ios16.py',
                    '--reference', args.reference.resolve(), '--work', work, '--out', profile,
                    '--gui', '--touch', '--wifi', '--gpu', '--audio', '--modem',
                    '--hotload-source', args.kit])
            xnu = args.out/'xnu'
            pin = '1b191cb58250d0705d8a51287127505aa4bc0789'
            command('xnu-clone', ['git', '-c', 'core.autocrlf=false', 'clone', '--quiet',
                    '--shared', '--no-checkout', '--config', 'core.autocrlf=false',
                    args.xnu_base.resolve(), xnu])
            command('xnu-pin', ['git', '-C', xnu, 'checkout', '--quiet', '--detach', pin])
            dependencies = (
                ('0019-stage-libpthread.sh', 'modem-libpthread-source', 'a6c70ca0e544347a0377babf748f831577ec24f9'),
                ('0020-stage-bearssl.sh', 'wifi-sae-bearssl', '7bea48e5e850ab4cafbe68d3765cdaba13a86d6f'),
                ('0026-stage-hfs.sh', 'modem-hfs-source', '31d5ec238dfab485e78a9a30b8b92d13e8914f03'),
            )
            for stage, directory, revision in dependencies:
                archive = args.out/(directory+'.tar')
                command(directory+'-archive', ['git', '-C', canonical_root/'.cache'/directory,
                        'archive', '--format=tar', '--output', archive, revision])
                source = args.out/('pinned-'+directory)
                source.mkdir()
                with tarfile.open(archive) as stream:
                    stream.extractall(source, filter='data')
                text = (profile/stage).read_text(encoding='utf-8')
                match = re.search(r"python3 - [^\n]+ <<'PY'\n(.*?)\nPY", text, re.S)
                if not match:
                    raise ValueError('Pinned staging entry changed: ' + stage)
                code = args.out/(directory+'-stage.py')
                code.write_text(match[1], encoding='utf-8')
                command(directory+'-stage', [sys.executable, code, source], cwd=xnu)
            for patch in sorted(profile.glob('*.patch')):
                command('apply-'+patch.stem, ['git', '-C', xnu, 'apply',
                                             '--whitespace=nowarn', patch])
            bsd = (xnu/'bsd/conf/files').read_text(encoding='utf-8')
            for unit in ('native_hotload.c', 'giulia_hotload_bridge.c'):
                if bsd.count('bsd/dev/'+unit+' standard') != 1:
                    raise ValueError('Incorrect linked translation unit registration: '+unit)
            for location in ('bsd/dev', 'iokit/Kernel/arm'):
                header = (xnu/location/'giulia_hotload.h').read_text(encoding='utf-8')
                if not header.startswith('#define GIULIA_HOTLOAD_ENABLED 1\n'):
                    raise ValueError('Generated public header disabled: ' + location)
            usb = (xnu/'bsd/dev/vermeer_usb_debug.c').read_text(encoding='utf-8')
            if ('(void)giulia_hotload_init();' not in usb or
                    usb.index('(void)giulia_hotload_init();') > usb.index('    while (!u.failed) {')):
                raise ValueError('Bridge registration must precede transport dispatch')
            steps.append({'id': 'complete-profile-applied', 'status': 'pass',
                          'patches': len(list(profile.glob('*.patch'))), 'source_tree': str(xnu)})
        else:
            steps.append({'id': 'complete-profile-applied', 'status': 'blocked',
                          'reason': 'Pinned reference and XNU base were not supplied'})
        report.pop('reason', None)
        report['status'] = 'blocked' if any(item['status']=='blocked' for item in steps) else 'pass'
        current = device_sources(canonical_root, sorted(DRIVERS))
        changed = sorted(name for name in set(before)|set(current) if before.get(name)!=current.get(name))
        report['canonical_device_changes_since_snapshot'] = changed
        report['canonical_kit_matches_snapshot'] = kit_before == {
            name: hashlib.sha256((canonical_kit/name).read_bytes()).hexdigest() for name in SOURCE_FILES}
        if changed or not report['canonical_kit_matches_snapshot']:
            report['snapshot_validation_status'] = report['status']
            report['status'] = 'blocked'
            report['reason'] = 'Frozen snapshot verified; canonical inputs changed and require a new final build'
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        report.pop('reason', None)
        report['status'], report['error'] = 'fail', str(error)
    save()
    completed = next((step for step in steps if step['id']=='complete-profile-applied'), {})
    print(json.dumps({'status':report['status'], 'result':str(args.out/'result.json'),
                      'compositions':sum(step['id'].startswith('composition-') for step in steps),
                      'patches':completed.get('patches',0), 'target_kernel_built':False,
                      'hardware_verified':False, 'error':report.get('error')},sort_keys=True))
    return 0 if report['status']=='pass' else 2 if report['status']=='blocked' else 1


if __name__ == '__main__':
    sys.exit(main())
