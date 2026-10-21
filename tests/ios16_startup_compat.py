import copy
from pathlib import Path
import plistlib
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from ios16_startup_compat import prepare_startup, LAUNCHD, MEDIA, VIRTUAL_AUDIO, DISABLED_AUDIO, SPRINGBOARD, JOBS
from ios16_root_layout import VERSION, MIGRATION


class Root:
    def __init__(self):
        self.data = {VERSION:plistlib.dumps({'ProductBuildVersion':'20H392'}), VIRTUAL_AUDIO:b'plugin'}
        self.data[LAUNCHD] = plistlib.dumps({'LaunchDaemons':{
            **{job:{'Label':'com.apple.CommCenter','KeepAlive':True} for job in JOBS},
            'accountsd':{'Label':'accountsd'}, 'migration':{'Label':'migration'}}, 'VersionNumber':1})
        for x in ('D22','D221','Default'):
            self.data[MEDIA.format(x)] = plistlib.dumps({'StartWirelessDisplayServer':True})
        self.metadata={}
    def read(self,p):return self.data[p]
    def exists(self,p):return p in self.data
    def write(self,p,data,uid=0,mode=0o644):self.data[p]=data;self.metadata[p]=(uid,mode)
    def directory(self,p,uid):self.data.setdefault(p,b'')
    def move(self,a,b):self.data[b]=self.data.pop(a)


class Tests(unittest.TestCase):
    def test_preserve_real_migration_accounts_and_setup(self):
        root=Root(); before=copy.deepcopy(root.data)
        prepare_startup(root)
        jobs=plistlib.loads(root.read(LAUNCHD))['LaunchDaemons']
        self.assertEqual(jobs,{'accountsd':{'Label':'accountsd'},'migration':{'Label':'migration'}})
        self.assertFalse(root.exists(MIGRATION))
        self.assertEqual(root.read(DISABLED_AUDIO+'/VirtualAudio.plugin'),before[VIRTUAL_AUDIO])
        self.assertEqual(root.metadata[SPRINGBOARD],(501,0o600))
        for x in ('D22','D221','Default'):
            self.assertEqual(plistlib.loads(root.read(MEDIA.format(x))),
                             {'StartWirelessDisplayServer':True,'StartRecorderServer':False})
        self.assertFalse(any('purplebuddy.plist' in p or 'data_ark.plist' in p for p in root.data))
    def test_drift_fails_before_writes(self):
        for change in ('build','migration','job','plugin'):
            root=Root()
            if change=='build':root.data[VERSION]=plistlib.dumps({'ProductBuildVersion':'other'})
            if change=='migration':root.data[MIGRATION]=b'existing'
            if change=='job':root.data[LAUNCHD]=plistlib.dumps({'LaunchDaemons':{}})
            if change=='plugin':del root.data[VIRTUAL_AUDIO]
            before=copy.deepcopy(root.data)
            with self.assertRaises(ValueError):prepare_startup(root)
            self.assertEqual(root.data,before)

if __name__=='__main__':unittest.main()
