"""Pure simulations of per-domain evidence and the actual device controller.

Only FakeClient/open_session are substituted. No serial, compiler, firmware,
kernel, or ARM64 image is used. Run explicitly with --out NEW after review.
"""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
from unittest.mock import patch

sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1]


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check(value,message):
    if not value: raise AssertionError(message)


def digest(label): return hashlib.sha256(('SIMULATION ONLY: '+label).encode()).hexdigest()


def module(name,version=1,variant=None,core=False):
    return {'image':{'name':name,'version':version,'interface_abi':0x434f0001 if core else 0x47480001,
        'state_abi':1,'kind':2 if core else 0,'dependencies':[]},'sha256':digest(name+str(version)+(variant or '')),
        'core_policy':core,'protocol_policy':not core,'variant':variant,'simulation':True}


def root_row(item,generation=1,cookie=101,epoch=1,state=1):
    image=item['image']
    return {'slot':0,'name':image['name'],'state':state,'active':image['version'],'generation':generation,
        'kind':image['kind'],'interface':image['interface_abi'],'state_abi':image['state_abi'],'deps':0,
        'blocked':int(state!=1),'calls':0,'image_cookie':cookie,'activation_epoch':epoch,'simulation':True}


def gateway_row(name,domain,calls=0,fallbacks=0,cookie=101,generation=1,epoch=1,owner=1,version=1):
    return {'name':name,'domain':domain,'busy':0,'poisoned':0,'calls':calls,'executed':calls,'fallbacks':fallbacks,
        'image_cookie':cookie,'generation':generation,'activation_epoch':epoch,'high_epoch':owner,
        'image_version':version,'simulation':True}


def claim(field,baseline,domain,op='gt'):
    return {'where':{'domain':domain},'field':field,'op':op,
            'baseline':{'step':baseline,'where':{'domain':domain},'field':field}}


class Model:
    def __init__(self,runner):
        self.runner=runner; self.tracker=runner.DeviceCoverage(); self.previous={}

    def event(self,step,response,item=None,before=None,after=None):
        self.previous[step['id']]=response
        result=self.tracker.observe(step,response,item,self.previous,
            {'before':before or [],'after':after if after is not None else before or []})
        print('SIMULATED_EVIDENCE',json.dumps({'step':step,'response':response,'proven':sorted(result)},sort_keys=True))
        return result

    def initial(self,name,core=False):
        item=module(name,core=core); row=root_row(item)
        self.event({'id':'load-'+name,'operation':'load'},
            {'error':0,'state':1,'active':1,'generation':1},item,after=[row])
        return item,row


def model_cases(runner,suite):
    def run(name,body):
        print('@BEGIN',name,flush=True)
        try:
            body()
            result={'id':name,'status':'pass','simulation':True,'device_accessed':False}
        except Exception as error:
            result={'id':name,'status':'fail','reason':str(error),'simulation':True,'device_accessed':False}
        suite.records.append(result); suite.flush()
        print('@RESULT',json.dumps(result,sort_keys=True),flush=True)

    def counter_case(change,target='wifi',explicit=True,field='calls'):
        m=Model(runner); item,row=m.initial('giulia-wifi-proto')
        before=[gateway_row(item['image']['name'],2)]
        step={'id':'before','operation':'gateway'}
        if explicit: step['coverage_domain']='wifi'
        m.event(step,before,before=[row])
        after=dict(before[0]); after.update(change)
        step={'id':'after','operation':'gateway','assert':[claim(field,'before',2)]}
        if explicit: step['coverage_domain']=target
        got=m.event(step,[after],before=[row])
        return m,got

    def cross_domain():
        m,got=counter_case({'calls':5},target='audio')
        check('module_execution' not in got,'Another domain stole a counter')
        report=m.tracker.report()
        check(all(r['module_execution']['status']=='blocked' for r in report['per_domain'].values()),'Unattributed row granted a domain')
    run('cross-domain-counter-target-rejected',cross_domain)

    def absent_target():
        _,got=counter_case({'calls':5},explicit=False)
        check('module_execution' not in got,'GH target was guessed from an assertion row')
    run('gh-without-explicit-target-is-not-attributed',absent_target)

    def unattributed():
        m,got=counter_case({'calls':5})
        check(got=={'module_execution'},'GH counters were promoted beyond module execution')
        record=m.tracker.report()['per_domain']['wifi']
        check(record['module_execution']['evidence'][0]['source']=='resident-unattributed','A source was fabricated')
        check(record['native_input']['status']=='blocked' and record['production_output_adoption']['status']=='not_claimed','Mailbox implies native/adoption')
        check(record['status']=='blocked','Module execution completed a hardware domain')
    run('gh-calls-only-prove-unattributed-module-execution',unattributed)

    for label,change in (
        ('cookie-change',{'calls':5,'image_cookie':999}),
        ('generation-change',{'calls':5,'generation':2}),
        ('activation-epoch-change',{'calls':5,'activation_epoch':2}),
        ('owner-epoch-change',{'calls':5,'high_epoch':2}),
        ('busy-snapshot',{'calls':5,'busy':1}),
        ('platform-touch-only',{'executed':5}),
    ):
        def bad(change=change,label=label):
            _,got=counter_case(change,field='executed' if label=='platform-touch-only' else 'calls')
            check('module_execution' not in got,'Mixed/touched/busy instance granted execution')
        run(label+'-cannot-prove-execution',bad)

    def fixture():
        m=Model(runner); item,row=m.initial('giulia-audio-proto')
        summary={'synthetic':1,'hardware_verified':0,'counter_exact':1,'cases':20,'passed_mask':0xfffff,
            'failed_mask':0,'adopted':16,'epoch':1,'native_provider':1,'hardware_ready':1}
        got=m.event({'id':'fixture','operation':'diagnostic','command':'audio.proto','coverage_domain':'audio'},
            {'scope':'synthetic','complete':True,'summary':summary,'rows':[summary]},before=[row])
        check(got=={'module_execution'},'Fixture gained native/lifecycle coverage')
        report=m.tracker.report(); record=report['per_domain']['audio']
        check(record['module_execution']['evidence'][0]['source']=='fixture','Fixture lost its source label')
        check(all(record[key]['status']!='pass' for key in runner.CLAIMS if key!='module_execution'),'Fixture provider-looking fields granted a higher layer')
        check(report['per_domain']['modem']['module_execution']['status']=='blocked','Fixture credited another domain')
    run('fixture-execution-never-becomes-native-or-adoption',fixture)

    def rollback():
        m=Model(runner); item,row=m.initial('giulia-audio-proto')
        wifi,wifi_row=m.initial('giulia-wifi-proto')
        restored=dict(row,activation_epoch=2)
        failed=module(item['image']['name'],2,'fail-start')
        m.event({'id':'failed-start','operation':'load'},
            {'error':5,'state':1,'active':1,'generation':1},failed,before=[row],after=[restored])
        m.event({'id':'rollback-call','operation':'call','name':item['image']['name'],'value':0},
            {'error':0,'state':1,'active':1,'generation':1,'result':7},before=[restored])
        got=m.event({'id':'other-call','operation':'call','name':wifi['image']['name'],'value':0},
            {'error':0,'state':1,'active':1,'generation':1,'result':7},before=[wifi_row])
        report=m.tracker.report()
        check('rollback' not in got,'Another module stole rollback')
        check(report['per_domain']['audio']['lifecycle']['rollback']['status']=='pass','Actual restored instance lost its rollback proof')
        check(report['per_domain']['wifi']['lifecycle']['rollback']['status']=='blocked','A union completed Wi-Fi rollback')
        check('rollback' in report['summary']['union'] and report['summary']['authoritative'] is False,'Summary was made authoritative')
        check(all(r['status']=='blocked' for r in report['per_domain'].values()),'Global union completed a hardware domain')
    run('rollback-is-per-module-and-union-cannot-complete-another-domain',rollback)

    def duplicate():
        m=Model(runner); item,row=m.initial('giulia-wifi-proto')
        m.event({'id':'before','operation':'gateway','coverage_domain':'wifi'},
            [gateway_row(item['image']['name'],2)],before=[row])
        duplicate_rows=[gateway_row(item['image']['name'],2,calls=3)]*2
        got=m.event({'id':'after','operation':'gateway','coverage_domain':'wifi','assert':[claim('calls','before',2)]},
            duplicate_rows,before=[row])
        check('module_execution' not in got,'Duplicate counter rows gained execution')
    # select() rejects duplicate assertion rows; that rejection is itself the
    # expected controller outcome, not an instruction to weaken identity checks.
    def duplicate_rejected():
        try: duplicate()
        except ValueError: return
        raise AssertionError('Duplicate rows were accepted')
    run('duplicate-domain-rows-are-rejected',duplicate_rejected)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--kit',type=Path,default=ROOT.parent/'UniXNU/kits/NativeHotload')
    p.add_argument('--runner',type=Path,default=ROOT/'scripts/verify-hotload-acceptance.py')
    args=p.parse_args()
    args.out,args.kit,args.runner=args.out.resolve(),args.kit.resolve(),args.runner.resolve()
    args.out.mkdir(parents=True,exist_ok=False)
    sys.path.insert(0,str(args.kit))
    import hotload
    control=load('hotload_evidence_control_base',ROOT/'scripts/check-hotload-runner-control.py')
    runner=load('hotload_evidence_controller_target',args.runner)
    spec=json.loads((ROOT/'hardware/hotload-protocols.json').read_text(encoding='utf-8'))
    modules={item['driver']:item for item in spec['modules']}
    inputs=[Path(__file__).resolve(),args.runner,ROOT/'scripts/check-hotload-runner-control.py',
            ROOT/'reports/hotload-control-20261024/evidence-contract.md']
    before={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    suite=control.Suite(args,runner,hotload,modules,before)
    suite.result.update(schema='giulia-hotload-evidence-control-v1',scope='host-evidence-controller-simulation')

    class EvidenceClient(control.FakeClient):
        def __init__(self,*values):
            super().__init__(*values); self.gateway_reads=0; self.protocol_counts={name:0 for name in runner.DOMAINS}
        def query(self,command):
            self.emit('query',command=command)
            if command=='list':
                result=[]
                for name,value in self.active.items():
                    domain=next(d for d,item in modules.items() if item['name']==name)
                    item=module(name,value['version'])
                    row=root_row(item,value['generation'],1000+runner.DOMAINS.index(domain)*100+value['generation'],100+value['generation'])
                    row['slot']=runner.DOMAINS.index(domain); result.append(row)
                return result
            if command=='gateway':
                self.gateway_reads+=1
                if self.gateway_reads==3: self.protocol_counts['wifi']+=5
                rows=[{'source_sha256':self.scenario.live_source,'simulation':True}]
                for index,domain in enumerate(runner.DOMAINS,1):
                    name=modules[domain]['name']; active=self.active.get(name)
                    if active:
                        rows.append(gateway_row(name,index,self.protocol_counts[domain],cookie=1000+(index-1)*100+active['generation'],
                            generation=active['generation'],epoch=100+active['generation'],version=active['version']))
                return rows
            return []
        def diagnostic(self,command):
            if command!='audio.proto': return super().diagnostic(command)
            self.emit('diagnostic',command=command)
            self.protocol_counts['audio']+=16
            summary={'synthetic':1,'hardware_verified':0,'counter_exact':1,'cases':20,'passed_mask':0xfffff,
                'expected_mask':0xfffff,'failed_mask':0,'attempted':16,'adopted':16,'fallback':0,'rejected':0,'changed':0,'epoch':1}
            return {'command':command,'scope':'synthetic','complete':True,'hardware_verified':False,'summary':summary,'rows':[summary],'simulation':True}

    class EvidenceScenario(control.Scenario):
        def __init__(self,*values,**options):
            super().__init__(*values,**options)
            for item in self.verified.values():
                item['image'].update(interface_abi=0x47480001,state_abi=1,kind=0,dependencies=[])
        @contextmanager
        def open_session(self,port,log,timeout,lock_timeout):
            check(port=='SIMULATION_NO_DEVICE','Simulation reached an actual port')
            self.opens+=1
            with Path(log).open('xb') as capture: yield EvidenceClient(self,capture)

    def controller_result(result):
        check(result['exception'] is None,'Controller raised in evidence simulation')
        check(result['controller_result']['device_accessed'] is False,'Simulation claimed device access')
        return result['controller_result']['device_evidence']
    def counter_assert(result,allowed):
        report=controller_result(result)
        check(report['per_domain']['wifi']['module_execution']['status']==('pass' if allowed else 'blocked'),'Controller domain attribution wrong')
        check(report['per_domain']['audio']['module_execution']['status']=='blocked','Wi-Fi execution credited audio')
        check(all(item['status']=='blocked' for item in report['per_domain'].values()),'Union credited hardware')
        check(report['summary']['authoritative'] is False,'Union became a completion gate')
    def counter_plan(target):
        return [control.load_step('wifi'),{'id':'before','operation':'gateway','coverage_domain':'wifi'},
            {'id':'after','operation':'gateway','coverage_domain':target,'coverage':['real_execution'],
             'assert':[claim('calls','before',2)]}]
    with patch.object(control,'Scenario',EvidenceScenario):
        suite.case('controller-cross-domain-counter-theft',counter_plan('audio'),lambda r:counter_assert(r,False))
        suite.case('controller-gh-legacy-real-execution-is-module-only',counter_plan('wifi'),lambda r:counter_assert(r,True))
        def fixture_result(result):
            report=controller_result(result); record=report['per_domain']['audio']
            check(record['module_execution']['status']=='pass','Controller lost the complete fixture execution')
            check(record['module_execution']['evidence'][0]['source']=='fixture','Controller promoted fixture origin')
            check(all(record[name]['status']!='pass' for name in runner.CLAIMS if name!='module_execution'),'Controller fixture granted native/adoption')
        suite.case('controller-fixture-cannot-satisfy-native-fields',
            [control.load_step('audio'),{'id':'audio-fixture','operation':'diagnostic','command':'audio.proto','coverage_domain':'audio'}],fixture_result)
    model_cases(runner,suite)
    after={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    suite.records.append({'id':'evidence-source-freeze','status':'pass' if before==after else 'fail',
                          'simulation':True,'device_accessed':False,'source_inputs':after})
    suite.result['source_inputs']=before; suite.flush()
    print(json.dumps(suite.result,indent=2,sort_keys=True))
    return {'pass':0,'fail':1,'blocked':2}[suite.result['status']]


if __name__=='__main__': raise SystemExit(main())
