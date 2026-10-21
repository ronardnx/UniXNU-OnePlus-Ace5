from pathlib import Path
import copy,plistlib,sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from ios16_boot_baseline import BASELINE,LOG_ENV,prepare_baseline
from ios16_root_layout import MIGRATION,VERSION
from ios16_startup_compat import LAUNCHD

class Root:
    def __init__(self):
        self.data={VERSION:plistlib.dumps({'ProductBuildVersion':'20H392','ProductVersion':'16.7.16'}),
                   LAUNCHD:plistlib.dumps({'LaunchDaemons':{'a':{'Label':'a','EnvironmentVariables':{'KEEP':'yes'}},
                                                           'b':{'Label':'b'}},'Other':{'keep':True}})}
        self.metadata={}
    def exists(self,p):return p in self.data
    def read(self,p):return self.data[p]
    def directory(self,p,uid):self.data.setdefault(p,b'')
    def write(self,p,data,uid=0,mode=0o644):self.data[p]=data;self.metadata[p]=(uid,mode)

class Tests(unittest.TestCase):
    def test_exact_upstream_schema_and_setup_retained(self):
        r=Root();out=prepare_baseline(r,True)
        self.assertEqual(plistlib.loads(r.read(MIGRATION)),BASELINE)
        self.assertEqual(r.metadata[MIGRATION],(501,0o600))
        self.assertFalse(out['setup_completion_seeded'])
        d=plistlib.loads(r.read(LAUNCHD));self.assertEqual(d['Other'],{'keep':True})
        self.assertEqual(d['LaunchDaemons']['a']['EnvironmentVariables'],{'KEEP':'yes',**LOG_ENV})
        self.assertEqual(d['LaunchDaemons']['b']['EnvironmentVariables'],LOG_ENV)
        self.assertFalse(any('purplebuddy' in p or 'data_ark' in p for p in r.data))
    def test_existing_state_and_wrong_version_are_refused(self):
        for field in ('history','active','version'):
            r=Root()
            if field=='history':r.data[MIGRATION]=b'existing history'
            if field=='active':r.data['/private/var/datamigrator/migrate']=b'active'
            if field=='version':r.data[VERSION]=plistlib.dumps({'ProductBuildVersion':'wrong'})
            before=copy.deepcopy(r.data)
            with self.assertRaises(ValueError):prepare_baseline(r,True)
            self.assertEqual(r.data,before)

if __name__=='__main__':unittest.main()
