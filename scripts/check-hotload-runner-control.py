"""Host-only control-flow fixtures for the real hotload acceptance runner.

python3 scripts/check-hotload-runner-control.py --out NEW [--kit KIT]
Only open_session is replaced; the actual device() controller validates plans
and chooses commands. All boot/image/provider responses are explicit simulation
data. No image is built, no hardware is accessed, and no port argument exists.
"""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def fake_digest(label):
    return hashlib.sha256(('SIMULATION ONLY: ' + label).encode()).hexdigest()


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def mutating_command(command):
    return command['method'] in ('load_bytes', 'call', 'unload') or (
        command['method'] == 'diagnostic' and command['command'].endswith('.proto'))


class FakeClient:
    def __init__(self, scenario, capture):
        self.scenario = scenario
        self.wire = SimpleNamespace(log=capture)
        self.active, self.generation, self.counts = {}, 0, {}
        self.triggered = False

    def emit(self, method, **details):
        command = {'simulation': True, 'device_accessed': False, 'method': method, **details}
        self.scenario.commands.append(command)
        self.wire.log.write(('SIMULATED_COMMAND ' + json.dumps(command, sort_keys=True) + '\n').encode())
        self.wire.log.flush()

    def response(self, method, normal):
        self.counts[method] = self.counts.get(method, 0) + 1
        trigger = self.scenario.trigger
        if (trigger and not self.triggered and trigger['method'] == method and
                self.counts[method] == trigger.get('occurrence', 1)):
            self.triggered = True
            response = dict(trigger['response'])
            if trigger.get('raise', True):
                raise self.scenario.hotload.DeviceError('SIMULATED actual operation failure', response)
            return response
        return normal

    def status(self):
        self.emit('status')
        return {'abi': 2, 'provider': 1, 'platform_abi': 1, 'resident_dispatch': 1,
                'busy': 0, 'slots': len(self.active), 'token': self.generation,
                'error': 0, 'recovery': 0, 'simulation': True}

    def memory(self):
        self.emit('memory')
        return {'abi': 2, 'arena': 1, 'error': 0, 'simulation': True}

    def query(self, command):
        self.emit('query', command=command)
        if command == 'gateway':
            return [{'source_sha256': self.scenario.live_source, 'simulation': True}]
        if command == 'core':
            return []
        check(command == 'list', 'Runner requested an unsupported query')
        return [{'name': name, 'state': 3 if self.triggered else 1, 'active': value['version'],
                 'generation': value['generation'], 'blocked': int(self.triggered),
                 'calls': 0, 'image_cookie': 123, 'simulation': True}
                for name, value in self.active.items()]

    def load_bytes(self, data):
        module = self.scenario.by_bytes.get(bytes(data))
        check(module is not None, 'Runner substituted an unrecognized simulated image')
        name, version = module['image']['name'], module['image']['version']
        self.emit('load_bytes', name=name, version=version, sha256=module['sha256'])
        response = self.response('load_bytes', {'error': 0, 'recovery': 0, 'state': 1,
            'active': version, 'generation': self.generation + 1})
        if not response.get('error') and response.get('state') == 1:
            self.generation += 1
            self.active[name] = {'version': version, 'generation': self.generation}
        return response

    def call(self, name, value):
        self.emit('call', name=name, value=value)
        active = self.active.get(name, {'version': 1, 'generation': 1})
        return self.response('call', {'error': 0, 'recovery': 0, 'state': 1,
            'active': active['version'], 'generation': active['generation'], 'result': 10})

    def unload(self, name):
        self.emit('unload', name=name)
        response = self.response('unload', {'error': 0, 'recovery': 0,
            'state': 0, 'active': 0, 'generation': self.generation})
        if not response.get('error') and response.get('state') == 0:
            self.active.pop(name, None)
        return response

    def diagnostic(self, command):
        check(command in self.scenario.hotload.DIAGNOSTICS, 'Runner called a forbidden diagnostic')
        self.emit('diagnostic', command=command)
        self.counts['diagnostic'] = self.counts.get('diagnostic', 0) + 1
        # Deliberately attractive cached numbers must never become hardware
        # attestation or execution coverage, even when a plan requests it.
        return {'command': command, 'scope': 'synthetic' if command.endswith('.proto') else 'cached_status',
            'simulation': True, 'hardware_verified': False, 'complete': True,
            'rows': [{'domain': 3, 'native_provider': 1, 'provider': 1, 'hardware_ready': 1,
                      'calls': self.counts['diagnostic'] * 10, 'simulation': True}]}


class Scenario:
    def __init__(self, suite, name, temporary, output, steps, *, trigger=None,
                 wrong_boot=False, wrong_source=False):
        self.suite, self.runner, self.hotload = suite, suite.runner, suite.hotload
        self.name, self.temporary, self.output, self.steps = name, temporary, output, steps
        self.trigger, self.commands, self.opens = trigger, [], 0
        self.expected_source = fake_digest('gateway source identity')
        self.live_source = fake_digest('different source') if wrong_source else self.expected_source
        self.boot_identity = {key: fake_digest(key) for key in (
            'boot_image_sha256', 'kernel_sha256', 'firmware_sha256', 'payload_sha256')}
        self.boot_identity['gateway_source_sha256'] = self.expected_source
        self.verified, self.by_bytes = {}, {}
        for domain, module in suite.modules.items():
            for variant, version in (('baseline', 1), ('upgrade', 2), ('fail-stop', 2)):
                # These bytes intentionally are NOT a valid NLH executable.
                # The fixture only injects the caller's frozen-module result
                # into the controller; real artifact validation is separate.
                data = ('SIMULATION ONLY; NOT NATIVE CODE; ' + domain + '/' + variant).encode()
                path = temporary / (domain + '-' + variant + '.simulation-bytes')
                path.write_bytes(data)
                value = {'image': {'name': module['name'], 'version': version},
                    'sha256': digest(path), 'bytes': data, 'core_policy': False,
                    'protocol_policy': True, 'variant': variant if variant.startswith('fail-') else None,
                    'simulation': True, 'source': module['source']}
                self.verified[str(path.resolve())] = value
                self.by_bytes[data] = value
        for step in steps:
            if step.get('operation') == 'load':
                step['image'] = step.pop('simulated_image') + '.simulation-bytes'
        boot = {'simulation': True, 'status': 'pass', **self.boot_identity}
        if wrong_boot:
            boot['kernel_sha256'] = fake_digest('wrong simulated kernel')
        boot_path = temporary / 'SIMULATED-boot-record.json'
        boot_path.write_text(json.dumps(boot, indent=2) + '\n', encoding='utf-8')
        self.plan = {'schema': 'giulia-hotload-device-plan-v1', 'simulation': True,
            'boot_record': {'path': boot_path.name, 'sha256': digest(boot_path)}, 'steps': steps}
        plan_path = temporary / 'SIMULATED-plan.json'
        plan_path.write_text(json.dumps(self.plan, indent=2) + '\n', encoding='utf-8')
        self.args = SimpleNamespace(execute_device=True, plan=plan_path,
            port='SIMULATION_NO_DEVICE', timeout=0.1, lock_timeout=0,
            verified_modules=self.verified, device_root=ROOT)
        runner = self.runner
        class SimulationLedger(runner.Ledger):
            def flush(ledger):
                # device() marks entry to its session branch. In this fixture
                # that branch is FakeClient, so do not persist a device claim.
                if ledger.data.get('device_accessed'):
                    ledger.data['simulated_session_branch_entered'] = True
                ledger.data['simulation'] = True
                ledger.data['device_accessed'] = False
                ledger.data['coverage_is_simulated'] = True
                super().flush()

            def add(ledger, name, scope, status, **details):
                details.update(simulation=True, device_accessed=False)
                return super().add(name, 'simulated_' + scope, status, **details)
        self.ledger = SimulationLedger(output)
        self.ledger.data.update(simulation=True, device_accessed=False,
            boot_identity=self.boot_identity, gateway_source_sha256=self.expected_source,
            simulated_plan=self.plan, simulated_boot_record=boot)

    @contextmanager
    def open_session(self, port, log, timeout, lock_timeout):
        check(port == 'SIMULATION_NO_DEVICE', 'Unexpected port value in simulation')
        self.opens += 1
        with Path(log).open('xb') as capture:
            client = FakeClient(self, capture)
            yield client

    def run(self, *, artifact_ready=True):
        exception = None
        with patch.object(self.hotload, 'open_session', self.open_session):
            try:
                self.runner.device(self.args, self.ledger, artifact_ready)
            except Exception as error:
                exception = {'type': type(error).__name__, 'message': str(error)}
        self.ledger.flush()
        result = {'simulation': True, 'device_accessed': False, 'fake_session_opens': self.opens,
            'commands': self.commands, 'exception': exception,
            'controller_result': self.ledger.data, 'artifact_ready_argument_is_simulated': artifact_ready}
        (self.output / 'command-trace.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        return result


def load_step(domain, variant='baseline', step_id='initial-load'):
    return {'id': step_id, 'operation': 'load', 'simulated_image': domain + '-' + variant}


def status_step(step_id='cached-status'):
    return {'id': step_id, 'operation': 'diagnostic', 'command': 'audio.status'}


class Suite:
    def __init__(self, args, runner, hotload, modules, sources):
        self.args, self.runner, self.hotload, self.modules = args, runner, hotload, modules
        self.records, self.sources = [], sources
        self.result = {'schema': 'giulia-hotload-runner-control-v1', 'scope': 'host-automation-control',
            'simulation': True, 'device_accessed': False, 'hardware_verified': False,
            'hardware_status': 'blocked', 'status': 'blocked', 'cases': self.records,
            'limitation': 'Actual controller with FakeClient; boot/artifact/provider data are simulated, not device evidence'}

    def flush(self):
        statuses = [row['status'] for row in self.records]
        self.result['status'] = ('fail' if 'fail' in statuses else 'blocked'
            if not statuses or 'blocked' in statuses else 'pass')
        temporary = self.args.out / 'result.json.tmp'
        temporary.write_text(json.dumps(self.result, indent=2) + '\n', encoding='utf-8')
        temporary.replace(self.args.out / 'result.json')

    def case(self, name, steps, assert_result, **options):
        print('@BEGIN', name, flush=True)
        output = self.args.out / name
        output.mkdir()
        artifact_ready = options.pop('artifact_ready', True)
        try:
            with tempfile.TemporaryDirectory(prefix='simulation-', dir=output) as directory:
                temporary = Path(directory).resolve()
                check(temporary.is_relative_to(output.resolve()), 'Simulation directory escaped its output')
                scenario = Scenario(self, name, temporary, output, steps, **options)
                result = scenario.run(artifact_ready=artifact_ready)
                assert_result(result)
                row = {'id': name, 'status': 'pass', 'simulation': True, 'device_accessed': False,
                    'command_trace': str(output / 'command-trace.json'),
                    'fake_session_opens': result['fake_session_opens'], 'commands': result['commands'],
                    'controller_exception': result['exception']}
        except Exception as error:
            row = {'id': name, 'status': 'fail', 'simulation': True, 'device_accessed': False,
                   'reason': str(error), 'error_type': type(error).__name__}
        self.records.append(row)
        self.flush()
        print('@RESULT', json.dumps(row, sort_keys=True), flush=True)


def no_open(result):
    check(result['exception'] is None, 'Controller raised instead of returning a blocked precondition')
    check(result['fake_session_opens'] == 0 and not result['commands'], 'Preconditions opened a session')
    check(any(row['status'] == 'blocked' for row in result['controller_result']['steps']),
          'Failed precondition was not recorded as blocked')


def no_mutation(result):
    check(result['exception'] is None, 'Unexpected controller exception')
    check(result['fake_session_opens'] == 1, 'Source identity must be checked in one simulated session')
    check(not any(mutating_command(row) for row in result['commands']), 'Identity failure issued a mutation')
    check(any(row['id'] == 'running-source-identity' and row['status'] == 'blocked'
              for row in result['controller_result']['steps']), 'Source mismatch was not blocked')


def plan_rejected(result):
    check(result['exception'] and result['exception']['type'] == 'ValueError', 'Unsafe plan was accepted')
    check(result['fake_session_opens'] == 0 and not result['commands'], 'Invalid plan reached a session')


def uncertainty_blocked(result):
    check(result['exception'] is None, 'Unexpected controller exception')
    commands = result['commands']
    check(any(row['method'] == 'diagnostic' and row['command'] == 'audio.status' for row in commands),
          'Cached status after expected uncertainty was not retained')
    check(sum(row['method'] == 'load_bytes' for row in commands) == 1, 'Uncertainty caused another upload')
    check(sum(row['method'] == 'call' for row in commands) <= 1, 'Uncertainty caused another CALL')
    check(sum(row['method'] == 'unload' for row in commands) <= 1, 'Uncertainty caused another unload')
    check(not any(row['method'] == 'diagnostic' and row['command'].endswith('.proto') for row in commands),
          'Synthetic protocol action executed after uncertainty')
    check(any(row['id'] == 'forbidden-next' and row['status'] == 'blocked'
              for row in result['controller_result']['steps']), 'Actual uncertainty did not block the next mutation')


def known_stop_retained(result):
    check(result['exception'] is None, 'Reviewed fail-STOP plan was rejected')
    check([row['method'] for row in result['commands']].count('load_bytes') == 1 and
          [row['method'] for row in result['commands']].count('unload') == 1,
          'Fail-STOP operation was retried')
    check(result['commands'][-1].get('command') == 'audio.status', 'Read-only post-isolation status was lost')
    check(any(row['id'] == 'post-isolation' and row['status'] == 'blocked'
              for row in result['controller_result']['steps']), 'Uncertain isolation was not retained')


def cache_is_not_hardware(result):
    check(result['exception'] is None, 'Cached diagnostic controller raised')
    rows = result['controller_result']['steps']
    check(not any(mutating_command(row) for row in result['commands']), 'Cached queries mutated the device')
    check(result['controller_result'].get('device_domains', []) == [], 'Cached fields granted a hardware domain')
    check('real_execution' not in result['controller_result'].get('device_coverage', []),
          'Cached call numbers counted as real module execution')
    check(all(row['status'] == 'blocked' for row in rows if row['id'].endswith('-hardware')),
          'Simulated provider/readiness fields granted hardware pass')
    check(any(row['id'] == 'cache-after-hardware' and row['status'] == 'blocked' for row in rows),
          'Requested hardware claim did not remain blocked')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True, help='New simulation evidence directory')
    parser.add_argument('--kit', type=Path, default=ROOT.parent / 'UniXNU/kits/NativeHotload')
    parser.add_argument('--runner', type=Path, default=ROOT / 'scripts/verify-hotload-acceptance.py')
    args = parser.parse_args()
    args.kit, args.runner, args.out = args.kit.resolve(), args.runner.resolve(), args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.kit))
    import hotload
    check(Path(hotload.__file__).resolve() == args.kit / 'hotload.py', 'Unexpected host-client import')
    specification = ROOT / 'hardware/hotload-protocols.json'
    spec = json.loads(specification.read_text(encoding='utf-8'))
    modules = {row['driver']: row for row in spec['modules']}
    check(set(modules) == {'gpu', 'wifi', 'audio', 'modem'}, 'Actual specification lacks a domain')
    stop = next(row for row in spec['variants'] if row['variant'] == 'fail-stop')
    for prefix in ('GH', 'GG_PROTO', 'GWPR', 'NLH'):
        for phase in ('START', 'IMPORT', 'STOP'):
            check(stop['defines'][prefix + '_FAIL_' + phase] == ('1' if phase == 'STOP' else '0'),
                  'The real fault specification lost an explicit macro alias')
    paths = [Path(__file__).resolve(), args.runner, specification,
             args.kit / 'hotload.py', args.kit / 'nlh_format.py', args.kit / 'serial_mutex.py']
    for row in modules.values():
        source = (ROOT / row['source']).resolve()
        check(source.is_relative_to(ROOT.resolve()) and source.is_file(), 'Actual protocol source missing or unsafe')
        paths.append(source)
    sources = {str(path): digest(path) for path in paths}
    module_spec = importlib.util.spec_from_file_location('hotload_runner_control_target', args.runner)
    runner = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(runner)
    suite = Suite(args, runner, hotload, modules, sources)
    suite.case('artifact-precondition-never-opens', [load_step('gpu')], no_open, artifact_ready=False)
    suite.case('boot-identity-mismatch-never-opens', [load_step('gpu')], no_open, wrong_boot=True)
    suite.case('live-source-mismatch-never-mutates', [load_step('gpu')], no_mutation, wrong_source=True)
    forbidden = {
        'load': load_step('gpu', 'upgrade', 'forbidden-next'),
        'call': {'id': 'forbidden-next', 'operation': 'call', 'name': modules['gpu']['name'], 'value': 0},
        'unload': {'id': 'forbidden-next', 'operation': 'unload', 'name': modules['gpu']['name']},
        'audio-proto': {'id': 'forbidden-next', 'operation': 'diagnostic', 'command': 'audio.proto'},
        'modem-proto': {'id': 'forbidden-next', 'operation': 'diagnostic', 'command': 'modem.proto'},
    }
    for fault in ('unlabelled-state3', 'unlabelled-recovery'):
        for name, next_step in forbidden.items():
            if fault == 'unlabelled-state3':
                event = {'id': 'actual-uncertainty', 'operation': 'unload',
                         'name': modules['gpu']['name'], 'expect_error': 5}
                trigger = {'method': 'unload', 'response': {'error': 5, 'recovery': 0,
                           'state': 3, 'active': 1, 'generation': 1}}
            else:
                event = {'id': 'actual-uncertainty', 'operation': 'call', 'name': modules['gpu']['name'], 'value': 0}
                trigger = {'method': 'call', 'raise': False, 'response': {'error': 0,
                           'recovery': 5, 'state': 1, 'active': 1, 'generation': 1, 'result': 0}}
            steps = [load_step('gpu'), event, status_step(),
                     {'id': 'global-cache', 'operation': 'status'}, dict(next_step)]
            suite.case(fault + '-blocks-' + name, steps, uncertainty_blocked, trigger=trigger)
    for domain, module in modules.items():
        errno = 16 if domain == 'wifi' else 5
        check(runner.stop_failure_errno(module['name']) == errno, 'Runner STOP errno differs from reviewed module semantics')
        stop_step = {'id': 'actual-stop-failure', 'operation': 'unload', 'name': module['name'],
                     'expect_error': errno, 'stop_failure': True}
        trigger = {'method': 'unload', 'response': {'error': errno, 'recovery': 0,
                   'state': 3, 'active': 2, 'generation': 1}}
        suite.case(domain + '-known-stop-last-mutation',
            [load_step(domain, 'fail-stop'), dict(stop_step), status_step()], known_stop_retained, trigger=trigger)
        wrong = dict(stop_step, expect_error=5 if errno == 16 else 16)
        suite.case(domain + '-wrong-stop-errno-rejected',
            [load_step(domain, 'fail-stop'), wrong, status_step()], plan_rejected)
        suite.case(domain + '-known-stop-before-proto-rejected',
            [load_step(domain, 'fail-stop'), dict(stop_step),
             {'id': 'late-fixture', 'operation': 'diagnostic', 'command': 'audio.proto'}], plan_rejected)
    suite.case('verified-stop-variant-cannot-hide-label',
        [load_step('wifi', 'fail-stop'), {'id': 'hidden-stop', 'operation': 'unload',
         'name': modules['wifi']['name'], 'expect_error': 16}], plan_rejected)
    suite.case('forbidden-hardware-diagnostic-never-opens',
        [{'id': 'forbidden-hardware', 'operation': 'diagnostic', 'command': 'audio.start'}], plan_rejected)
    before = status_step('cache-before')
    after = {'id': 'cache-after', 'operation': 'diagnostic', 'command': 'audio.status',
        'hardware_domain': 'audio', 'coverage': ['real_execution'], 'assert': [
            {'where': {'domain': 3}, 'field': 'native_provider', 'value': 1},
            {'where': {'domain': 3}, 'field': 'hardware_ready', 'value': 1},
            {'where': {'domain': 3}, 'field': 'calls', 'op': 'gt',
             'baseline': {'step': 'cache-before', 'where': {'domain': 3}, 'field': 'calls'}}]}
    suite.case('cached-provider-and-counter-are-not-hardware', [before, after], cache_is_not_hardware)
    after_sources = {path: digest(path) for path in sources}
    freeze = {'id': 'production-source-freeze', 'status': 'pass' if after_sources == sources else 'fail',
              'simulation': True, 'device_accessed': False, 'source_inputs': after_sources}
    suite.records.append(freeze)
    suite.result['source_inputs'] = sources
    suite.flush()
    print(json.dumps(suite.result, indent=2, sort_keys=True))
    return {'pass': 0, 'fail': 1, 'blocked': 2}[suite.result['status']]


if __name__ == '__main__':
    raise SystemExit(main())
