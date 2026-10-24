"""Offline, explicitly simulated relocation of 15 core and 20 GH artifacts.

Uses real, already-built object/ELF/images and the production verifier. Copies
to --out NEW before changing only copied report/dependency paths. Never builds
code, executes ARM64, opens a device or changes the original artifact/source.
The result is controller-relocation evidence, not a new CI compilation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import sys
from types import SimpleNamespace
import uuid


ROOT=Path(__file__).resolve().parents[1]
CORE_NAMES={'xnu-sched','xnu-vm','xnu-vfs'}
GH_NAMES={'giulia-gpu-proto','giulia-wifi-proto','giulia-audio-proto','giulia-modem-proto'}
VARIANTS={'baseline','upgrade','fail-start','fail-import','fail-stop'}


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def read(path):return json.loads(path.read_text(encoding='utf-8'))
def json_sha(value):return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()


class MissingFixture(Exception):pass


def write_report(path,report):
    report['build_inputs_sha256']=json_sha(report['build_inputs'])
    report['source_inputs_sha256']=json_sha(report['source_inputs'])
    path.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')


def refresh_command_prefixes(report):
    build=report['build_inputs']
    flags=build['compile_flags']
    for key in ('compile_command','dependency_command'):
        command=report[key]
        # All fixture mutations retain flag count, as in the real builder.
        report[key]=[build['compiler'],*flags,*command[1+len(flags):]]
    command=report['link_command'];link_flags=build['link_flags']
    report['link_command']=[build['linker'],*link_flags,*command[1+len(link_flags):]]


def relocate_copy(path,args,verifier,prefix,index):
    report_path=path.parent/'module.json';report=read(report_path)
    layout=verifier['module_source_layout'](args,verifier['verify_module'](args,path)['image'],report['build_inputs'])
    normalize=verifier['module_recorded_path'];under=verifier['module_relative_path']
    old_kit=dict(layout['recorded_roots'])['kit']
    old_device=dict(layout['recorded_roots']).get('include_0')
    if old_device:old_device=old_device[:-len('/ios16')]
    flags=report['build_inputs']['compile_flags']
    obj=normalize(report['compile_command'][-1]);stage=obj.rsplit('/',1)[0]
    replacements=[(stage,prefix+'/staging/'+str(index)),(old_kit,prefix+'/NativeHotload')]
    if old_device:replacements.append((old_device,prefix+'/device'))
    def replacement(value):
        try:normalized=normalize(value)
        except ValueError:return value
        for old,new in replacements:
            suffix=under(normalized,old)
            if suffix is not None:return new+('/'+suffix if suffix else '')
        return value
    original_sha=sha(report_path)
    report['build_inputs']['source']=replacement(report['build_inputs']['source'])
    report['build_inputs']['compile_flags']=[replacement(value) for value in flags]
    report['build_inputs']['link_flags']=[replacement(value) for value in report['build_inputs']['link_flags']]
    for key in ('compile_command','dependency_command','link_command'):
        report[key]=[replacement(value) for value in report[key]]
    for name in ('module.d','module-before.d'):
        target=path.parent/name
        dependencies=verifier['module_dependency_paths'](target.read_text(encoding='utf-8'))
        rewritten=[replacement(value) for value in sorted(dependencies)]
        target.write_text('nlh-module: '+' '.join(rewritten)+'\n',encoding='utf-8',newline='\n')
        report['artifacts'][name]=sha(target)
    report['relocation_simulation']={'simulation':True,'not_recompiled':True,
        'original_module_manifest_sha256':original_sha,
        'purpose':'test verifier relocation only; object/ELF/image bytes unchanged'}
    write_report(report_path,report)


def update_suite_inventory(folder):
    for filename in ('core-suite.json','protocol-suite.json'):
        path=folder/filename
        if not path.is_file():continue
        suite=read(path)
        for entry in suite.get('artifacts',[]):
            manifest=(folder/entry['module_manifest']).resolve()
            if not manifest.is_relative_to(folder.resolve()):raise ValueError('Suite manifest escapes copied fixture')
            entry['module_manifest_sha256']=sha(manifest)
        suite['relocation_simulation']={'simulation':True,'not_recompiled':True,
            'purpose':'controller path relocation; original compilation logs retained unchanged'}
        path.write_text(json.dumps(suite,indent=2)+'\n',encoding='utf-8',newline='\n')


def check_inventory(paths,names,count):
    keys={(read(path.parent/'module.json')['name'],path.parent.name) for path in paths}
    if len(paths)!=count or keys!={(name,variant) for name in names for variant in VARIANTS}:
        raise MissingFixture('Requires complete real '+str(count)+' image suite with exact names/five variants')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kit',type=Path,required=True)
    p.add_argument('--device-root',type=Path,default=ROOT)
    p.add_argument('--core-suite',type=Path,required=True)
    p.add_argument('--protocol-suite',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    for name in ('kit','device_root','core_suite','protocol_suite','out'):setattr(a,name,getattr(a,name).resolve())
    if a.out.exists():p.error('--out must be a new directory')
    if a.out.is_relative_to(a.core_suite) or a.out.is_relative_to(a.protocol_suite):
        p.error('--out cannot be inside either immutable input suite')
    a.out.mkdir(parents=True)
    result={'schema':1,'status':'blocked','simulation':True,'not_recompiled':True,
            'device_accessed':False,'hardware_tested':False,'cases':[],'relocated_modules':[]}
    def case(name,expected,action,terms=()):
        try:
            detail=action()
            status='pass' if expected=='accept' else 'fail'
            evidence={'accepted':True}
            if isinstance(detail,dict):detail={k:v for k,v in detail.items() if k!='bytes'}
            if detail is not None:evidence['evidence']=detail
        except (ValueError,OSError,KeyError,TypeError) as error:
            reason=str(error)
            status='pass' if expected=='reject' and (not terms or any(term in reason for term in terms)) else 'fail'
            evidence={'accepted':False,'reason':reason}
        record={'id':name,'status':status,'expected':expected,**evidence}
        raw=a.out/(name+'.json');raw.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8',newline='\n')
        result['cases'].append({**record,'raw_log':raw.name})
        if status!='pass':raise ValueError('Relocation fixture case failed: '+name)
    try:
        sys.path.insert(0,str(a.kit))
        verifier=runpy.run_path(str(a.device_root/'scripts/verify-hotload-acceptance.py'))
        args=SimpleNamespace(kit=a.kit,device_root=a.device_root)
        originals=[*sorted(a.core_suite.rglob('*.nlh')),*sorted(a.protocol_suite.rglob('*.nlh'))]
        check_inventory(sorted(a.core_suite.rglob('*.nlh')),CORE_NAMES,15)
        check_inventory(sorted(a.protocol_suite.rglob('*.nlh')),GH_NAMES,20)
        source_before={str(path):sha(path) for image in originals for path in
                       (image,image.parent/'module.json',image.parent/'module.o',image.parent/'module.elf',
                        image.parent/'module.d',image.parent/'module-before.d')}
        for image in originals:verifier['verify_module'](args,image)
        result['original_inputs_sha256']=source_before
        for name,source in (('core',a.core_suite),('protocol',a.protocol_suite)):
            target=a.out/name
            if not target.resolve().is_relative_to(a.out):raise ValueError('Fixture copy escaped new output root')
            shutil.copytree(source,target,symlinks=True)
        relocated=[*sorted((a.out/'core').rglob('*.nlh')),*sorted((a.out/'protocol').rglob('*.nlh'))]
        prefix='/__unixnu_hotload_relocation__/simulation-'+uuid.uuid4().hex
        if Path(prefix).exists():raise ValueError('Simulated missing CI root unexpectedly exists')
        result['simulated_recorded_root']=prefix
        result['limitation']='Only copied report/dependency paths changed. No compiler ran; this is not new CI provenance.'
        for index,image in enumerate(relocated):relocate_copy(image,args,verifier,prefix,index)
        update_suite_inventory(a.out/'core');update_suite_inventory(a.out/'protocol')
        for index,image in enumerate(relocated):
            def accept(image=image):
                metadata=verifier['verify_module'](args,image)
                entry={'image':str(image.relative_to(a.out)),'sha256':metadata['sha256'],
                       'module_manifest_sha256':metadata['module_manifest_sha256'],
                       'path_relocation':metadata['path_relocation']}
                result['relocated_modules'].append(entry)
                return entry
            case('relocated-'+str(index),'accept',accept)
        # Copy the exact checked source closure into NEW before negative data
        # changes. Never mutate real kit/device sources, even temporarily.
        local=SimpleNamespace(kit=a.out/'current-kit',device_root=a.out/'current-device')
        closure={}
        for image in relocated:
            report=read(image.parent/'module.json')
            metadata=verifier['verify_module'](args,image)
            layout=verifier['module_source_layout'](args,metadata['image'],report['build_inputs'])
            for key,digest in report['source_inputs'].items():
                label,suffix=key.split('/',1);source=(layout['current_roots'][label]/suffix).resolve()
                if source.is_relative_to(a.kit):target=local.kit/source.relative_to(a.kit)
                elif source.is_relative_to(a.device_root):target=local.device_root/source.relative_to(a.device_root)
                else:raise ValueError('Source closure escapes the checked kit/device')
                closure[target]=(source,digest,image)
        for name in read(relocated[0].parent/'module.json')['kit_inputs']:
            closure[local.kit/name]=(a.kit/name,sha(a.kit/name),relocated[0])
        spec=a.device_root/'hardware/hotload-protocols.json'
        closure[local.device_root/'hardware/hotload-protocols.json']=(spec,sha(spec),relocated[-1])
        for target,(source,digest,image) in closure.items():
            if sha(source)!=digest:raise ValueError('True source changed before closure copy')
            target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target)
        case('copied-source-closure','accept',lambda:{'modules':sum(
            bool(verifier['verify_module'](local,image)) for image in relocated)})
        for index,(target,(source,digest,image)) in enumerate(sorted(closure.items())):
            if target.suffix!='.h':continue
            original=target.read_bytes()
            try:
                target.write_bytes(original+b'\n/* relocation negative fixture: changed header */\n')
                case('changed-header-'+str(index),'reject',lambda image=image:verifier['verify_module'](local,image),
                     ('different current kit','Source/header differs'))
            finally:target.write_bytes(original)
        representatives=(relocated[0],next(path for path in relocated if path.name.startswith('giulia-')))
        for index,image in enumerate(representatives):
            report_path=image.parent/'module.json';original_report=report_path.read_bytes()
            report=read(report_path);flags=report['build_inputs']['compile_flags']
            positions=[i+1 for i,value in enumerate(flags[:-1]) if value=='-I']
            try:
                if index==0:
                    flags[positions[0]]=prefix+'/wrong-kit';flags[positions[1]]=prefix+'/wrong-kit/core'
                    report['build_inputs']['link_flags'][-1]=prefix+'/wrong-kit/module.ld'
                else:
                    flags[positions[2]]=prefix+'/wrong-device/ios16';flags[positions[3]]=prefix+'/wrong-device/ios16/common'
                refresh_command_prefixes(report);write_report(report_path,report)
                case('wrong-root-'+str(index),'reject',lambda image=image:verifier['verify_module'](args,image),
                     ('exact official source','exact recorded GH device root'))
            finally:report_path.write_bytes(original_report)
            for dependency_name in ('module.d','module-before.d'):
                dependency=image.parent/dependency_name;original_dependency=dependency.read_bytes()
                try:
                    report=read(report_path)
                    layout=verifier['module_source_layout'](args,report,report['build_inputs'])
                    values=sorted(verifier['module_dependency_paths'](dependency.read_text(encoding='utf-8')))
                    omitted=next(value for value in values if value!=layout['recorded_source'])
                    dependency.write_text('nlh-module: '+' '.join(value for value in values if value!=omitted)+'\n',encoding='utf-8',newline='\n')
                    report['artifacts'][dependency_name]=sha(dependency);write_report(report_path,report)
                    case('missing-dependency-'+str(index)+'-'+dependency_name.replace('.','-'),'reject',
                         lambda image=image:verifier['verify_module'](args,image),('dependency set does not match',))
                finally:
                    dependency.write_bytes(original_dependency);report_path.write_bytes(original_report)
        if {name:sha(Path(name)) for name in source_before}!=source_before:
            raise ValueError('Original artifacts changed during simulation')
        if any(sha(source)!=digest for source,digest,image in closure.values()):
            raise ValueError('Original true source changed during simulation')
        result.update(status='pass',module_count=len(relocated))
    except MissingFixture as error:result.update(status='blocked',reason=str(error))
    except (ValueError,OSError,KeyError,TypeError) as error:result.update(status='fail',reason=str(error))
    target=a.out/'result.json';target.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps({'status':result['status'],'simulation':True,'not_recompiled':True,
                      'modules':len(result['relocated_modules']),'result':str(target)},sort_keys=True))
    return {'pass':0,'fail':1,'blocked':2}[result['status']]


if __name__=='__main__':raise SystemExit(main())
