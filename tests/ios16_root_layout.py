import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from ios16_root_layout import MountedRoot, MIGRATION, VERSION, prepare_layout


@unittest.skipUnless(hasattr(os, 'geteuid') and os.geteuid() == 0,
                     'Root required to verify numeric ownership')
class RootLayoutTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.io = MountedRoot(self.root)
        self.put(VERSION, {'ProductBuildVersion': '20H392'})
        (self.root/'private/var/mobile').mkdir(parents=True)

    def put(self, path, value):
        target = self.root/path.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(plistlib.dumps(value))
        return target

    def test_layout_retains_real_migration(self):
        unrelated = self.put('/private/var/mobile/Library/Preferences/keep.plist', {'keep': True})
        saved = unrelated.read_bytes()
        report = prepare_layout(self.io, '20H392')
        prepare_layout(self.io, '20H392')
        self.assertTrue(report['normal_migration_enabled'])
        self.assertFalse(self.io.exists(MIGRATION))
        self.assertEqual(unrelated.read_bytes(), saved)
        for name, uid in [('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)]:
            st = (self.root/'private/var/containers'/name).stat()
            self.assertEqual((st.st_uid, st.st_gid, st.st_mode & 0o777), (uid, uid, 0o755))

    def test_migration_history_is_not_overwritten(self):
        pref = self.put(MIGRATION, {'LastSystemVersion': '20H392'})
        before = pref.read_bytes()
        with self.assertRaises(ValueError): prepare_layout(self.io, '20H392')
        self.assertEqual(pref.read_bytes(), before)
        self.assertFalse(self.io.exists('/private/var/containers'))

    def test_build_mismatch(self):
        with self.assertRaises(ValueError): prepare_layout(self.io, 'wrong')
        self.assertFalse(self.io.exists('/private/var/containers'))

    def test_active_migration_refused(self):
        self.put('/private/var/datamigrator/migrate', {})
        with self.assertRaises(ValueError): prepare_layout(self.io, '20H392')
        self.assertFalse(self.io.exists('/private/var/containers'))

    def test_symlink_escape_refused(self):
        with tempfile.TemporaryDirectory() as outside:
            (self.root/'private/var/containers').symlink_to(outside)
            with self.assertRaises(ValueError): prepare_layout(self.io, '20H392')
            self.assertEqual(list(Path(outside).iterdir()), [])


if __name__ == '__main__': unittest.main()
