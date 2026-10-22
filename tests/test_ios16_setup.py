"""Exercise the optional Setup skip and its userdata assembly integration."""
import copy
import contextlib
import hashlib
import importlib.util
import io
from pathlib import Path
import plistlib
import struct
import sys
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
from ios16_boot_baseline import BASELINE, LOG_ENV
from ios16_root_layout import MIGRATION, VERSION
from ios16_setup import development_setup_properties, prepare_setup
from ios16_startup_compat import DISABLED_AUDIO, JOBS, LAUNCHD, MEDIA, SPRINGBOARD, VIRTUAL_AUDIO

spec = importlib.util.spec_from_file_location('ios16_userdata_rebuild', SCRIPTS / 'rebuild-ios16-userdata.py')
rebuild = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rebuild)

PREFERENCES = '/private/var/mobile/Library/Preferences/'
PURPLEBUDDY = PREFERENCES + 'com.apple.purplebuddy.plist'
EXPECTED = {
    PURPLEBUDDY: {
        'SetupDone': True, 'SetupFinishedAllSteps': True,
        'SetupVersion': 11, 'daemonizeNeedsToRun': False,
    },
    PREFERENCES + 'com.apple.purplebuddy.notbackedup.plist': {
        name: False for name in (
            'ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
            'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
            'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
            'SilentICDPUpgrade',
        )
    },
    PREFERENCES + 'com.apple.managedconfiguration.notbackedup.plist': {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False,
    },
    SPRINGBOARD: {'MCForceShowCloudConfigurationUI': False},
}
ACTIVE_MIGRATION = '/private/var/datamigrator/migrate'
DATA_ARK = '/private/var/containers/Data/System/activation/Library/internal/data_ark.plist'


class Root:
    def __init__(self):
        self.data = {
            VERSION: plistlib.dumps({
                'ProductBuildVersion': '20H392', 'ProductVersion': '16.7.16',
            }),
            VIRTUAL_AUDIO: b'virtual audio plugin',
            LAUNCHD: plistlib.dumps({'LaunchDaemons': {
                **{job: {'Label': 'com.apple.CommCenter', 'KeepAlive': True} for job in JOBS},
                'accountsd': {'Label': 'accountsd', 'EnvironmentVariables': {'KEEP': 'yes'}},
                'migration': {'Label': 'migration'},
            }, 'VersionNumber': 1}),
        }
        for variant in ('D22', 'D221', 'Default'):
            self.data[MEDIA.format(variant)] = plistlib.dumps({'StartWirelessDisplayServer': True})
        self.metadata = {}
        self.mutations = []

    def read(self, path):
        return self.data[path]

    def exists(self, path):
        return path in self.data

    def directory(self, path, uid):
        self.mutations.append(('directory', path, uid))
        self.data.setdefault(path, b'')

    def write(self, path, data, uid=0, mode=0o644):
        self.mutations.append(('write', path))
        self.data[path] = data
        self.metadata[path] = (uid, mode)

    def move(self, source, destination):
        self.mutations.append(('move', source, destination))
        self.data[destination] = self.data.pop(source)


class NoAccessRoot:
    def __getattr__(self, name):
        raise AssertionError(f'Disabled Setup skip accessed root.{name}')


class SetupTests(unittest.TestCase):
    def test_development_setup_identity_requires_gui(self):
        with self.assertRaisesRegex(ValueError, 'requires the GUI'):
            development_setup_properties()
        values = development_setup_properties(gui=True)
        self.assertEqual({key: struct.unpack('<I', value)[0]
                          for key, value in values.items()}, {
            'certificate-production-status': 0,
            'effective-production-status-ap': 0,
            'certificate-security-mode': 1,
            'effective-security-mode-sep': 1,
        })
        cli_spec = importlib.util.spec_from_file_location(
            'ios16_adt_cli', SCRIPTS / 'make-ios16-adt.py')
        cli = importlib.util.module_from_spec(cli_spec)
        cli_spec.loader.exec_module(cli)
        stderr = io.StringIO()
        with patch.object(sys, 'argv', [
            'make-ios16-adt.py', '--reference', 'unused', '--out', 'unused',
            '--development-setup',
        ]), contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as error:
                cli.main()
        self.assertEqual(error.exception.code, 2)
        self.assertIn('--development-setup requires --gui', stderr.getvalue())

    def test_default_and_explicit_false_do_not_access_the_root(self):
        self.assertEqual(prepare_setup(NoAccessRoot()), {'skip_setup': False})
        self.assertEqual(prepare_setup(NoAccessRoot(), enabled=False), {'skip_setup': False})

    def test_enabled_preferences_permissions_and_report(self):
        root = Root()
        before = copy.deepcopy(root.data)
        report = prepare_setup(root, enabled=True)
        self.assertTrue(report['skip_setup'])
        self.assertTrue(report['setup_completion_seeded'])
        self.assertFalse(report['activation_state_seeded'])
        self.assertFalse(report['setup_skip']['hardware_verified'])
        changes = report['setup_skip']['preference_changes']
        self.assertEqual({change['path'] for change in changes}, set(EXPECTED))
        self.assertEqual(len(changes), len(EXPECTED))
        for path, expected in EXPECTED.items():
            self.assertEqual(plistlib.loads(root.read(path)), expected)
            self.assertEqual(root.metadata[path], (501, 0o600))
            change = next(item for item in changes if item['path'] == path)
            self.assertIn('before_sha256', change)
            self.assertEqual(change['after_sha256'], hashlib.sha256(root.read(path)).hexdigest())
        for path, original in before.items():
            self.assertEqual(root.read(path), original)
        self.assertFalse(root.exists(MIGRATION))
        self.assertFalse(any('data_ark.plist' in path for path in root.data))

    def test_preserves_extra_preferences_migration_and_activation_bytes(self):
        root = Root()
        for path in EXPECTED:
            root.data[path] = plistlib.dumps({'unrelated': {'keep': 1}})
        root.data[SPRINGBOARD] = plistlib.dumps({
            'unrelated': {'keep': 1}, 'BKNoWatchdogs': True,
            'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100},
        })
        root.data[MIGRATION] = b'existing migration bytes'
        root.data[DATA_ARK] = b'existing activation bytes'
        before = copy.deepcopy(root.data)
        report = prepare_setup(root, enabled=True)
        for path, expected in EXPECTED.items():
            merged = plistlib.loads(root.read(path))
            self.assertEqual(merged, {**plistlib.loads(before[path]), **expected})
            change = next(item for item in report['setup_skip']['preference_changes'] if item['path'] == path)
            self.assertEqual(change['before_sha256'], hashlib.sha256(before[path]).hexdigest())
        self.assertEqual(root.read(MIGRATION), before[MIGRATION])
        self.assertEqual(root.read(DATA_ARK), before[DATA_ARK])
        self.assertFalse(report['activation_state_seeded'])

    def test_wrong_build_version_and_active_migration_fail_before_mutations(self):
        for case in ('build', 'version', 'active migration'):
            with self.subTest(case=case):
                root = Root()
                version = plistlib.loads(root.read(VERSION))
                if case == 'build':
                    version['ProductBuildVersion'] = '21H461'
                elif case == 'version':
                    version['ProductVersion'] = '17.7.11'
                else:
                    root.data[ACTIVE_MIGRATION] = b'active'
                root.data[VERSION] = plistlib.dumps(version)
                before = copy.deepcopy(root.data)
                with self.assertRaises(ValueError):
                    prepare_setup(root, enabled=True)
                self.assertEqual(root.data, before)
                self.assertEqual(root.mutations, [])

    def test_every_preference_is_preflighted_before_mutations(self):
        for path in EXPECTED:
            for invalid in (plistlib.dumps(['not', 'a', 'dictionary']), b'not a plist'):
                with self.subTest(path=path, invalid=invalid[:20]):
                    root = Root()
                    root.data[path] = invalid
                    before = copy.deepcopy(root.data)
                    with self.assertRaises(ValueError):
                        prepare_setup(root, enabled=True)
                    self.assertEqual(root.data, before)
                    self.assertEqual(root.mutations, [])

    def test_readback_mismatch_is_an_error(self):
        class CorruptRoot(Root):
            def read(self, path):
                if path == PURPLEBUDDY and path in self.metadata:
                    return plistlib.dumps({'unexpected': True})
                return super().read(path)

        with self.assertRaises(ValueError):
            prepare_setup(CorruptRoot(), enabled=True)

    def test_disabled_preserves_existing_setup_bytes(self):
        root = Root()
        for path in EXPECTED:
            root.data[path] = plistlib.dumps({'existing': True})
        before = copy.deepcopy(root.data)
        self.assertEqual(prepare_setup(root, enabled=False), {'skip_setup': False})
        self.assertEqual(root.data, before)
        self.assertEqual(root.mutations, [])


class RootAssemblyTests(unittest.TestCase):
    def test_startup_baseline_logging_and_skip_combinations(self):
        for startup in (False, True):
            for baseline in (False, True):
                for skip in (False, True):
                    with self.subTest(startup=startup, baseline=baseline, skip=skip):
                        root = Root()
                        report = rebuild.prepare_root(
                            root, '20H392', startup_compat=startup,
                            migration_baseline=baseline, full_boot_log=baseline,
                            skip_setup=skip,
                        )
                        self.assertEqual(report['skip_setup'], skip)
                        self.assertEqual(report.get('setup_completion_seeded', False), skip)
                        self.assertFalse(report.get('activation_state_seeded', False))
                        self.assertEqual(report['migration_completion_seeded'], baseline)
                        self.assertEqual(report['normal_migration_enabled'], not baseline)
                        self.assertEqual(root.exists(MIGRATION), baseline)
                        if baseline:
                            self.assertEqual(plistlib.loads(root.read(MIGRATION)), BASELINE)
                            self.assertEqual(root.metadata[MIGRATION], (501, 0o600))
                        self.assertEqual(root.exists(PURPLEBUDDY), skip)
                        if skip:
                            self.assertEqual(plistlib.loads(root.read(PURPLEBUDDY)), EXPECTED[PURPLEBUDDY])
                        self.assertFalse(any('data_ark.plist' in path for path in root.data))
                        self.assertEqual(root.exists(DISABLED_AUDIO + '/VirtualAudio.plugin'), startup)
                        jobs = plistlib.loads(root.read(LAUNCHD))['LaunchDaemons']
                        self.assertIn('accountsd', jobs)
                        self.assertIn('migration', jobs)
                        self.assertEqual(any(job in jobs for job in JOBS), not startup)
                        self.assertEqual(jobs['accountsd'].get('EnvironmentVariables'),
                                         {'KEEP': 'yes', **(LOG_ENV if baseline else {})})
                        if baseline:
                            self.assertEqual(jobs['migration']['EnvironmentVariables'], LOG_ENV)
                        if startup or skip:
                            prefs = plistlib.loads(root.read(SPRINGBOARD))
                            self.assertEqual(prefs.get('BKNoWatchdogs', False), startup)
                            if startup:
                                self.assertEqual(prefs['FBLaunchWatchdogExceptions'], {'com.apple.purplebuddy': 100})
                            if skip:
                                self.assertFalse(prefs['MCForceShowCloudConfigurationUI'])


if __name__ == '__main__':
    unittest.main()
