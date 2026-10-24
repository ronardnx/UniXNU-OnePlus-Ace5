"""Reproducible hotload evidence; never opens a device without --execute-device.

Host: --kit ../UniXNU/kits/NativeHotload --output out/NEW --host-runtime
      (Linux/WSL cc, pthread and OpenSSL development headers are required.)
Artifact: --manifest reviewed.json [--kernel FILE --kernel-sha256 HEX]
          [--module-suite DIRECTORY] [--linked-report JSON]
Device: add --plan reviewed-plan.json --port COM21 --execute-device.

Manifest v1: {"schema":"giulia-hotload-acceptance-v1", "artifacts":[
 {"path":"...","sha256":"...","role":"kernel|boot_image|build_manifest|module_suite"}],
 "source_sha256":{"/absolute/or/manifest-relative/file":"sha256"}}
Plan v1: {"schema":"giulia-hotload-device-plan-v1", "boot_record":
 {"path":"...","sha256":"..."}, "steps":[{"id":"...",
 "operation":"status|memory|list|core|gateway|load|call|unload",
 "image":"...", "name":"...", "value":0, "expect_error":0,
 "assert":[{"where":{"domain":1},"field":"calls","op":"gt",
 "baseline":{"step":"before","where":{"domain":1},"field":"calls"}}],
 "coverage_module":"giulia-wifi-proto", "coverage_domain":"wifi",
 "coverage":["module_execution"], "stop_failure":false}]}
V2 uses the same operations with explicit coverage_module/coverage_domain.
Legacy GH real_execution labels never grant native or hardware evidence.
All paths resolve relative to their containing JSON. Assert supports eq/ne/gt/ge/lt/le,
with literal "value" or a prior-step baseline; rows select by exact "where".
STOP failure must be the final mutation; read-only isolation evidence may follow.
No shell command or arbitrary hardware write
is accepted. Protocol modules do not establish missing hardware providers.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shlex
import shutil
import struct
import subprocess
import sys
import time

REQUIRED = ('upload', 'real_execution', 'replace', 'state_migration', 'concurrent_calls',
            'old_epoch_rejected', 'start_failure', 'import_failure', 'rollback',
            'unload', 'resident_fallback', 'stop_failure_isolation')
DOMAINS = ('gpu', 'wifi', 'audio', 'modem')
GH_MODULES = {name:'giulia-'+name+'-proto' for name in DOMAINS}
CORE_MODULES = {'xnu-sched':1,'xnu-vm':2,'xnu-vfs':3}
CLAIMS = ('module_execution','production_output_adoption','fix_semantics',
          'native_input','native_adoption','native_unload_fallback','hardware_function')
PLAN_COVERAGE = (*REQUIRED, *CLAIMS)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b''): digest.update(block)
    return digest.hexdigest()


def read_json(path): return json.loads(Path(path).read_text(encoding='utf-8'))
def relative(base, path): return (base / path).resolve()
def json_sha(value): return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()


def declared_path(value):
    """Read builder absolute paths on Windows or its WSL mount of this tree."""
    if os.name=='nt' and re.match(r'^/mnt/[a-z]/',value):
        value=value[5].upper()+':/'+value[7:]
    elif os.name!='nt' and re.match(r'^[A-Za-z]:[\\/]',value):
        value='/mnt/'+value[0].lower()+'/'+value[3:].replace('\\','/')
    return Path(value).resolve()


class Ledger:
    def __init__(self, output):
        self.output, self.steps = output, []
        self.data = {'schema': 'giulia-hotload-acceptance-result-v1', 'status': 'blocked',
                     'steps': self.steps, 'scopes': {}, 'device_accessed': False}

    def add(self, name, scope, status, **details):
        entry = {'id': name, 'scope': scope, 'status': status, **details}
        self.steps.append(entry)
        self.flush()
        return entry

    def flush(self):
        statuses = [step['status'] for step in self.steps]
        self.data['status'] = ('fail' if 'fail' in statuses else 'blocked'
                               if not statuses or 'blocked' in statuses else 'pass')
        scopes={}
        for step in self.steps:
            counts=scopes.setdefault(step['scope'],{'pass':0,'fail':0,'blocked':0})
            counts[step['status']]+=1
        for counts in scopes.values():
            counts['status']='fail' if counts['fail'] else 'blocked' if counts['blocked'] else 'pass'
        self.data['scopes']=scopes
        path = self.output / 'result.json'
        temporary = path.with_suffix('.json.tmp')
        temporary.write_text(json.dumps(self.data, indent=2, sort_keys=True) + '\n', encoding='utf-8')
        temporary.replace(path)

    def process(self, name, command, timeout=120):
        raw = self.output / (name + '.log')
        with raw.open('xb') as log:
            log.write(('ARGV ' + json.dumps(command) + '\n').encode())
            log.flush()
            try:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
                return result.returncode, raw
            except subprocess.TimeoutExpired:
                log.write(b'\nPROCESS TIMEOUT; no automatic retry\n')
                return 124, raw


def collect_cases(ledger, raw, scope, expected):
    text = raw.read_text(encoding='utf-8', errors='replace')
    found = set()
    for block in re.split(r'(?m)^@BEGIN ', text)[1:]:
        name = block.splitlines()[0].strip()
        if not re.fullmatch('[a-z0-9_-]+', name): continue
        path = ledger.output / (scope + '-' + name + '.log')
        path.write_text('@BEGIN ' + block, encoding='utf-8')
        match = re.search(r'(?m)^@RESULT (.+)$', block)
        result = json.loads(match[1]) if match else {'status': 'fail', 'reason': 'case did not finish'}
        found.add(name)
        ledger.add(name, scope, result['status'], raw_log=str(path), evidence=result)
    if len(found) != expected:
        ledger.add(scope + '-completeness', scope, 'fail', reason=f'expected {expected} cases; observed {len(found)}', raw_log=str(raw))


def host_runtime(args, ledger):
    scope = 'host_runtime'
    compiler = shlex.split(args.cc)
    if not compiler or not shutil.which(compiler[0]):
        ledger.add('host-runtime-build', scope, 'blocked', reason='No host C compiler; run this script under Linux/WSL with cc, pthread and OpenSSL headers')
        return
    shim = ledger.output / 'shim-include'
    for name in ('sys/systm.h', 'sys/errno.h', 'kern/locks.h', 'kern/thread.h',
                 'mach/thread_act.h', 'kern/clock.h', 'kern/kalloc.h',
                 'machine/machine_routines.h', 'libkern/crypto/sha2.h'):
        path = shim / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "shim.h"\n', encoding='utf-8')
    source = args.kit / 'native_hotload.c'
    inputs = [source, args.kit/'native_hotload.h', args.kit/'native_hotload_core.h',
              args.kit/'core/native_core_algorithms.h', args.kit/'tests/runtime_harness.c',
              args.kit/'tests/shim.h']
    before = {str(path): sha(path) for path in inputs}
    ledger.data['host_runtime_inputs'] = before
    binary = ledger.output / ('runtime-harness.exe' if os.name == 'nt' else 'runtime-harness')
    command = compiler + ['-std=gnu11', '-O1', '-g', '-pthread', '-Wno-deprecated-declarations',
                          '-DKERNEL', '-DNATIVE_RUNTIME_SOURCE=' + json.dumps(str(source)),
                          '-I'+str(shim), '-I'+str(args.kit/'tests'), '-I'+str(args.kit),
                          '-I'+str(args.kit/'core'), str(args.kit/'tests/runtime_harness.c'),
                          '-lcrypto', '-o', str(binary)]
    code, raw = ledger.process('host-runtime-build', command)
    after = {str(path): sha(path) for path in inputs}
    if before != after:
        ledger.add('host-runtime-build', scope, 'blocked', reason='Runtime changed during compilation; rerun the complete group once sources are frozen', raw_log=str(raw))
        return
    ledger.add('host-runtime-build', scope, 'pass' if code == 0 else 'fail', exit_code=code,
               raw_log=str(raw), limitation='Host callbacks and VM/OS shims; no ARM64 bytes, pmap or physical hardware execution')
    if code: return
    code, raw = ledger.process('host-runtime-execute', [str(binary)], timeout=45)
    collect_cases(ledger, raw, scope, 10)
    ledger.add('host-runtime-execute', scope, 'pass' if code == 0 else 'fail', exit_code=code, raw_log=str(raw))
    bridge=args.bridge
    header=bridge.parent.parent/'common/giulia_hotload.h'
    if not bridge.is_file() or not header.is_file():
        ledger.add('bridge-host-build','host_bridge','blocked',reason='Actual resident bridge/public header not found',path=str(bridge))
        return
    bridge_inputs=inputs+[bridge,header,args.kit/'tests/bridge_harness.c']
    before={str(path):sha(path) for path in bridge_inputs}
    binding=hashlib.sha256(json.dumps(before,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    ledger.data['host_bridge_inputs']=before
    for suffix,source_id,expected_cases in (('bound',binding,10),('unbound','unbound',1)):
        binary=ledger.output/('bridge-harness-'+suffix)
        command=compiler+['-std=gnu11','-O1','-g','-pthread','-Wno-deprecated-declarations',
                         '-DKERNEL','-DGIULIA_HOTLOAD_ENABLED=1',
                         '-DNATIVE_RUNTIME_SOURCE='+json.dumps(str(source)),
                         '-DHOTLOAD_BRIDGE_SOURCE='+json.dumps(str(bridge)),
                         '-DGIULIA_HOTLOAD_SOURCE_ID='+json.dumps(source_id),
                         '-I'+str(shim),'-I'+str(args.kit/'tests'),'-I'+str(args.kit),
                         '-I'+str(args.kit/'core'),str(args.kit/'tests/bridge_harness.c'),
                         '-lcrypto','-o',str(binary)]
        code,raw=ledger.process('bridge-host-build-'+suffix,command)
        if before!={str(path):sha(path) for path in bridge_inputs}:
            ledger.add('bridge-host-build-'+suffix,'host_bridge','blocked',reason='Sources changed during build; rerun complete frozen group',raw_log=str(raw)); return
        ledger.add('bridge-host-build-'+suffix,'host_bridge','pass' if code==0 else 'fail',exit_code=code,raw_log=str(raw),source_id=source_id,
                   limitation='Actual bridge/runtime with native host fixtures, no ARM64 or hardware execution')
        if code: return
        code,raw=ledger.process('bridge-host-execute-'+suffix,[str(binary)],timeout=45)
        collect_cases(ledger,raw,'host_bridge_'+suffix,expected_cases)
        ledger.add('bridge-host-execute-'+suffix,'host_bridge','pass' if code==0 else 'fail',exit_code=code,raw_log=str(raw))


def artifacts(args, ledger):
    records, source_hashes = [], {}
    if args.manifest:
        manifest = read_json(args.manifest)
        if manifest.get('schema') not in ('giulia-hotload-acceptance-v1',2):
            raise ValueError('Unsupported acceptance manifest schema')
        base = args.manifest.parent
        records = [{**item, 'path': str(relative(base,item['path']))} for item in manifest.get('artifacts',[])]
        source_hashes = {str(relative(base,name)): digest for name,digest in manifest.get('source_sha256',{}).items()}
        if manifest.get('schema')==2:
            records.append({'role':'build_manifest','path':str(args.manifest),'sha256':sha(args.manifest)})
            source_hashes.update({str((args.kit/name).resolve()):digest for name,digest in manifest.get('files',{}).items()})
            source_hashes.update({str((args.device_root/name).resolve()):digest for name,digest in manifest.get('device_files',{}).items()})
            binding=hashlib.sha256(json.dumps({'kit':manifest.get('files',{}),'device':manifest.get('device_files',{}),
                'selected':manifest.get('gateway_selected',[])},sort_keys=True,separators=(',',':')).encode()).hexdigest()
            valid=binding==manifest.get('gateway_source_sha256') and manifest.get('platform_abi')==1 and manifest.get('gateway_abi')==1
            ledger.add('profile-gateway-source-binding','artifacts','pass' if valid else 'fail',expected_sha256=manifest.get('gateway_source_sha256'),actual_sha256=binding)
            if not valid: raise ValueError('Profile lacks valid new resident gateway/source binding')
            ledger.data['gateway_source_sha256']=binding
        elif manifest.get('gateway_source_sha256'):
            ledger.data['gateway_source_sha256']=manifest['gateway_source_sha256']
        ledger.data['manifest_sha256'] = sha(args.manifest)
    if args.kernel:
        records.append({'role':'kernel','path':str(args.kernel),'sha256':args.kernel_sha256})
    okay = True
    for index, item in enumerate(records):
        path = Path(item['path'])
        expected = item.get('sha256','')
        actual = sha(path) if path.is_file() else None
        valid = bool(re.fullmatch('[0-9a-f]{64}',expected or '') and expected == actual)
        ledger.add('artifact-'+str(index), 'artifacts', 'pass' if valid else 'fail',
                   role=item.get('role'), path=str(path), expected_sha256=expected, actual_sha256=actual)
        okay &= valid
    for index,(name,expected) in enumerate(source_hashes.items()):
        actual = sha(name) if Path(name).is_file() else None
        valid = actual == expected
        ledger.add('source-'+str(index), 'artifacts', 'pass' if valid else 'fail', path=name, expected_sha256=expected, actual_sha256=actual)
        okay &= valid
    required_source = {str((args.kit/name).resolve()) for name in ('native_hotload.c','native_hotload.h')}
    roles = {item.get('role') for item in records}
    bound = {'kernel','boot_image','build_manifest','module_suite'} <= roles and required_source <= set(source_hashes)
    if not bound:
        ledger.add('artifact-source-binding','artifacts','blocked',reason='Requires kernel, boot_image, build_manifest and module_suite artifact hashes plus current runtime .c/.h source hashes')
    verified_modules={}
    if args.module_suite:
        modules = sorted(args.module_suite.rglob('*.nlh'))
        if not modules:
            ledger.add('arm64-module-suite','arm64_artifacts','blocked',reason='No .nlh images found')
        for index,path in enumerate(modules):
            try:
                metadata = verify_module(args,path)
                verified_modules[str(path.resolve())]=metadata
                ledger.add('module-'+str(index),'arm64_artifacts','pass',path=str(path),sha256=metadata['sha256'],metadata=metadata['image'],
                           limitation='Actual AArch64 ELF/object, frozen sources, build inputs and image equality verified; device execution remains separate')
            except (ValueError,OSError,KeyError,TypeError) as error:
                okay = False
                ledger.add('module-'+str(index),'arm64_artifacts','fail',path=str(path),reason=str(error))
    ledger.data['verified_modules']={key:{k:v for k,v in value.items() if k!='bytes'} for key,value in verified_modules.items()}
    args.verified_modules=verified_modules
    declared_modules={}
    suites=[item for item in records if item.get('role')=='module_suite']
    profiles=[item for item in records if item.get('role')=='build_manifest' and Path(item['path']).is_file() and read_json(item['path']).get('schema')==2]
    for suite in suites:
        suite_path=Path(suite['path'])
        inventory=read_json(suite_path)
        if 'hardware_protocol_suite_requested' in inventory:
            if inventory.get('status')!='pass' or not profiles:
                ledger.add('protocol-suite-completeness','arm64_artifacts','blocked',reason='A complete passing protocol suite and its resident profile are required')
                continue
            report_path=ledger.output/('protocol-suite-reverified-'+str(len(declared_modules))+'.json')
            command=[sys.executable,str(args.device_root/'scripts/build-hotload-protocols.py'),
                     '--kit',str(args.kit),'--resident-manifest',profiles[0]['path'],
                     '--verify-suite',str(suite_path),'--report',str(report_path),
                     '--domains',*inventory.get('requested_domains',[])]
            code,raw=ledger.process('protocol-suite-reverify-'+str(len(declared_modules)),command,timeout=120)
            ledger.add('protocol-suite-completeness','arm64_artifacts','pass' if code==0 else 'blocked' if code==3 else 'fail',exit_code=code,raw_log=str(raw))
            if code: continue
        else:
            domains={'xnu-sched':1,'xnu-vm':2,'xnu-vfs':3}
            variants={'baseline','upgrade','fail-start','fail-import','fail-stop'}
            expected={(name,variant) for name in domains for variant in variants}
            entries=inventory.get('artifacts',[])
            pairs=[(entry.get('name'),entry.get('variant')) for entry in entries]
            if (inventory.get('schema')!=1 or inventory.get('abi')!=2 or inventory.get('domains')!=domains or
                len(pairs)!=15 or len(set(pairs))!=15 or set(pairs)!=expected):
                ledger.add('core-suite-completeness','arm64_artifacts','blocked',reason='Core suite must contain exactly all three domains and five variants; partial suite cannot establish readiness')
                continue
            if any(sha(args.kit/name)!=digest for name,digest in inventory.get('inputs',{}).items()) or not inventory.get('inputs'):
                raise ValueError('Core suite source/tool inputs differ from current kit')
            ledger.add('core-suite-completeness','arm64_artifacts','pass',artifacts=15)
        for entry in inventory.get('artifacts',[]):
            image_path=relative(suite_path.parent,entry['image'])
            if not image_path.is_relative_to(suite_path.parent.resolve()):
                raise ValueError('Suite image escapes its recorded artifact directory')
            if str(image_path) in declared_modules:
                raise ValueError('Duplicate image in hashed suite inventory')
            module=verified_modules.get(str(image_path))
            if not module or module['sha256']!=entry['image_sha256']:
                raise ValueError('Suite image is not in the actually verified module set')
            variant=entry.get('variant')
            if variant in ('fail-start','fail-import','fail-stop') and module.get('variant')!=variant:
                raise ValueError('Suite failure variant differs from actual verified compile definitions')
            declared_modules[str(image_path)]=entry['image_sha256']
    suite_bound=bool(declared_modules) and declared_modules=={name:item['sha256'] for name,item in verified_modules.items()}
    bound &= suite_bound
    ledger.add('module-suite-inventory-binding','arm64_artifacts','pass' if suite_bound else 'blocked',
               reason=None if suite_bound else 'The hashed module_suite inventory must name exactly every verified image and SHA256; no alternate suite may be uploaded')
    kernels=[item for item in records if item.get('role')=='kernel']
    if kernels and profiles and okay:
        report_path=ledger.output/'linked-kernel-verified.json'
        command=[sys.executable,str(args.device_root/'scripts/check-ios16-hotload-kernel.py'),kernels[0]['path'],
                 '--manifest',profiles[0]['path'],'--source',str(args.kit),'--device-source',str(args.device_root),'--report',str(report_path)]
        code,raw=ledger.process('linked-kernel-reverify',command,timeout=180)
        report=read_json(report_path) if report_path.is_file() else {}
        valid=code==0 and report.get('schema')==1 and report.get('verified') is True and report.get('status')=='pass'
        valid &= report.get('kernel_sha256')==kernels[0]['sha256'] and report.get('gateway_source_sha256')==ledger.data.get('gateway_source_sha256')
        selected=read_json(profiles[0]['path']).get('gateway_selected',[])
        attribution=report.get('hardware_protocol_call_attribution',{})
        valid &= all(attribution.get(domain,{}).get('gateway_calls') and attribution.get(domain,{}).get('inbound_calls') for domain in selected)
        if args.linked_report:
            supplied=read_json(args.linked_report)
            valid &= supplied.get('schema')==1 and supplied.get('verified') is True and supplied.get('status')=='pass'
            valid &= all(supplied.get(key)==report.get(key) for key in ('kernel_sha256','gateway_source_sha256','hardware_protocol_call_attribution'))
        ledger.add('linked-kernel-entry-report','arm64_artifacts','pass' if valid else 'fail',path=str(report_path),raw_log=str(raw),exit_code=code)
        bound &= bool(valid)
    else:
        bound = False
        ledger.add('linked-kernel-entry-report','arm64_artifacts','blocked',reason='New target kernel/link-entry verification report is absent')
    ledger.data['artifacts'] = records
    package_bound=verify_packaging(args,ledger,records)
    return okay and bound and package_bound


def packaging_wsl_local_path(path):
    """Only existing fixed local Windows drives have an accepted WSL map."""
    if os.name!='nt':raise ValueError('WSL packaging fallback is Windows-only')
    import ctypes
    path=Path(path).resolve(strict=True)
    if not re.fullmatch(r'[A-Za-z]:',path.drive) or path.anchor.startswith('\\\\'):
        raise ValueError('WSL payload verification rejects UNC/unknown drive paths')
    kernel32=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel32.GetDriveTypeW.argtypes=[ctypes.c_wchar_p]
    kernel32.GetDriveTypeW.restype=ctypes.c_uint
    if kernel32.GetDriveTypeW(path.anchor)!=3:
        raise ValueError('WSL payload verification requires an existing fixed local drive')
    return ('/mnt/'+path.drive[0].lower()+'/'+path.as_posix()[3:]).rstrip('/')


def packaging_verification_command(args,ledger,payload):
    """Choose local FAT tooling; WSL never receives a device operation."""
    script=args.device_root/'scripts/verify-payload.py'
    if shutil.which('mcopy'):
        return [sys.executable,str(script),str(payload.parent)],{'execution':'native-readonly'}
    wsl=shutil.which('wsl.exe') if os.name=='nt' else None
    if not wsl:
        ledger.add('payload-tool-preflight','artifacts','blocked',
                   reason='mcopy is absent; supported Windows fallback requires existing wsl.exe and Ubuntu python3/mcopy')
        return None,None
    try:
        paths={'script':packaging_wsl_local_path(script),'folder':packaging_wsl_local_path(payload.parent),
               'payload':packaging_wsl_local_path(payload),'report':packaging_wsl_local_path(payload.parent/'payload.json')}
    except (ValueError,OSError) as error:
        ledger.add('payload-tool-preflight','artifacts','blocked',reason=str(error))
        return None,None
    script_digest=sha(script)
    probe_code=('import hashlib,json,os,shutil,sys;from pathlib import Path;'
        'assert sys.version_info>=(3,11),"Python3.11+ required";'
        'script,folder,payload,report=map(Path,sys.argv[1:5]);'
        'assert script.is_file() and folder.is_dir() and payload.is_file() and report.is_file(),"Local drive mapping unavailable";'
        'digest=hashlib.sha256(script.read_bytes()).hexdigest();'
        'assert digest==sys.argv[5],"WSL script mapping differs from trusted Windows source";'
        'mcopy=shutil.which("mcopy");assert mcopy and os.access(mcopy,os.X_OK),"mcopy absent in Ubuntu";'
        'print("WSL_PAYLOAD_PROBE "+json.dumps({"status":"pass","read_only":True,"script_sha256":digest,'
        '"mcopy":mcopy,"python_version":list(sys.version_info[:3]),"paths":[str(script),str(folder),str(payload),str(report)]},sort_keys=True))')
    probe=[wsl,'--distribution','Ubuntu','--exec','python3','-c',probe_code,
           paths['script'],paths['folder'],paths['payload'],paths['report'],script_digest]
    code,raw=ledger.process('payload-wsl-readonly-preflight',probe,timeout=30)
    # wsl.exe can prepend a UTF-16LE warning to Python's UTF-8 stdout. Keep
    # the raw bytes, discard only orphaned NULs at the next line boundary and
    # require one exact, trusted probe marker plus its full identity checks.
    lines=[line.lstrip(b'\0').rstrip(b'\r\0') for line in raw.read_bytes().split(b'\n')]
    records=[line[len(b'WSL_PAYLOAD_PROBE '):].decode('utf-8') for line in lines
             if line.startswith(b'WSL_PAYLOAD_PROBE ')]
    try:result=json.loads(records[0]) if len(records)==1 else {}
    except ValueError:result={}
    if not isinstance(result,dict):result={}
    valid=(code==0 and result.get('status')=='pass' and result.get('read_only') is True and
           result.get('script_sha256')==script_digest and result.get('paths')==[paths[k] for k in ('script','folder','payload','report')] and
           isinstance(result.get('mcopy'),str) and result['mcopy'].startswith('/'))
    ledger.add('payload-tool-preflight','artifacts','pass' if valid else 'blocked',exit_code=code,raw_log=str(raw),
               command=probe,probe=result,reason=None if valid else 'Existing Ubuntu python3/mcopy or exact local drive mapping could not be verified')
    if not valid:return None,None
    return [wsl,'--distribution','Ubuntu','--exec','python3',paths['script'],paths['folder']],{
        'execution':'windows-wsl-readonly','distribution':'Ubuntu','local_paths':paths,
        'script_sha256':script_digest,'probe_log':str(raw),'probe_command':probe}


def verify_packaging(args,ledger,records):
    required=('kernel','boot_image','firmware','payload','payload_report','pack_report')
    grouped={role:[item for item in records if item.get('role')==role] for role in required}
    if any(len(items)!=1 for items in grouped.values()):
        ledger.add('packaging-identity-chain','artifacts','blocked',reason='Device requires exactly one hashed kernel/boot_image/firmware/payload/payload_report/pack_report and their checked relationships')
        return False
    items={role:entries[0] for role,entries in grouped.items()}
    report=read_json(items['pack_report']['path'])
    expected={
        'sha256':items['boot_image']['sha256'],
        'firmware_sha256':items['firmware']['sha256'],
        'payload_sha256':items['payload']['sha256'],
        'payload_report_sha256':items['payload_report']['sha256'],
        'kernel_sha256':items['kernel']['sha256'],
    }
    if any(report.get(key)!=value for key,value in expected.items()):
        ledger.add('packaging-identity-chain','artifacts','blocked',reason='Pack report does not bind the exact boot image, firmware, payload, payload report and checked kernel',expected=expected)
        return False
    binding=report.get('hotload_binding',{})
    if (report.get('debug_hotload_enabled') is not True or
        binding.get('kernel_sha256')!=expected['kernel_sha256'] or
        binding.get('gateway_source_sha256')!=ledger.data.get('gateway_source_sha256') or
        binding.get('linked_call_verification') is not True):
        ledger.add('packaging-identity-chain','artifacts','blocked',reason='Final image report lacks exact verified hotload code/source/call binding')
        return False
    image=Path(items['boot_image']['path'])
    payload=Path(items['payload']['path'])
    # Independently read the actual Android v3/v4 ramdisk range. A boot record
    # for another same-source image cannot substitute for this exact payload.
    with image.open('rb') as handle:
        header=handle.read(4096)
        if len(header)!=4096 or header[:8]!=b'ANDROID!': raise ValueError('Boot artifact is not an Android image')
        kernel_bytes,payload_bytes=struct.unpack_from('<II',header,8)
        version=struct.unpack_from('<I',header,40)[0]
        if version not in (3,4) or payload_bytes!=payload.stat().st_size:
            raise ValueError('Boot image header/payload size differs from verified artifacts')
        offset=4096+((kernel_bytes+4095)//4096)*4096
        if offset+payload_bytes>image.stat().st_size: raise ValueError('Boot payload range exceeds image')
        handle.seek(offset)
        digest=hashlib.sha256()
        remaining=payload_bytes
        while remaining:
            chunk=handle.read(min(remaining,1024*1024))
            if not chunk: raise ValueError('Truncated boot payload')
            digest.update(chunk); remaining-=len(chunk)
    if digest.hexdigest()!=expected['payload_sha256']: raise ValueError('Actual boot image embeds a different payload')
    if payload.name!='payload.img' or Path(items['payload_report']['path'])!=payload.parent/'payload.json':
        ledger.add('packaging-identity-chain','artifacts','blocked',reason='Full payload re-verification requires the canonical payload.img/payload.json artifact folder')
        return False
    command,provenance=packaging_verification_command(args,ledger,payload)
    if command is None:
        ledger.add('packaging-identity-chain','artifacts','blocked',reason='Read-only local FAT/source/link verification tooling is unavailable')
        return False
    code,raw=ledger.process('payload-identity-reverify',command,timeout=180)
    ledger.add('packaging-identity-chain','artifacts','pass' if code==0 else 'fail',raw_log=str(raw),exit_code=code,
               relationships=expected,command=command,provenance=provenance)
    if not code:
        ledger.data['boot_identity']={
            'boot_image_sha256':expected['sha256'],'kernel_sha256':expected['kernel_sha256'],
            'firmware_sha256':expected['firmware_sha256'],'payload_sha256':expected['payload_sha256'],
            'gateway_source_sha256':binding['gateway_source_sha256']}
    return code==0


def module_recorded_path(value,canonical=False):
    """Normalize a foreign absolute path lexically; never access its filesystem."""
    if not isinstance(value,str) or not value or any(c in value for c in ('\0','\n','\r')):
        raise ValueError('Invalid recorded module path')
    text=value.replace('\\','/')
    if text.startswith('//'):
        raise ValueError('UNC/ambiguous module roots are unsupported')
    if re.match(r'^[A-Za-z]:/',text):
        anchor=text[0].upper()+':/'
        remaining=text[3:]
    elif text.startswith('/'):
        anchor='/'
        remaining=text[1:]
    else:
        raise ValueError('Recorded module paths must be absolute')
    parts=[]
    for part in remaining.split('/'):
        if part in ('','.'):continue
        if part=='..':
            if not parts:raise ValueError('Recorded dependency escapes filesystem root')
            parts.pop()
        else:parts.append(part)
    normalized=anchor+'/'.join(parts)
    comparison=text[0].upper()+text[1:] if re.match(r'^[A-Za-z]:/',text) else text
    if canonical and normalized!=comparison.rstrip('/'):
        raise ValueError('Build source/include roots must already be canonical absolute paths')
    return normalized


def module_relative_path(path,root):
    """Segment-exact relation in the recorded path namespace."""
    if path==root:return ''
    prefix=root.rstrip('/')+'/'
    return path[len(prefix):] if path.startswith(prefix) else None


def module_dependency_paths(text):
    text=text.replace('\\\n','').replace('$$','$')
    if not text.startswith('nlh-module:'):raise ValueError('Bad compiler dependency target')
    paths=[module_recorded_path(value) for value in shlex.split(text[len('nlh-module:'):])]
    if not paths:raise ValueError('Empty compiler dependency set')
    return set(paths)


def module_source_layout(args,image,build):
    """Only the official core/GH layouts may relocate to verified local roots."""
    flags=build.get('compile_flags',[])
    if not isinstance(flags,list) or any(not isinstance(flag,str) for flag in flags):
        raise ValueError('Invalid bound compiler flags')
    includes=[]
    for i,flag in enumerate(flags):
        if flag=='-I':
            if i+1==len(flags):raise ValueError('Missing compiler include argument')
            includes.append(module_recorded_path(flags[i+1],True))
        elif flag.startswith('-I') or flag in ('-isystem','-iquote','-idirafter','-include','-imacros') or flag.startswith('--sysroot'):
            raise ValueError('Unofficial compiler include-root spelling')
    if len(includes)<2 or includes[1]!=includes[0].rstrip('/')+'/core':
        raise ValueError('Official kit and kit/core include roots are required')
    old_source=module_recorded_path(build.get('source'),True)
    old_kit=includes[0]
    current_kit=args.kit.resolve()
    core={'xnu-sched':1,'xnu-vm':2,'xnu-vfs':3}
    protocols={'giulia-gpu-proto':('gpu',1),'giulia-wifi-proto':('wifi',2),
               'giulia-audio-proto':('audio',3),'giulia-modem-proto':('modem',4)}
    specification=None
    if image['name'] in core:
        if (len(includes)!=2 or old_source!=old_kit.rstrip('/')+'/core/policy.c' or
                image['kind']!=2 or image['interface_abi']!=0x434f0001 or
                image['state_abi']!=1 or image['dependencies'] or
                flags.count('-DNLH_POLICY_DOMAIN='+str(core[image['name']]))!=1):
            raise ValueError('Core image does not match its exact official source/domain/include layout')
        source=current_kit/'core/policy.c'
        old_device=None
        current_device=None
        core_policy,protocol_policy=True,False
    elif image['name'] in protocols:
        driver,domain=protocols[image['name']]
        specification_path=args.device_root/'hardware/hotload-protocols.json'
        specification=read_json(specification_path)
        if (specification.get('schema')!=1 or specification.get('abi')!=2 or
                specification.get('gateway_abi')!=1 or specification.get('interface_abi')!='0x47480001' or
                specification.get('state_abi')!=1 or specification.get('kind')!='service' or specification.get('dependencies')!=[]):
            raise ValueError('Protocol specification differs from the fixed GH contract')
        candidates=[item for item in specification.get('modules',[]) if
                    item.get('driver')==driver and item.get('domain')==domain and item.get('name')==image['name']]
        if len(candidates)!=1:raise ValueError('Protocol image lacks one exact verified source specification')
        relative_source=candidates[0].get('source')
        if (not isinstance(relative_source,str) or not re.fullmatch(r'ios16/'+driver+r'/[A-Za-z0-9_./-]+\.c',relative_source) or
                any(part in ('','.','..') for part in relative_source.split('/'))):
            raise ValueError('Protocol specification source is not a canonical owned .c path')
        if len(includes)!=4 or not includes[2].endswith('/ios16'):
            raise ValueError('GH images require exactly ios16 and ios16/common extra includes')
        old_device=includes[2][:-len('/ios16')]
        if (not old_device or includes[3]!=old_device+'/ios16/common' or
                old_source!=old_device+'/'+relative_source or image['interface_abi']!=0x47480001 or
                image['state_abi']!=1 or image['kind']!=0 or image['dependencies']):
            raise ValueError('Protocol source/ABI does not match the exact recorded GH device root')
        current_device=args.device_root.resolve()
        source=(current_device/relative_source).resolve()
        if not source.is_relative_to(current_device):raise ValueError('Protocol source escapes current verified device root')
        core_policy,protocol_policy=False,True
    else:
        raise ValueError('Unknown module source layout; relocation is not inferred from basenames')
    link_flags=build.get('link_flags')
    if (not isinstance(link_flags,list) or len(link_flags)!=5 or
            link_flags[:4]!=['--no-undefined','--fatal-warnings','--orphan-handling=error','-T']) or \
            module_recorded_path(link_flags[-1],True)!=old_kit.rstrip('/')+'/module.ld':
        raise ValueError('Bound linker script is not the official kit/module.ld')
    recorded=[('kit',old_kit),('source',old_source.rsplit('/',1)[0])]
    current=[('kit',current_kit),('source',source.parent)]
    if protocol_policy:
        recorded += [('include_0',old_device+'/ios16'),('include_1',old_device+'/ios16/common')]
        current += [('include_0',current_device/'ios16'),('include_1',current_device/'ios16/common')]
    def logical(value):
        normalized=module_recorded_path(value)
        for label,root in recorded:
            relative_name=module_relative_path(normalized,root)
            if relative_name:
                return label+'/'+relative_name
        raise ValueError('Compiler dependency lies outside the exact recorded logical roots: '+normalized)
    relocation={'kind':'official-core' if core_policy else 'official-gh',
                'recorded_source':old_source,'current_source':str(source),
                'roots':[{'logical_root':label,'recorded':directory,'current':str(dict(current)[label])}
                         for label,directory in recorded]}
    if specification is not None:relocation['protocol_spec_sha256']=sha(specification_path)
    return {'source':source,'recorded_source':old_source,'recorded_roots':recorded,'current_roots':dict(current),
            'logical_key':logical,'primary_key':logical(old_source),'core_policy':core_policy,
            'protocol_policy':protocol_policy,'relocation':relocation}


def verify_module(args,path):
    """Reverify real ELF/object/input provenance, not a sidecar success flag."""
    from nlh_format import parse_image,pack_image,KINDS
    builder=runpy.run_path(str(args.kit/'build-module.py'))
    data=path.read_bytes()
    image=parse_image(data)
    report=read_json(path.parent/'module.json')
    if report.get('image_sha256')!=sha(path) or any(report.get(key)!=value for key,value in image.items()):
        raise ValueError('Module image/manifest metadata mismatch')
    build=report.get('build_inputs',{})
    if report.get('build_inputs_sha256')!=json_sha(build) or report.get('source_inputs_sha256')!=json_sha(report.get('source_inputs',{})):
        raise ValueError('Module build/source digest mismatch')
    if build.get('source_inputs')!=report.get('source_inputs') or build.get('kit_inputs')!=report.get('kit_inputs'):
        raise ValueError('Module build identity does not bind source and kit inputs')
    image_kind={value:key for key,value in KINDS.items()}[image['kind']]
    expected_image={key:image[key] for key in ('name','version','interface_abi','state_abi')}
    expected_image.update(kind=image_kind,dependencies=[
        [item['name'],item['interface_abi'],item['minimum_version']] for item in image['dependencies']])
    if (build.get('image')!=expected_image or any(build.get(key)!=report.get(key)
            for key in ('compiler_identity','linker_identity'))):
        raise ValueError('Bound build image/tool identity differs from the module report')
    actual_kit=builder['kit_hashes']()
    if report.get('kit_inputs')!=actual_kit:
        raise ValueError('Module was built with a different current kit/ABI/tool source')
    flags=build.get('compile_flags',[])
    required=('--target=aarch64-none-elf','-ffreestanding','-fPIC','-mgeneral-regs-only','-ffixed-x18','-mno-outline-atomics')
    if any(flag not in flags for flag in required): raise ValueError('Missing required AArch64 compile constraints')
    compile_command=report.get('compile_command',[])
    dependency_command=report.get('dependency_command',[])
    link_command=report.get('link_command',[])
    if (compile_command[:1+len(flags)]!=[build.get('compiler'),*flags] or
        dependency_command[:1+len(flags)]!=[build.get('compiler'),*flags] or
        link_command[:1+len(build.get('link_flags',[]))]!=[build.get('linker'),*build.get('link_flags',[])]):
        raise ValueError('Recorded compiler/dependency/link commands drift from bound build inputs')
    if ('-c' not in compile_command or compile_command[compile_command.index('-c')+1]!=build['source'] or
        not dependency_command or dependency_command[-1]!=build['source']):
        raise ValueError('Actual compile/dependency command does not name the bound source')
    layout=module_source_layout(args,image,build)
    source=layout['source']
    # Keep original command strings bound to build_inputs. Relocation changes
    # no report, command, module bytes or stored compiler artifact.
    compile_tail=compile_command[1+len(flags):]
    dependency_tail=dependency_command[1+len(flags):]
    link_tail=link_command[1+len(build['link_flags']):]
    if (len(compile_tail)!=9 or compile_tail[:4]!=['-MMD','-MT','nlh-module','-MF'] or
            compile_tail[5:]!=['-c',build['source'],'-o',compile_tail[-1]] or
            len(dependency_tail)!=6 or dependency_tail[:4]!=['-MM','-MT','nlh-module','-MF'] or
            dependency_tail[-1]!=build['source'] or len(link_tail)!=3 or link_tail[1]!='-o' or
            link_tail[0]!=compile_tail[-1]):
        raise ValueError('Unofficial compiler/dependency/link command structure')
    recorded_object=module_recorded_path(compile_tail[-1],True)
    stage,separator,name=recorded_object.rpartition('/')
    if (not separator or name!='module.o' or
            module_recorded_path(compile_tail[4],True)!=stage+'/module.d' or
            module_recorded_path(dependency_tail[4],True)!=stage+'/module-before.d' or
            module_recorded_path(link_tail[2],True)!=stage+'/module.elf'):
        raise ValueError('Recorded compiler artifacts do not share the exact official staging root')
    layout['relocation']['artifact_root']={'recorded':stage,'current':str(path.parent.resolve())}
    inputs={}
    for key,digest in report['source_inputs'].items():
        label,relative_name=key.split('/',1)
        directory=layout['current_roots'].get(label)
        if directory is None: raise ValueError('Unknown compiler source root: '+label)
        if (not relative_name or '\\' in relative_name or ':' in relative_name or relative_name.startswith('/') or
                any(part in ('','.','..') for part in relative_name.split('/'))):
            raise ValueError('Noncanonical compiler logical key: '+key)
        old_root=dict(layout['recorded_roots'])[label]
        if layout['logical_key'](old_root.rstrip('/')+'/'+relative_name)!=key:
            raise ValueError('Compiler logical key violates original root precedence: '+key)
        target=(directory/relative_name).resolve()
        if not target.is_relative_to(directory.resolve()) or sha(target)!=digest:
            raise ValueError('Source/header differs from actual module build: '+key)
        inputs[key]=digest
    if (not inputs or sha(source)!=report.get('source_sha256') or
            inputs.get(layout['primary_key'])!=report.get('source_sha256')):
        raise ValueError('Missing or changed primary module source')
    for name,digest in report.get('artifacts',{}).items():
        artifact=(path.parent/name).resolve()
        if artifact.parent!=path.parent.resolve() or sha(artifact)!=digest:
            raise ValueError('Module build artifact mismatch: '+name)
    if not {'module.o','module.elf','module.d','module-before.d',path.name}<=set(report.get('artifacts',{})):
        raise ValueError('Missing actual compiler/link/dependency outputs')
    # Dependency paths may contain compiler-emitted ../ components. Collapse
    # them lexically in the old namespace, then reproduce builder root order.
    # No old CI source/header/staging path is read or required to exist.
    for dependency_file in ('module.d','module-before.d'):
        dependencies=module_dependency_paths((path.parent/dependency_file).read_text())
        if layout['recorded_source'] not in dependencies:
            raise ValueError('Compiler dependency set omits the actual primary module source')
        logical={layout['logical_key'](value) for value in dependencies}
        if logical!=set(inputs):
            raise ValueError('Compiler dependency set does not match source inputs')
    builder['check_object']((path.parent/'module.o').read_bytes())
    code,entry,state=builder['extract_elf']((path.parent/'module.elf').read_bytes())
    kind={value:key for key,value in KINDS.items()}[image['kind']]
    dependencies=[(d['name'],d['interface_abi'],d['minimum_version']) for d in image['dependencies']]
    rebuilt=pack_image(code,entry,state,image['name'],image['version'],image['interface_abi'],image['state_abi'],kind,dependencies)
    if rebuilt!=data: raise ValueError('Image bytes differ from actual verified AArch64 ELF')
    variant=None
    definitions=set(flag[2:] for flag in flags if flag.startswith('-D'))
    core_policy=layout['core_policy']
    protocol_policy=layout['protocol_policy']
    prefix='NLH_' if core_policy else 'GH_' if protocol_policy else None
    if prefix:
        if prefix+'FAIL_START=1' in definitions: variant='fail-start'
        elif prefix+'FAIL_IMPORT=1' in definitions: variant='fail-import'
        elif prefix+'FAIL_STOP=1' in definitions: variant='fail-stop'
    return {'image':image,'sha256':sha(path),'module_manifest_sha256':sha(path.parent/'module.json'),
            'core_policy':core_policy,'protocol_policy':protocol_policy,
            'source_inputs_sha256':json_sha(inputs),'build_inputs_sha256':json_sha(build),'variant':variant,
            'path_relocation':layout['relocation'],
            'build_provenance_simulation':isinstance(report.get('relocation_simulation'),dict) and
                report['relocation_simulation'].get('simulation') is True,
            'bytes':data}


def select(response, where):
    rows = response if isinstance(response,list) else response['rows'] if isinstance(response,dict) and isinstance(response.get('rows'),list) else [response]
    matches = [row for row in rows if all(row.get(k)==v for k,v in where.items())]
    if len(matches)!=1: raise ValueError(f'Expected one response row matching {where}; got {len(matches)}')
    return matches[0]


def assertions(response, checks, previous):
    evidence=[]
    for check in checks:
        actual=select(response,check.get('where',{})).get(check['field'])
        baseline=check.get('baseline')
        expected=(select(previous[baseline['step']],baseline.get('where',{})).get(baseline.get('field',check['field']))
                  if baseline else check.get('value'))
        op=check.get('op','eq')
        if actual is None or expected is None: raise ValueError('Assertion field/baseline missing')
        predicates={'eq':lambda a,b:a==b,'ne':lambda a,b:a!=b,'gt':lambda a,b:a>b,
                    'ge':lambda a,b:a>=b,'lt':lambda a,b:a<b,'le':lambda a,b:a<=b}
        if op not in predicates or not predicates[op](actual,expected):
            raise AssertionError(f'{check}: actual={actual}, expected={expected}')
        evidence.append({'actual':actual,'operator':op,'expected':expected,'field':check['field']})
    return evidence


class DeviceCoverage:
    """Attribute evidence to one module instance; no native attestation API.

    The caller owns the clean-registry/frozen-upload causal chain. A root list
    identity is bracketed around counters, because neither list inflight counts
    nor the gateway's cumulative counters directly attest an image digest.
    """
    def __init__(self):
        self.active,self.old,self.counts,self.failures={},{},{},{}
        self.unloaded,self.quarantined,self.closed=set(),set(),set()
        self.retired,self.quarantine_identity,self.snapshots={},{},{}
        self.step_operations,self.step_order={},{}
        self.events,self.rejected_claims=[],[]

    @staticmethod
    def descriptor(name):
        if name in CORE_MODULES: return ('core',name,CORE_MODULES[name])
        for index,domain in enumerate(DOMAINS,1):
            if name==GH_MODULES[domain]: return ('gh',domain,index)
        return None

    @classmethod
    def target(cls,step,implicit=None):
        """An explicit GH target is required; a row is never its own target."""
        name=step.get('coverage_module')
        domain=step.get('coverage_domain')
        if domain is not None:
            if isinstance(domain,int) and not isinstance(domain,bool) and 1<=domain<=4:
                actual=name or implicit
                if actual in CORE_MODULES:
                    if CORE_MODULES[actual]!=domain: raise ValueError('Core coverage domain/module mismatch')
                    domain=actual.removeprefix('xnu-')
                else: domain=DOMAINS[domain-1]
            if domain in DOMAINS:
                mapped=GH_MODULES[domain]
            elif domain in ('sched','vm','vfs'):
                mapped='xnu-'+domain
            else: raise ValueError('Unknown coverage_domain')
            if name is not None and name!=mapped:
                raise ValueError('coverage_domain and coverage_module name different targets')
            name=mapped
        if name is not None and cls.descriptor(name) is None:
            raise ValueError('Unknown coverage_module')
        if implicit and name is not None and name!=implicit:
            raise ValueError('Coverage target differs from the actual operation module')
        return name or implicit

    @staticmethod
    def one(rows,name):
        if not isinstance(rows,list): return None
        selected=[row for row in rows if row.get('name')==name]
        return selected[0] if len(selected)==1 else None

    def identity(self,name,rows,active=None,states=(1,)):
        active=active or self.active.get(name)
        row=self.one(rows,name)
        if not active or not row or row.get('state') not in states: return None
        image=active['module'].get('image',{})
        if not {'interface_abi','state_abi','kind','dependencies'}<=set(image): return None
        if (row.get('active')!=active['version'] or row.get('generation')!=active['generation'] or
            row.get('interface')!=image['interface_abi'] or row.get('state_abi')!=image['state_abi'] or
            row.get('kind')!=image['kind'] or row.get('deps')!=len(image['dependencies']) or
            not isinstance(row.get('slot'),int) or not 0<=row['slot']<16 or
            not isinstance(row.get('image_cookie'),int) or row['image_cookie']<=0 or
            not isinstance(row.get('activation_epoch'),int) or row['activation_epoch']<=0 or
            row.get('blocked')!=(0 if row['state']==1 else 1)):
            return None
        return {'module':name,'scope':self.descriptor(name)[0] if self.descriptor(name) else 'other',
                'image_sha256':active['module']['sha256'],'version':active['version'],
                'image_cookie':row['image_cookie'],'generation':row['generation'],
                'activation_epoch':row['activation_epoch'],'slot':row['slot'],
                'digest_attribution':'frozen-upload-and-root-list-causal-chain; not device digest attestation'}

    def stable(self,name,window,active=None,states=(1,)):
        if not window: return None
        before=self.identity(name,window.get('before'),active,states)
        after=self.identity(name,window.get('after'),active,states)
        return before if before and before==after else None

    def emit(self,step,name,claim,identity,source,**details):
        if not identity or not self.descriptor(name): return
        event={'step':step['id'],'module':name,'claim':claim,'identity':dict(identity),
               'source':source,'hardware_verified':False,**details}
        self.events.append(event)

    def claims_for_step(self,step_id):
        return {event['claim'] for event in self.events if event['step']==step_id}

    def observe(self,step,response,module,previous,identity_window=None):
        operation=step['operation']
        self.step_operations[step['id']]=operation
        self.step_order[step['id']]=len(self.step_order)
        window=identity_window or {}
        before=None
        name=module['image']['name'] if module else step.get('name')
        error=response.get('error',0) if isinstance(response,dict) else 0
        if name: before=self.active.get(name)
        target=self.target(step,name)
        pre=self.identity(name,window.get('before'),before) if name else None
        if before and pre: before['identity']=pre
        if operation=='load':
            if not error and response.get('state')==1 and response.get('active')==module['image']['version']:
                active={'version':response['active'],'generation':response.get('generation'),
                    'count':None,'module':module,'migration_count_verified':False}
                self.active[name]=active
                post=self.identity(name,window.get('after'),active)
                active['identity']=post
                self.emit(step,name,'upload',post,'framework-management')
                if pre and post and response.get('generation',0)>before['generation'] and response['active']>before['version']:
                    self.old[name]=dict(before)
                    self.emit(step,name,'replace',post,'framework-management',previous_identity=pre)
                self.unloaded.discard(name); self.failures.pop(name,None)
            elif error==5 and before and response.get('state')==1 and response.get('active')==before['version'] and response.get('generation')==before['generation']:
                if (module.get('core_policy') or module.get('protocol_policy')) and module.get('variant') in ('fail-start','fail-import'):
                    post=self.identity(name,window.get('after'),before)
                    # Rollback restarts the old image: cookie/generation remain,
                    # activation_epoch advances. A stale old window is retired.
                    if (pre and post and post['image_cookie']==pre['image_cookie'] and
                        post['activation_epoch']>pre['activation_epoch']):
                        claim='start_failure' if module['variant']=='fail-start' else 'import_failure'
                        self.emit(step,name,claim,post,'framework-management',previous_identity=pre,
                                  candidate_image_sha256=module['sha256'])
                        self.failures[name]={'identity':post,'variant':module['variant']}
                        before['identity']=post
        elif operation=='call' and before and not error and response.get('state')==1 and response.get('active')==before['version'] and response.get('generation')==before['generation']:
            identity=self.stable(name,window,before)
            failure=self.failures.get(name)
            if identity and failure and failure['identity']==identity:
                self.emit(step,name,'rollback',identity,'framework-management')
            if identity and before['module'].get('core_policy'):
                if step.get('value')==0:
                    old=self.old.get(name)
                    if (old and old.get('identity') and old.get('count',0)>0 and
                        response.get('result',-1)>=old['count']):
                        before['migration_count_verified']=True
                    self.counts[name]=response['result']; before['count']=response['result']
                if step.get('value')==2 and response.get('result',0)>=1 and before.get('migration_count_verified'):
                    self.emit(step,name,'state_migration',identity,'core-private-telemetry',
                              previous_identity=self.old[name]['identity'],migrated_nonzero_count=self.old[name]['count'])
        elif operation=='unload' and before:
            if not error and response.get('state')==0 and response.get('active')==0:
                row=self.one(window.get('after'),name)
                if pre and row and row.get('state')==0 and row.get('active')==0 and row.get('image_cookie')==0 and row.get('blocked')==1:
                    self.emit(step,name,'unload',pre,'framework-management')
                    self.retired[name]=pre
                self.unloaded.add(name); self.active.pop(name,None)
                self.old.pop(name,None); self.counts.pop(name,None); self.failures.pop(name,None)
            elif error==stop_failure_errno(name) and step.get('stop_failure') and response.get('state')==3 and response.get('active')==before['version'] and before['module'].get('variant')=='fail-stop':
                self.quarantined.add(name)
                post=self.identity(name,window.get('after'),before,(3,))
                row=self.one(window.get('after'),name)
                if pre and post==pre and row.get('calls')==0:
                    self.quarantine_identity[name]=pre; self.closed.add(name)
                    self.emit(step,name,'quarantine_retained',pre,'framework-management',retained_slot=row)
        if operation=='list':
            for row in response:
                name=row.get('name')
                active=self.active.get(name,{})
                if (name in self.quarantined and row.get('state')==3 and row.get('blocked')==1 and
                    row.get('active')==active.get('version') and row.get('generation')==active.get('generation') and
                    row.get('calls')==0 and row.get('image_cookie',0)>0):
                    identity=self.identity(name,response,active,(3,))
                    if identity and identity==self.quarantine_identity.get(name): self.closed.add(name)
        if operation=='diagnostic' and isinstance(response,dict) and response.get('scope')=='synthetic':
            fixture_name={'audio.proto':GH_MODULES['audio'],'modem.proto':GH_MODULES['modem']}.get(step.get('command'))
            if fixture_name:
                self.target(step,fixture_name)
                identity=self.stable(fixture_name,window)
                summary=response.get('summary') or next((r for r in response.get('rows',[]) if 'cases' in r),{})
                # Client validates complete case/bitmap summaries. Recheck the
                # source and successful module-call count here, independently
                # of any provider-looking cached fields in a controller fixture.
                complete=response.get('complete') is True
                if step['command']=='audio.proto':
                    count=summary.get('adopted',0)
                    complete &= (summary.get('synthetic')==1 and summary.get('hardware_verified')==0 and
                                 summary.get('counter_exact')==1 and summary.get('cases')==20 and
                                 summary.get('passed_mask')==0xfffff and summary.get('failed_mask')==0)
                else:
                    count=summary.get('module_delta',0)
                    complete &= (summary.get('source')=='fixture' and summary.get('cases')==22 and
                                 summary.get('passed')==22 and summary.get('failed')==0 and
                                 summary.get('native_delta')==0 and summary.get('hardware_event')==0)
                if complete and identity and isinstance(count,int) and count>0:
                    self.emit(step,fixture_name,'module_execution',identity,'fixture',
                              fixture_command=step['command'],calls=count,fixture_owner_epoch=summary.get('epoch'),
                              production_output_adoption='not_claimed',fix_semantics='not_claimed')
        if operation in ('core','gateway'):
            snapshot={}
            for row in response:
                name=row.get('name'); descriptor=self.descriptor(name)
                if not descriptor or row.get('domain')!=descriptor[2]: continue
                if len([r for r in response if r.get('name')==name or r.get('domain')==row.get('domain')])!=1: continue
                identity=self.stable(name,window)
                retired=self.retired.get(name) or self.quarantine_identity.get(name)
                if not identity and retired:
                    states=(0,) if name in self.unloaded else (3,)
                    before_row=self.one(window.get('before'),name); after_row=self.one(window.get('after'),name)
                    if before_row and before_row==after_row and before_row.get('state') in states and before_row.get('blocked')==1:
                        if name in self.unloaded and before_row.get('active')==0 and before_row.get('image_cookie')==0:
                            identity=retired
                        elif name in self.closed and self.stable(name,window,states=(3,))==retired:
                            identity=retired
                if operation=='gateway':
                    if (row.get('busy')!=0 or row.get('poisoned')!=0 or
                        not isinstance(row.get('high_epoch'),int) or row['high_epoch']<0 or not identity or
                        any(row.get(field)!=identity[key] for field,key in
                            (('image_version','version'),('image_cookie','image_cookie'),
                             ('generation','generation'),('activation_epoch','activation_epoch')))):
                        identity=None
                if identity:
                    snapshot[name]={'identity':identity,'row':dict(row),
                                    'owner_epoch':row.get('high_epoch') if operation=='gateway' else None}
            self.snapshots[step['id']]=snapshot
            for check in step.get('assert',[]):
                baseline=check.get('baseline')
                if not baseline or self.step_operations.get(baseline['step'])!=operation: continue
                row=select(response,check.get('where',{}))
                name=row.get('name'); domain=row.get('domain')
                descriptor=self.descriptor(name)
                if not descriptor or descriptor[2]!=domain or check.get('where',{}).get('domain')!=domain: continue
                if operation=='gateway' and target!=name:
                    self.rejected_claims.append({'step':step['id'],'module':name,'reason':'GH counter has no matching explicit coverage target'})
                    continue
                if operation=='core' and target is not None and target!=name: continue
                baseline_row=select(previous[baseline['step']],baseline.get('where',{}))
                if (baseline_row.get('name')!=name or baseline_row.get('domain')!=domain or
                    baseline.get('field',check['field'])!=check['field']): continue
                current=snapshot.get(name); old=self.snapshots.get(baseline['step'],{}).get(name)
                if not current or not old or current['identity']!=old['identity'] or current['owner_epoch']!=old['owner_epoch']:
                    self.rejected_claims.append({'step':step['id'],'module':name,'reason':'Missing, changed or unbracketed image/owner epoch'})
                    continue
                if any(not isinstance(row.get(k),int) or not isinstance(baseline_row.get(k),int) or row[k]<baseline_row[k] for k in ('calls','fallbacks')): continue
                identity=current['identity']
                if (check.get('field')=='calls' and check.get('op')=='gt' and row['calls']>baseline_row['calls'] and
                    name in self.active and name not in self.closed):
                    claim='real_execution' if operation=='core' else 'module_execution'
                    self.emit(step,name,claim,identity,'core-resident-policy' if operation=='core' else 'resident-unattributed',
                              calls_delta=row['calls']-baseline_row['calls'],owner_epoch=current['owner_epoch'],
                              counter_snapshot='approximate cumulative counters; same-instance bracket')
                if name in self.unloaded|self.closed and check.get('field')=='fallbacks' and check.get('op')=='gt':
                    frozen=any(c.get('field')=='calls' and c.get('op','eq')=='eq' and c.get('baseline')
                               and c.get('where',{})==check.get('where',{}) and
                               c['baseline'].get('where',{})==baseline.get('where',{}) and
                               c['baseline'].get('field','calls')=='calls' and
                               c['baseline']['step']==baseline['step'] and row['calls']==baseline_row['calls'] and
                               row['fallbacks']>baseline_row['fallbacks']
                               for c in step.get('assert',[]))
                    if frozen:
                        self.emit(step,name,'resident_fallback',identity,'core-resident-policy' if operation=='core' else 'resident-unattributed',
                                  owner_epoch=current['owner_epoch'],fallback_delta=row['fallbacks']-baseline_row['fallbacks'],native_unload_fallback='not_claimed')
                        if name in self.closed:
                            self.emit(step,name,'stop_failure_isolation',identity,'framework-management',owner_epoch=current['owner_epoch'])
        return self.claims_for_step(step['id'])

    def report(self):
        """Only same-module events enter its record; union is informational."""
        def module_record(name):
            descriptor=self.descriptor(name)
            events=[event for event in self.events if event['module']==name]
            lifecycle={claim:{'status':'pass' if any(e['claim']==claim for e in events) else 'blocked',
                'evidence':[e for e in events if e['claim']==claim]} for claim in REQUIRED}
            identities={}
            for event in events:
                key=json_sha(event['identity'])
                item=identities.setdefault(key,{'identity':event['identity'],'events':[]})
                item['events'].append(event)
            execution=[event for event in events if event['claim'] in ('module_execution','real_execution')]
            claims={field:{'status':'not_claimed' if field in ('production_output_adoption','fix_semantics') else 'blocked',
                'evidence':[],'reason':'No implemented domain-specific native/adoption/functional attestation input'} for field in CLAIMS}
            claims['module_execution']={'status':'pass' if execution else 'blocked','evidence':execution,
                'allowed_sources':['fixture','resident-unattributed'] if descriptor[0]=='gh' else ['core-resident-policy'],
                'implies_production_adoption':False,'implies_native_input':False,'implies_hardware_function':False}
            return {'module':name,'domain':descriptor[2],'scope':descriptor[0],'status':'blocked',
                    **claims,'lifecycle':lifecycle,'identities':identities,
                    'completion_reason':'Native input/adoption/unload fallback/function evidence is absent' if descriptor[0]=='gh' else 'Per-core device lifecycle requirements are evaluated independently'}
        domains={domain:module_record(name) for domain,name in GH_MODULES.items()}
        core={name:module_record(name) for name in CORE_MODULES}
        for item in core.values():
            item['status']='pass' if all(row['status']=='pass' for row in item['lifecycle'].values()) else 'blocked'
        return {'schema':'giulia-hotload-device-evidence-v2','authoritative_completion':'per-module/per-domain only',
                'per_domain':domains,'core_framework':{'status':'pass' if all(r['status']=='pass' for r in core.values()) else 'blocked','modules':core},
                'summary':{'authoritative':False,'union':sorted({event['claim'] for event in self.events}),
                           'warning':'A global union cannot satisfy any module or hardware domain'},
                'rejected_claims':list(self.rejected_claims),'native_pass_interface_implemented':False}

    def publish(self,ledger):
        report=self.report()
        ledger.data['device_evidence']=report
        ledger.data['per_domain']=report['per_domain']
        ledger.data['core_framework']=report['core_framework']
        ledger.data['device_coverage']=report['summary']['union']
        ledger.data['device_coverage_authoritative']=False
        ledger.data['device_domains']=[]


def finalize_device_evidence(ledger,tracker=None):
    if tracker is not None: tracker.publish(ledger)
    elif 'device_evidence' not in ledger.data: DeviceCoverage().publish(ledger)
    existing={step['id'] for step in ledger.steps}
    for domain in DOMAINS:
        if domain+'-hardware' not in existing:
            ledger.add(domain+'-hardware','device','blocked',reason='This domain lacks native input, production adoption, same-domain unload fallback and hardware function evidence')
    for name,item in ledger.data['core_framework']['modules'].items():
        if item['status']!='pass' and 'core-'+name+'-device' not in existing:
            ledger.add('core-'+name+'-device','device_core_framework','blocked',reason='Per-module core lifecycle requirements are incomplete; union coverage is non-authoritative')


def stop_failure_errno(name):
    # Wi-Fi deliberately retains private admission and reports EBUSY; the
    # other reviewed modules report EIO. Both require actual quarantine proof.
    return 16 if name=='giulia-wifi-proto' else 5


def device_mutation(step):
    return step.get('operation') in ('load','call','unload') or (
        step.get('operation')=='diagnostic' and step.get('command') in ('audio.proto','modem.proto'))


def device(args, ledger, artifact_ready):
    from hotload import open_session, DeviceError, UnknownOutcome, DiagnosticBlocked, DIAGNOSTICS
    tracker=DeviceCoverage()
    tracker.publish(ledger)
    if not args.execute_device:
        ledger.add('device-suite','device','blocked',reason='Device execution not selected; no port opened')
        for domain in DOMAINS:
            ledger.add(domain+'-hardware','device','blocked',reason='Requires new resident kernel, real native provider, hardware preconditions and domain execution/fallback counters')
        return
    if not args.plan or not args.port or not artifact_ready:
        ledger.add('device-preconditions','device','blocked',reason='Requires --plan, --port and passing new artifact/source/link binding; no port opened')
        return
    plan=read_json(args.plan)
    if plan.get('schema') not in ('giulia-hotload-device-plan-v1','giulia-hotload-device-plan-v2'):
        raise ValueError('Unsupported device plan schema')
    boot=plan.get('boot_record',{})
    path=relative(args.plan.parent,boot.get('path',''))
    if not path.is_file() or sha(path)!=boot.get('sha256'):
        ledger.add('boot-record','device','blocked',reason='Missing/hash-mismatched actual temporary-boot record; no port opened'); return
    record=read_json(path)
    identity=ledger.data.get('boot_identity',{})
    if record.get('status')!='pass' or not identity or any(record.get(key)!=value for key,value in identity.items()):
        ledger.add('boot-record','device','blocked',reason='Actual temporary-boot record must match exact checked boot image/kernel/firmware/payload/source identity; no port opened'); return
    steps=plan.get('steps',[])
    ids=[step.get('id') for step in steps]
    if any(not re.fullmatch('[a-z0-9_-]+',name or '') for name in ids) or len(set(ids))!=len(ids): raise ValueError('Unique lowercase step IDs required')
    mutations=[i for i,step in enumerate(steps) if device_mutation(step)]
    stops=[i for i,step in enumerate(steps) if step.get('stop_failure')]
    if len(stops)>1 or (stops and (not mutations or stops[0]!=mutations[-1])):
        raise ValueError('STOP failure must be the final mutation; only read-only evidence may follow')
    if stops and (steps[stops[0]].get('operation')!='unload' or
            steps[stops[0]].get('expect_error')!=stop_failure_errno(steps[stops[0]].get('name'))):
        raise ValueError('STOP failure requires the reviewed module-specific unload error')
    frozen={}
    planned_active={}
    for step_index,step in enumerate(steps):
        if step.get('operation') not in ('status','memory','list','core','gateway','load','call','unload','diagnostic'):
            raise ValueError('Unsupported device operation; no port opened')
        if step.get('operation')=='diagnostic' and step.get('command') not in DIAGNOSTICS:
            raise ValueError('Diagnostic is outside the fixed cached/synthetic allowlist; no port opened')
        if step.get('operation')=='load':
            image_path=str(relative(args.plan.parent,step['image']))
            module=args.verified_modules.get(image_path)
            if not module or sha(Path(image_path))!=module['sha256']:
                raise ValueError('Plan load is absent from the verified/frozen suite: '+image_path)
            frozen[step['id']]=module
            DeviceCoverage.target(step,module['image']['name'])
            if not step.get('expect_error'):
                planned_active[module['image']['name']]=module
        if step.get('operation')=='unload':
            planned=planned_active.get(step.get('name'),{})
            if planned.get('variant')=='fail-stop':
                if (not step.get('stop_failure') or step.get('expect_error')!=stop_failure_errno(step.get('name')) or
                    not mutations or step_index!=mutations[-1]):
                    raise ValueError('Verified fail-stop unload requires explicit final-mutation isolation plan')
            if not step.get('expect_error'): planned_active.pop(step.get('name'),None)
        if step.get('operation') in ('call','unload'):
            DeviceCoverage.target(step,step.get('name'))
        elif step.get('operation')=='diagnostic' and step.get('command') in ('audio.proto','modem.proto'):
            DeviceCoverage.target(step,GH_MODULES[step['command'].split('.')[0]])
        else:
            DeviceCoverage.target(step)
        if any(requirement not in PLAN_COVERAGE for requirement in step.get('coverage',[])):
            raise ValueError('Unknown coverage requirement')
    # Frozen bytes, rather than re-reading mutable image paths, are uploaded.
    # This also prevents a plan from substituting another module after preflight.
    previous={}
    uncertain=False
    with open_session(args.port,ledger.output/'device-session.log',args.timeout,args.lock_timeout) as client:
        ledger.data['device_accessed']=True
        status=client.status()
        required={'abi':2,'provider':1,'platform_abi':1,'resident_dispatch':1,'busy':0,'slots':0,'token':0}
        if any(status.get(key)!=value for key,value in required.items()):
            ledger.add('running-kernel-bridge','device','blocked',response=status,reason='Running kernel lacks new resident provider/ABI/dispatch or is busy; no upload issued'); return
        ledger.add('running-kernel-bridge','device','pass',response=status)
        gateway=client.query('gateway')
        source_rows=[row for row in gateway if row.get('source_sha256')]
        expected_source=ledger.data.get('gateway_source_sha256')
        if (len(source_rows)!=1 or not isinstance(source_rows[0].get('source_sha256'),str) or
            not re.fullmatch('[0-9a-f]{64}',source_rows[0]['source_sha256']) or
            source_rows[0]['source_sha256']!=expected_source):
            ledger.add('running-source-identity','device','blocked',response=gateway,reason='Live gateway source marker does not match the verified new manifest; no upload issued'); return
        ledger.add('running-source-identity','device','pass',source_sha256=expected_source)
        for step in steps:
            if uncertain and device_mutation(step):
                ledger.add(step['id'],'device','blocked',reason='Actual quarantine/recovery uncertainty forbids further mutation regardless of plan labels')
                break
            raw=ledger.output/('device-'+step['id']+'.log')
            old_log=client.wire.log
            with raw.open('xb') as log:
                class Tee:
                    def write(self,data): log.write(data); old_log.write(data)
                    def flush(self): log.flush(); old_log.flush()
                client.wire.log=Tee()
                try:
                    operation=step['operation']
                    identity_window={}
                    bracket=(operation in ('load','call','unload','core','gateway') or
                             operation=='diagnostic' and step.get('command') in ('audio.proto','modem.proto'))
                    if bracket:
                        identity_window['before']=client.query('list')
                        if any(row.get('state')==3 or row.get('recovery',0) for row in identity_window['before']):
                            uncertain=True
                        if uncertain and device_mutation(step):
                            ledger.add(step['id'],'device','blocked',reason='Root list exposed quarantine/recovery uncertainty before mutation',identity_window=identity_window,raw_log=str(raw))
                            break
                    try:
                        if operation in ('status','memory'): response=getattr(client,operation)()
                        elif operation in ('list','core','gateway'): response=client.query(operation)
                        elif operation=='load': response=client.load_bytes(frozen[step['id']]['bytes'])
                        elif operation=='call': response=client.call(step['name'],step['value'])
                        elif operation=='unload': response=client.unload(step['name'])
                        elif operation=='diagnostic': response=client.diagnostic(step['command'])
                        else: raise ValueError('Unsupported device operation')
                    except DeviceError as error:
                        response=error.response
                        if response.get('state')==3 or response.get('recovery',0): uncertain=True
                        if not step.get('expect_error'): raise
                    if isinstance(response,dict) and (response.get('state')==3 or response.get('recovery',0)):
                        uncertain=True
                    actual_error=response.get('error',0) if isinstance(response,dict) else 0
                    if actual_error!=step.get('expect_error',0):
                        raise AssertionError('Unexpected operation error')
                    if bracket:
                        identity_window['after']=client.query('list')
                        if any(row.get('state')==3 or row.get('recovery',0) for row in identity_window['after']):
                            uncertain=True
                    if operation=='list' and any(row.get('state')==3 or row.get('recovery',0) for row in response):
                        uncertain=True
                    checks=step.get('assert',[])
                    evidence=assertions(response,checks,previous)
                    previous[step['id']]=response
                    proven=tracker.observe(step,response,frozen.get(step['id']),previous,identity_window)
                    tracker.publish(ledger)
                    unproven=set(step.get('coverage',[]))-proven
                    if unproven:
                        ledger.add(step['id']+'-coverage','device','blocked',reason='Operation cannot prove requested behavior: '+','.join(sorted(unproven)))
                    if step.get('hardware_domain'):
                        if step['hardware_domain'] not in DOMAINS: raise ValueError('Invalid hardware domain')
                        ledger.add(step['id']+'-hardware','device','blocked',reason='This transport runner has no native-provider/readiness/hardware-operation attestation; gateway/global provider or inflight counts never establish hardware pass')
                    scope='device_protocol_fixture' if operation=='diagnostic' and step['command'].endswith('.proto') else 'device'
                    ledger.add(step['id'],scope,'pass',operation=operation,response=response,assertions=evidence,
                               raw_log=str(raw),hardware_verified=False,identity_window=identity_window)
                except DiagnosticBlocked as error:
                    ledger.add(step['id'],'device','blocked',reason=str(error),response=error.response,raw_log=str(raw))
                except UnknownOutcome as error:
                    ledger.add(step['id'],'device','blocked',reason=str(error),resolution=error.resolution,raw_log=str(raw)); break
                except (OSError,ValueError,RuntimeError,TimeoutError,AssertionError,KeyError) as error:
                    ledger.add(step['id'],'device','fail',reason=str(error),raw_log=str(raw)); break
                finally: client.wire.log=old_log
        if tracker.quarantined or uncertain:
            ledger.add('post-isolation','device','blocked',reason='STOP uncertainty retained after read-only evidence; no mutating retry/reboot. Reviewed normal temporary-boot recovery required')
    finalize_device_evidence(ledger,tracker)


def main():
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--kit',type=Path,default=Path(__file__).resolve().parents[2]/'UniXNU/kits/NativeHotload')
    p.add_argument('--bridge',type=Path,default=Path(__file__).resolve().parents[1]/'ios16/terminal/giulia_hotload_bridge.c')
    p.add_argument('--device-root',type=Path,default=Path(__file__).resolve().parents[1])
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--host-runtime',action='store_true')
    p.add_argument('--cc',default='cc')
    p.add_argument('--manifest',type=Path)
    p.add_argument('--kernel',type=Path)
    p.add_argument('--kernel-sha256')
    p.add_argument('--module-suite',type=Path)
    p.add_argument('--linked-report',type=Path)
    p.add_argument('--plan',type=Path)
    p.add_argument('--port')
    p.add_argument('--execute-device',action='store_true')
    p.add_argument('--timeout',type=float,default=30)
    p.add_argument('--lock-timeout',type=float,default=0)
    args=p.parse_args()
    for name in ('kit','bridge','device_root','output','manifest','kernel','module_suite','linked_report','plan'):
        value=getattr(args,name)
        if value: setattr(args,name,value.resolve())
    if not 0<args.timeout<=3600: p.error('--timeout must be 0..3600 seconds')
    args.output.mkdir(parents=True,exist_ok=False)
    ledger=Ledger(args.output)
    sys.path.insert(0,str(args.kit))
    try:
        if args.host_runtime:
            code,raw=ledger.process('transport-execute',[sys.executable,str(args.kit/'tests/verify_transport.py')],timeout=30)
            collect_cases(ledger,raw,'host_transport',6)
            ledger.add('transport-execute','host_transport','pass' if code==0 else 'fail',exit_code=code,raw_log=str(raw))
            host_runtime(args,ledger)
        else: ledger.add('host-runtime','host_runtime','blocked',reason='Host runtime execution not selected')
        ready=artifacts(args,ledger)
        device(args,ledger,ready)
    except (OSError,ValueError,RuntimeError,KeyError,AssertionError,TypeError,SyntaxError) as error:
        ledger.add('runner-error','automation','fail',reason=str(error))
    finalize_device_evidence(ledger)
    ledger.flush()
    print(json.dumps({'status':ledger.data['status'],'result':str(args.output/'result.json')},sort_keys=True))
    return {'pass':0,'fail':1,'blocked':2}[ledger.data['status']]


if __name__=='__main__': raise SystemExit(main())
