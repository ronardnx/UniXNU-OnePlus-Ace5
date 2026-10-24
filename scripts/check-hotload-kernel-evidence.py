"""Pure Python fixtures for conservative hotload linked-evidence claims.

The real check_kernel() is exercised with a simulated MachO object's symbols,
function ranges and B/BL records. No Mach-O parsing, ARM64 execution, compiler,
kernel build, serial port or hardware is exercised. --out must be a new folder.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
BINDING = 'a' * 64
CLAIMS = ('production_path', 'native_input', 'output_adoption', 'hardware_function')


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class SimulatedMachO:
    def __init__(self, verifier):
        self.sections = [{'name': b'__text', 'executable': True},
                         {'name': b'__initcode', 'executable': True}]
        names = tuple(dict.fromkeys(verifier.SYMBOLS + verifier.BOOT_SYMBOLS + verifier.DRIVER_SYMBOLS))
        self.symbols = {name: {'address': 0x1000 + index * 0x100,
            'section': 1, 'external': True} for index, name in enumerate(names)}
        self.ranges = {name: (row['address'], row['address'] + 0x80,
            'SIMULATED exact function range') for name, row in self.symbols.items()}
        self.branches, self.queries, self.fixtures = {}, [], {}
        for name in verifier.CORE_CALLERS:
            self.branch('_nlh_core_select', self.symbols[name]['address'] + 4)
            self.branch('_nlh_core_select', self.symbols[name]['address'] + 8)
        for caller, callee in (('_kmem_init', '_nlh_memory_boot_reserve'),
                ('_nlh_memory_map_init', '_kmem_suballoc'),
                ('_giulia_hotload_init', '_nlh_register_platform_v2'),
                ('_giulia_hotload_select', '_nlh_dispatch_resident')):
            self.branch(callee, self.symbols[caller]['address'] + 4)
        for index, domain in enumerate(verifier.GATEWAY_MODULES):
            adapter = '_giulia_' + domain + '_hotload_decide'
            self.branch('_giulia_hotload_select', self.symbols[adapter]['address'] + 4)
            fixture = '_synthetic_fixture_' + domain
            self.fixtures[fixture] = 0x90000 + index * 0x100
            self.branch(adapter, self.fixtures[fixture] + 4)

    def branch(self, target, address):
        self.branches.setdefault(self.symbols[target]['address'], []).append(
            {'address': address, 'file_offset': address - 0x1000, 'instruction': 'BL'})

    def gateway_calls(self, target):
        self.queries.append({'method': 'gateway_calls', 'target': target})
        return copy.deepcopy(self.branches.get(target, []))

    def function_range(self, name):
        self.queries.append({'method': 'function_range', 'symbol': name})
        return self.ranges[name]

    def description(self):
        return {'simulation': True, 'symbols': self.symbols, 'ranges': self.ranges,
                'branches': self.branches, 'only_inbound_sources': self.fixtures,
                'queries': self.queries}


class Suite:
    def __init__(self, args, verifier, helper):
        self.args, self.verifier, self.helper = args, verifier, helper
        self.cases = []
        self.kernel = b'\n'.join(verifier.MARKERS) + b'\nNLH gateway source_sha256=' + BINDING.encode() + b'\0'
        self.result = {'schema': 'hotload-kernel-evidence-fixture-v1',
            'scope': 'host-link-evidence-control', 'simulation': True,
            'device_accessed': False, 'hardware_verified': False,
            'macho_parsing_exercised': False, 'native_execution_verified': False,
            'hardware_status': 'blocked', 'status': 'blocked', 'cases': self.cases}

    def model(self):
        return SimulatedMachO(self.verifier)

    def linked(self, model, selected, kernel=None):
        with patch.object(self.verifier, 'MachO', return_value=model):
            return self.verifier.check_kernel(self.kernel if kernel is None else kernel, BINDING, selected)

    def rejected(self, model, selected, fragment, kernel=None):
        try:
            self.linked(model, selected, kernel)
        except ValueError as error:
            check(fragment in str(error), 'Rejected for an unrelated condition: ' + str(error))
            return {'expected_rejection': str(error), 'model': model.description()}
        raise AssertionError('Invalid integration was accepted')

    def flush(self):
        self.result['status'] = 'fail' if any(case['status'] == 'fail' for case in self.cases) else 'pass'
        temporary = self.args.out / 'result.json.tmp'
        temporary.write_text(json.dumps(self.result, indent=2) + '\n', encoding='utf-8')
        temporary.replace(self.args.out / 'result.json')

    def case(self, name, run):
        print('@BEGIN', name, flush=True)
        try:
            evidence = run()
            row = {'id': name, 'status': 'pass', 'simulation': True, 'device_accessed': False}
        except Exception as error:
            evidence = {'error_type': type(error).__name__, 'reason': str(error)}
            row = {'id': name, 'status': 'fail', 'simulation': True, 'device_accessed': False}
        raw = self.args.out / (name + '.log')
        raw.write_text(json.dumps({'simulation': True, 'device_accessed': False,
            'case': row, 'evidence': evidence}, indent=2) + '\n', encoding='utf-8')
        row['raw_log'] = str(raw)
        if row['status'] == 'fail': row.update(evidence)
        self.cases.append(row)
        self.flush()
        print('@RESULT', json.dumps(row, sort_keys=True), flush=True)

    def fixture_only(self):
        model = self.model()
        selected = tuple(self.verifier.GATEWAY_MODULES)
        report = self.linked(model, selected)
        check(report['hardware_protocol_scope'] == 'adapter-integration-only', 'Global scope is ambiguous')
        for domain in selected:
            adapter = report['hardware_protocol_call_attribution'][domain]
            check(adapter['scope'] == 'adapter-integration-only' and adapter['adapter_integration'] is True,
                  'Required adapter integration proof was lost')
            check(adapter['gateway_calls'] and adapter['inbound_calls'], 'Real structural evidence was discarded')
            check(adapter['production_callers'] == [] and adapter['inbound_source'] == 'unclassified',
                  'Fixture branches were relabelled as production callers')
            for claim in CLAIMS:
                check(adapter[claim] is False and adapter['evidence'][claim]['status'] == 'blocked' and
                      adapter['evidence'][claim]['verified'] is False,
                      'Only-fixture inbound granted a production/native/adoption/hardware claim')
                domain_evidence = report['hardware_protocol_domain_evidence'][domain]
                check(domain_evidence['status'] == 'blocked' and domain_evidence[claim] is False,
                      'Domain aggregate promoted integration to hardware evidence')
        check(len(report['required_callers']) == 2 and len(report['vm_bootstrap_calls']) == 2 and
              len(report['resident_gateway_calls']) == 2, 'Required runtime/core/bootstrap evidence disappeared')
        check(report['hardware_tested'] is False and report['hardware_verified'] is False,
              'Static simulated integration granted a hardware claim')
        return {'model': model.description(), 'report': report}

    def core_only(self):
        model = self.model(); report = self.linked(model, ())
        check(report['hardware_protocol_call_attribution'] == {}, 'Unselected raw calls granted domain attribution')
        for evidence in report['hardware_protocol_domain_evidence'].values():
            check(evidence['selected'] is False and evidence['adapter_integration'] is False and
                  evidence['status'] == 'blocked' and all(evidence[claim] is False for claim in CLAIMS),
                  'Unselected hardware domain was not explicitly blocked')
        return {'report': report}

    def borrow_outbound(self, domain):
        model = self.model()
        name = '_giulia_' + domain + '_hotload_decide'
        start, end, unused = model.ranges[name]
        target = model.symbols['_giulia_hotload_select']['address']
        model.branches[target] = [call for call in model.branches[target] if not start <= call['address'] < end]
        return self.rejected(model, (domain,), 'lacks its own inbound/gateway integration')

    def borrow_inbound(self, domain):
        model = self.model()
        name = '_giulia_' + domain + '_hotload_decide'
        model.branches[model.symbols[name]['address']] = []
        return self.rejected(model, (domain,), 'lacks its own inbound/gateway integration')

    def adapter_missing(self, kind):
        model = self.model(); name = '_giulia_gpu_hotload_decide'
        if kind == 'absent': model.symbols.pop(name)
        elif kind == 'nonexternal': model.symbols[name]['external'] = False
        else: model.symbols[name]['section'] = 2
        return self.rejected(model, ('gpu',), 'no permanent external protocol adapter')

    def self_only(self):
        model = self.model(); name = '_giulia_gpu_hotload_decide'
        model.branches[model.symbols[name]['address']] = [
            {'address': model.symbols[name]['address'] + 8, 'file_offset': 8, 'instruction': 'BL'}]
        return self.rejected(model, ('gpu',), 'lacks its own inbound/gateway integration')

    def aliases(self):
        model = self.model()
        gpu, audio = '_giulia_gpu_hotload_decide', '_giulia_audio_hotload_decide'
        model.symbols[audio]['address'] = model.symbols[gpu]['address']
        model.ranges[audio] = model.ranges[gpu]
        return self.rejected(model, ('gpu', 'audio'), 'aliases another domain')

    def marker_missing(self):
        return self.rejected(self.model(), ('gpu',), 'lacks linked implementation',
            self.kernel.replace(self.verifier.MARKERS[-1], b'SIMULATED missing marker', 1))

    def binding_mismatch(self):
        return self.rejected(self.model(), ('gpu',), 'source binding differs',
                             self.kernel.replace(BINDING.encode(), b'b' * 64))

    def runtime_missing(self):
        model = self.model(); model.symbols.pop('_nlh_register_platform_v2')
        return self.rejected(model, ('gpu',), 'lacks linked implementation')

    def bootstrap_missing(self):
        model = self.model()
        model.branches[model.symbols['_nlh_memory_boot_reserve']['address']] = []
        return self.rejected(model, ('gpu',), 'Missing linked VM bootstrap call')

    def pack_helper(self, stale=False, overstated=False):
        model = self.model(); selected = ['audio', 'modem']
        linked = self.linked(model, selected)
        sources = {'gateway_selected': selected, 'gateway_source_sha256': BINDING,
            'manifest_sha256': 'c' * 64, 'source_sha256': 'd' * 64,
            'device_files_sha256': {'SIMULATION_ONLY': 'e' * 64}}
        expected = {'schema': 1, 'status': 'pass', 'verified': True, **sources, **copy.deepcopy(linked)}
        if stale:
            expected['hardware_protocol_call_attribution']['audio'].pop('scope')
        if overstated:
            expected['hardware_protocol_call_attribution']['audio']['production_path'] = True
        simulated_verifier = {'check_source': lambda *unused: sources,
                              'check_kernel': lambda *unused: linked}
        with patch.object(self.helper.runpy, 'run_path', return_value=simulated_verifier):
            try:
                binding = self.helper.check_hotload_kernel(self.kernel, expected,
                    manifest=Path('SIMULATION-manifest.json'), source=Path('SIMULATION-kit'), device_root=ROOT)
            except ValueError as error:
                check((stale or overstated) and 'hardware_protocol_call_attribution' in str(error),
                      'Pack helper rejected for an unrelated reason: ' + str(error))
                return {'expected_rejection': str(error), 'source_verification_simulated': True}
        check(not (stale or overstated), 'Pack helper accepted stale or unsupported production/native claims')
        check(binding['hardware_protocol_call_attribution'] == linked['hardware_protocol_call_attribution'],
              'Pack helper changed or removed blocked evidence')
        for entry in binding['hardware_protocol_call_attribution'].values():
            check(entry['scope'] == 'adapter-integration-only' and all(entry[claim] is False for claim in CLAIMS),
                  'Packaging promoted adapter integration into native/hardware claims')
        return {'pack_binding': binding, 'source_verification_simulated': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--verifier', type=Path, default=ROOT / 'scripts/check-ios16-hotload-kernel.py')
    args = parser.parse_args()
    args.out, args.verifier = args.out.resolve(), args.verifier.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('hotload_kernel_evidence_target', args.verifier)
    verifier = importlib.util.module_from_spec(spec); spec.loader.exec_module(verifier)
    import ios16_hotload
    suite = Suite(args, verifier, ios16_hotload)
    paths = (Path(__file__).resolve(), args.verifier, ROOT / 'scripts/ios16_hotload.py')
    before = {str(path): sha(path) for path in paths}
    suite.case('fixture-only-inbound-retains-integration-blocks-native', suite.fixture_only)
    suite.case('core-only-keeps-every-hardware-domain-blocked', suite.core_only)
    for domain in verifier.GATEWAY_MODULES:
        suite.case(domain + '-cannot-borrow-another-domain-outbound', lambda d=domain: suite.borrow_outbound(d))
        suite.case(domain + '-cannot-borrow-another-domain-inbound', lambda d=domain: suite.borrow_inbound(d))
    for kind in ('absent', 'nonexternal', 'initcode'):
        suite.case('adapter-' + kind + '-rejected', lambda k=kind: suite.adapter_missing(k))
    suite.case('self-recursion-does-not-prove-integration', suite.self_only)
    suite.case('adapter-domain-alias-rejected', suite.aliases)
    suite.case('real-marker-gate-retained', suite.marker_missing)
    suite.case('embedded-source-marker-gate-retained', suite.binding_mismatch)
    suite.case('mandatory-runtime-symbol-gate-retained', suite.runtime_missing)
    suite.case('vm-bootstrap-call-gate-retained', suite.bootstrap_missing)
    suite.case('pack-helper-preserves-integration-only-scope', suite.pack_helper)
    suite.case('pack-helper-rejects-stale-overstated-report', lambda: suite.pack_helper(True))
    suite.case('pack-helper-rejects-unbacked-native-pass', lambda: suite.pack_helper(overstated=True))
    after = {str(path): sha(path) for path in paths}
    suite.cases.append({'id': 'production-source-freeze', 'status': 'pass' if before == after else 'fail',
                        'simulation': True, 'device_accessed': False})
    suite.result['source_inputs'] = after
    suite.flush()
    print(json.dumps(suite.result, indent=2, sort_keys=True))
    return 0 if suite.result['status'] == 'pass' else 1


if __name__ == '__main__':
    raise SystemExit(main())
