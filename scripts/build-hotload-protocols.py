"""Build real GH protocol modules and bind their complete inputs to a kernel.

No module implementation is synthesized. Missing sources produce a blocked
suite and a nonzero exit status. --verify-suite performs artifact/source checks
only, for the kernel CI job after the resident profile has been generated.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

from ios16_hotload import GATEWAY_MODULES, SOURCE_FILES, device_sources, source_digest


ROOT = Path(__file__).resolve().parents[1]
DOMAINS = {'gpu': 1, 'wifi': 2, 'audio': 3, 'modem': 4}
VARIANTS = ('baseline', 'upgrade', 'fail-start', 'fail-import', 'fail-stop')
INTERFACE = 0x47480001
MODULE_KIT_INPUTS = {'native_hotload.h', 'native_hotload_core.h',
                     'core/native_core_algorithms.h', 'module.ld',
                     'nlh_format.py', 'build-module.py'}


class Blocked(Exception):
    """A required implementation or source record is absent."""


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def relative_name(name):
    if (not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_./-]+', name)
            or PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts
            or str(PurePosixPath(name)) != name):
        raise ValueError('Unsafe or noncanonical source/artifact path: ' + str(name))
    return name


def contained(root, name):
    path = (root / relative_name(name)).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError('Source/artifact escapes its declared root: ' + name)
    return path


def hash_map(value, label):
    if not isinstance(value, dict) or not value:
        raise ValueError('Missing SHA256 map: ' + label)
    for name, sha in value.items():
        relative_name(name)
        if not isinstance(sha, str) or not re.fullmatch('[0-9a-f]{64}', sha):
            raise ValueError('Invalid SHA256 in ' + label + ': ' + name)
    return value


def definitions(variant):
    result = {'GH_MODULE_VERSION': '1' if variant == 'baseline' else '2'}
    # Preserve the reviewed module implementations' local fault switches.
    # All aliases are explicit and equal; a scene cannot silently compile a
    # successful module merely because its driver's macro prefix differs.
    for prefix in ('GH', 'GG_PROTO', 'GWPR', 'NLH'):
        for phase in ('START', 'IMPORT', 'STOP'):
            result[prefix + '_FAIL_' + phase] = str(int(variant == 'fail-' + phase.lower()))
    return result


def read_spec(path):
    spec = json.loads(path.read_text(encoding='utf-8'))
    expected = {'schema': 1, 'abi': 2, 'gateway_abi': 1,
                'interface_abi': '0x47480001', 'state_abi': 1,
                'kind': 'service', 'dependencies': []}
    if not isinstance(spec, dict) or set(spec) != {*expected, 'modules', 'variants'}:
        raise ValueError('Unsupported GH protocol specification shape')
    if any(type(spec.get(key)) is not type(value) or spec[key] != value
           for key, value in expected.items()):
        raise ValueError('GH protocol specification changes the frozen ABI')
    modules = {}
    if not isinstance(spec['modules'], list):
        raise ValueError('Protocol modules must be a list')
    for item in spec['modules']:
        if not isinstance(item, dict) or set(item) != {'driver', 'domain', 'name', 'source'}:
            raise ValueError('Invalid protocol module specification')
        driver = item['driver']
        if (driver not in DOMAINS or driver in modules or type(item['domain']) is not int
                or item['domain'] != DOMAINS[driver] or item['name'] != GATEWAY_MODULES[driver]):
            raise ValueError('Protocol domain/name differs from the resident contract')
        source = relative_name(item['source'])
        if not source.startswith('ios16/') or not source.endswith('.c'):
            raise ValueError('Protocol source must be an explicit resident-recorded ios16 .c file')
        modules[driver] = item
    if set(modules) != set(DOMAINS):
        raise ValueError('Specification must name each of the four protocol domains once')
    expected_variants = [{'variant': variant, 'version': 1 if variant == 'baseline' else 2,
                          'defines': definitions(variant)} for variant in VARIANTS]
    if spec['variants'] != expected_variants:
        raise ValueError('Specification must retain all five exact GH scenario definitions')
    return spec, modules


def load_context(a):
    spec, modules = read_spec(a.spec)
    selected = sorted(a.domains)
    if len(selected) != len(set(selected)):
        raise ValueError('Duplicate --domains entry')
    binding_path = a.resident_manifest or a.source_snapshot
    if not binding_path.is_file():
        raise Blocked('Missing source binding manifest: ' + str(binding_path))
    declaration = json.loads(binding_path.read_text(encoding='utf-8'))
    if not isinstance(declaration, dict):
        raise ValueError('Source binding manifest must be a JSON object')
    kit_files = hash_map(declaration.get('files'), 'kit files')
    if set(kit_files) != set(SOURCE_FILES):
        raise ValueError('Source binding does not record every supported kit input exactly once')
    for name, sha in kit_files.items():
        path = contained(a.kit, name)
        if not path.is_file():
            raise Blocked('Missing common-kit source: ' + name)
        if digest(path) != sha:
            raise ValueError('Common-kit source binding mismatch: ' + name)
    if a.source_snapshot and (declaration.get('schema') != 1 or
            declaration.get('canonical_source') != 'UniXNU/kits/NativeHotload'):
        raise ValueError('Not a recorded NativeHotload source snapshot')
    for driver in selected:
        name = modules[driver]['source']
        if not contained(ROOT, name).is_file():
            raise Blocked('Real protocol module source is required; no stub generated: ' + name)
    devices = device_sources(ROOT, selected)
    public = (ROOT / 'ios16/common/giulia_hotload.h').read_text(encoding='utf-8')
    for name, expected in (('GH_ABI', 1), ('GH_INTERFACE_ABI', INTERFACE),
                           ('GH_STATE_ABI', 1), ('GH_MAX_WORDS', 32)):
        found = re.findall(r'^#define\s+' + name + r'\s+(0x[0-9a-fA-F]+|[0-9]+)[uU]?\s*$', public, re.M)
        if len(found) != 1 or int(found[0], 0) != expected:
            raise ValueError('Public header differs from frozen GH specification: ' + name)
    for driver in selected:
        if modules[driver]['source'] not in devices:
            raise Blocked('Protocol source is absent from resident device_files: ' + modules[driver]['source'])
    gateway = source_digest({'kit': kit_files, 'device': devices, 'selected': selected})
    if a.resident_manifest:
        expected = {'schema': 2, 'abi': 2, 'platform_abi': 1, 'gateway_abi': 1,
                    'gateway_interface_abi': INTERFACE, 'gateway_state_abi': 1,
                    'gateway_selected': selected, 'device_files': devices,
                    'gateway_source_sha256': gateway,
                    'gateway_module_names': {name: GATEWAY_MODULES[name] for name in selected}}
        for key, value in expected.items():
            if type(declaration.get(key)) is not type(value) or declaration[key] != value:
                raise ValueError('Resident manifest/source mismatch: ' + key)
    return {'spec': spec, 'modules': modules, 'selected': selected,
            'kit_files': kit_files, 'device_files': devices, 'gateway': gateway,
            'source_binding': {'kind': 'resident-manifest' if a.resident_manifest else 'source-snapshot',
                               'sha256': digest(binding_path)},
            'spec_sha256': digest(a.spec), 'builder_sha256': digest(Path(__file__))}


def input_closure(module, source, a, context):
    raw = hash_map(module.get('source_inputs'), 'compiler source_inputs')
    if module.get('source_inputs_sha256') != source_digest(raw):
        raise ValueError('Compiler source_inputs digest mismatch')
    roots = {'kit': a.kit.resolve(), 'source': contained(ROOT, source).parent,
             'include_0': (ROOT / 'ios16').resolve(),
             'include_1': (ROOT / 'ios16/common').resolve()}
    devices, kit_inputs = {}, {}
    for name, sha in raw.items():
        label, separator, suffix = name.partition('/')
        if not separator or label not in roots:
            raise ValueError('Undeclared compiler input root: ' + name)
        path = contained(roots[label], suffix)
        if not path.is_file() or digest(path) != sha:
            raise ValueError('Compiler input changed or is missing: ' + name)
        if path.is_relative_to(a.kit.resolve()):
            canonical = path.relative_to(a.kit.resolve()).as_posix()
            if context['kit_files'].get(canonical) != sha:
                raise ValueError('Compiler dependency is not bound to kit snapshot: ' + canonical)
            kit_inputs[canonical] = sha
        elif path.is_relative_to(ROOT):
            canonical = path.relative_to(ROOT).as_posix()
            if canonical not in context['device_files']:
                raise Blocked('Compiler dependency absent from resident device_files: ' + canonical)
            if context['device_files'][canonical] != sha:
                raise ValueError('Compiler dependency differs from resident source: ' + canonical)
            devices[canonical] = sha
        else:
            raise ValueError('Compiler dependency escapes kit/device roots')
    if source not in devices or module.get('source_sha256') != context['device_files'][source]:
        raise ValueError('Compiler did not record the real protocol entry source')
    builder_inputs = hash_map(module.get('kit_inputs'), 'module kit_inputs')
    if set(builder_inputs) != MODULE_KIT_INPUTS:
        raise ValueError('Module manifest omits or adds a builder/linker input')
    for name, sha in builder_inputs.items():
        if context['kit_files'].get(name) != sha:
            raise ValueError('Module builder/linker input differs from snapshot: ' + name)
    return raw, devices, kit_inputs, builder_inputs


def image_parser(kit):
    spec = importlib.util.spec_from_file_location('_giulia_nlh_format', kit / 'nlh_format.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.parse_image


def inspect_artifact(directory, item, variant, a, context, parse_image):
    image_path = directory / (item['name'] + '.nlh')
    module_path = directory / 'module.json'
    image = image_path.read_bytes()
    parsed = parse_image(image)
    expected = {'abi': 2, 'name': item['name'], 'version': variant['version'],
                'interface_abi': INTERFACE, 'state_abi': 1, 'kind': 0, 'dependencies': []}
    if any(parsed.get(key) != value for key, value in expected.items()):
        raise ValueError('Built image differs from requested GH scenario')
    module = json.loads(module_path.read_text(encoding='utf-8'))
    if (any(module.get(key) != value for key, value in parsed.items()) or
            module.get('image_sha256') != hashlib.sha256(image).hexdigest()):
        raise ValueError('Module manifest differs from parsed image')
    command = module.get('compile_command')
    expected_defines = sorted('-D' + key + '=' + value for key, value in variant['defines'].items())
    if not isinstance(command, list) or any(not isinstance(value, str) for value in command):
        raise ValueError('Invalid module compile_command')
    scenario_defines = sorted(value for value in command if value.startswith('-D') and
                              value[2:].partition('=')[0] in variant['defines'])
    if scenario_defines != expected_defines:
        raise ValueError('Build command does not contain the exact scenario defines')
    if any(not isinstance(module.get(name), str) or not module[name].strip()
           for name in ('compiler_identity', 'linker_identity')):
        raise ValueError('Module manifest omits compiler/linker identity')
    raw, devices, kit_closure, builder = input_closure(module, item['source'], a, context)
    return {**parsed, 'driver': item['driver'], 'domain': item['domain'],
            'variant': variant['variant'], 'defines': variant['defines'],
            'source': item['source'], 'source_sha256': context['device_files'][item['source']],
            'source_inputs': raw, 'device_inputs': devices, 'compiler_kit_inputs': kit_closure,
            'kit_inputs': builder, 'image_sha256': hashlib.sha256(image).hexdigest(),
            'module_manifest_sha256': digest(module_path),
            'gateway_source_sha256': context['gateway']}


def unchanged(a, context):
    if (device_sources(ROOT, context['selected']) != context['device_files'] or
            any(digest(contained(a.kit, name)) != sha for name, sha in context['kit_files'].items()) or
            digest(a.spec) != context['spec_sha256'] or digest(Path(__file__)) != context['builder_sha256']):
        raise ValueError('Bound source changed during build/verification; discard this suite')


def build(a, context, report):
    parse_image = image_parser(a.kit)
    for driver in context['selected']:
        item = context['modules'][driver]
        for variant in context['spec']['variants']:
            target = a.out / driver / variant['variant']
            log = a.out / (driver + '-' + variant['variant'] + '.log')
            command = [sys.executable, str(a.kit / 'build-module.py'),
                       '--source', str(contained(ROOT, item['source'])), '--out', str(target),
                       '--name', item['name'], '--version', str(variant['version']),
                       '--interface-abi', hex(INTERFACE), '--state-abi', '1', '--kind', 'service',
                       '--include-dir', str(ROOT / 'ios16'), '--include-dir', str(ROOT / 'ios16/common'),
                       '--clang', a.clang, '--linker', a.linker]
            for name, value in variant['defines'].items():
                command += ['--define', name + '=' + value]
            with log.open('x', encoding='utf-8') as stream:
                stream.write(json.dumps({'command': command}) + '\n')
                stream.flush()
                subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)
            entry = inspect_artifact(target, item, variant, a, context, parse_image)
            entry.update(image=(target / (item['name'] + '.nlh')).relative_to(a.out).as_posix(),
                         module_manifest=(target / 'module.json').relative_to(a.out).as_posix(),
                         raw_log=log.relative_to(a.out).as_posix(), raw_log_sha256=digest(log))
            report['artifacts'].append(entry)
    unchanged(a, context)


def verify(a, context, report):
    suite = json.loads(a.verify_suite.read_text(encoding='utf-8'))
    expected = {'schema': 1, 'status': 'pass', 'abi': 2, 'gateway_abi': 1,
                'interface_abi': INTERFACE, 'state_abi': 1,
                'requested_domains': context['selected'], 'kit_files': context['kit_files'],
                'device_files': context['device_files'], 'gateway_source_sha256': context['gateway'],
                'spec_sha256': context['spec_sha256'], 'builder_sha256': context['builder_sha256'],
                'hardware_protocol_suite_requested': True, 'hardware_tested': False,
                'hardware_validated': False, 'expected_artifacts': len(context['selected']) * 5}
    for key, value in expected.items():
        if type(suite.get(key)) is not type(value) or suite[key] != value:
            raise ValueError('Protocol suite/resident mismatch: ' + key)
    artifacts = suite.get('artifacts')
    if not isinstance(artifacts, list) or len(artifacts) != len(context['selected']) * 5:
        raise ValueError('Protocol suite must contain exactly five images per requested domain')
    parse_image = image_parser(a.kit)
    seen = set()
    variants = {item['variant']: item for item in context['spec']['variants']}
    for entry in artifacts:
        driver, variant = entry.get('driver'), entry.get('variant')
        if (driver not in context['selected'] or variant not in variants or (driver, variant) in seen):
            raise ValueError('Duplicate/unexpected GH protocol artifact')
        seen.add((driver, variant))
        image = contained(a.verify_suite.parent, entry['image'])
        manifest = contained(a.verify_suite.parent, entry['module_manifest'])
        if (image.parent != manifest.parent or manifest.name != 'module.json' or
                image.name != context['modules'][driver]['name'] + '.nlh'):
            raise ValueError('Protocol image/manifest layout mismatch')
        actual = inspect_artifact(image.parent, context['modules'][driver], variants[variant], a, context, parse_image)
        if any(entry.get(key) != value for key, value in actual.items()):
            raise ValueError('Protocol artifact manifest/source mismatch: ' + driver + '/' + variant)
        raw_log = contained(a.verify_suite.parent, entry['raw_log'])
        if not raw_log.is_file() or digest(raw_log) != entry.get('raw_log_sha256'):
            raise ValueError('Protocol artifact lacks its raw build log')
        report['artifacts'].append({'driver': driver, 'variant': variant,
                                    'image_sha256': actual['image_sha256']})
    unchanged(a, context)
    report['verified_suite_sha256'] = digest(a.verify_suite)
    report['source_agreement'] = True


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kit', type=Path, required=True)
    p.add_argument('--out', type=Path, help='New build directory; never overwritten')
    binding = p.add_mutually_exclusive_group(required=True)
    binding.add_argument('--resident-manifest', type=Path)
    binding.add_argument('--source-snapshot', type=Path)
    p.add_argument('--spec', type=Path, default=ROOT / 'hardware/hotload-protocols.json')
    p.add_argument('--domains', nargs='+', choices=tuple(DOMAINS), default=list(DOMAINS))
    p.add_argument('--verify-suite', type=Path, help='Read-only artifact verification; no compiler invoked')
    p.add_argument('--report', type=Path, help='New verification report file')
    p.add_argument('--clang', default='clang')
    p.add_argument('--linker', default='ld.lld')
    a = p.parse_args()
    if a.verify_suite:
        if a.out or a.source_snapshot:
            p.error('--verify-suite requires --resident-manifest and does not accept --out')
        if a.report and a.report.exists():
            p.error('--report must be a new file')
    else:
        if not a.out or a.report:
            p.error('Building requires --out NEW; suite report is written inside that directory')
        if a.out.exists():
            p.error('--out must be a new directory')
        a.out.mkdir(parents=True)
    a.kit = a.kit.resolve()
    report = {'schema': 1, 'status': 'blocked', 'abi': 2, 'gateway_abi': 1,
              'interface_abi': INTERFACE, 'state_abi': 1,
              'requested_domains': sorted(a.domains), 'expected_artifacts': 5 * len(a.domains),
              'artifacts': [], 'hardware_protocol_suite_requested': True,
              'hardware_tested': False, 'hardware_validated': False,
              'kernel_pin': 'xnu-8796.141.3'}
    try:
        context = load_context(a)
        report.update({key: context[key] for key in
                       ('kit_files', 'device_files', 'source_binding', 'spec_sha256', 'builder_sha256')})
        report['gateway_source_sha256'] = context['gateway']
        (verify if a.verify_suite else build)(a, context, report)
        report['status'] = 'pass'
    except Blocked as error:
        report.update(status='blocked', reason=str(error))
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        report.update(status='fail', reason=str(error))
    target = a.report if a.verify_suite else a.out / 'protocol-suite.json'
    if target:
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('x', encoding='utf-8', newline='\n') as stream:
            stream.write(json.dumps(report, indent=2) + '\n')
    summary = {'status': report['status'], 'requested_domains': report['requested_domains'],
               'artifacts': len(report['artifacts']), 'expected_artifacts': report['expected_artifacts'],
               'report': str(target) if target else None, 'hardware_validated': False}
    if 'reason' in report:
        summary['reason'] = report['reason']
    print(json.dumps(summary, sort_keys=True))
    return 0 if report['status'] == 'pass' else 3 if report['status'] == 'blocked' else 1


if __name__ == '__main__':
    raise SystemExit(main())
