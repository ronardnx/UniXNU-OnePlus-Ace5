"""Exercise fresh-root initialization and refusal to overwrite migration state.

Run as root on Linux/macOS so numeric ownership is checked on real files.
"""
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from ios16_gui_seed import MountedRoot, PREF_PATH, VERSION_PATH, seed


@unittest.skipUnless(hasattr(os, 'geteuid') and os.geteuid() == 0,
                     'Requires root for numeric ownership checks')
class SeedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.io = MountedRoot(self.root)
        self.put(VERSION_PATH, {'ProductBuildVersion': '20H392'})
        (self.root / 'private/var/mobile').mkdir(parents=True)
        (self.root / 'var').symlink_to('private/var', target_is_directory=True)

    def put(self, path, data):
        target = self.root / path.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(plistlib.dumps(data))
        return target

    def test_clean_root_permissions_and_idempotency(self):
        unrelated = self.put('/private/var/mobile/Library/Preferences/keep.plist',
                             {'keep': 'unchanged'})
        saved = unrelated.read_bytes()
        seed(self.io, '20H392')
        pref = self.root / PREF_PATH.lstrip('/')
        stat = pref.stat()
        self.assertEqual((stat.st_uid, stat.st_gid, stat.st_mode & 0o777),
                         (501, 501, 0o600))
        bundle = (self.root / 'private/var/containers/Bundle').stat()
        self.assertEqual((bundle.st_uid, bundle.st_gid, bundle.st_mode & 0o777),
                         (33, 33, 0o755))
        first = pref.read_bytes()
        seed(self.io, '20H392')
        self.assertEqual(pref.read_bytes(), first)
        self.assertEqual(unrelated.read_bytes(), saved)
        self.assertEqual(plistlib.loads(first)['LastSystemVersion'], '20H392')
        self.assertFalse((pref.parent / 'com.apple.purplebuddy.plist').exists())

    def test_wrong_build_refused_before_writes(self):
        with self.assertRaisesRegex(ValueError, 'SystemVersion'):
            seed(self.io, '21H461')
        self.assertFalse((self.root / 'private/var/containers').exists())

    def test_active_migration_refused_before_writes(self):
        sentinel = self.root / 'private/var/datamigrator/migrate'
        sentinel.parent.mkdir()
        sentinel.touch()
        with self.assertRaisesRegex(ValueError, 'sentinel'):
            seed(self.io, '20H392')
        self.assertFalse((self.root / 'private/var/containers').exists())

    def test_existing_history_is_preserved(self):
        pref = self.put(PREF_PATH, {'LastSystemVersion': 'older', 'custom': 1})
        saved = pref.read_bytes()
        with self.assertRaisesRegex(ValueError, 'history'):
            seed(self.io, '20H392')
        self.assertEqual(pref.read_bytes(), saved)
        self.assertFalse((self.root / 'private/var/containers').exists())

    def test_symlink_escape_refused(self):
        with tempfile.TemporaryDirectory() as outside:
            (self.root / 'private/var/containers').symlink_to(outside)
            with self.assertRaisesRegex(ValueError, 'escapes'):
                seed(self.io, '20H392')
            self.assertEqual(list(Path(outside).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
