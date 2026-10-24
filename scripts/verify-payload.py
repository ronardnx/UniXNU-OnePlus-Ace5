"""Verify the downloaded FAT payload and its embedded kernel/ADT against the build report."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import sys
import tempfile
import tomllib
import zlib
from ios16_platform import check_platform, read_nodes
from ios16_wifi import check_wifi_platform
from ios16_wifi_runtime import check_runtime
from ios16_gpu_runtime import check_runtime as check_gpu_runtime
from ios16_audio_runtime import check_runtime as check_audio_runtime
from ios16_modem import check_contract as check_modem_contract, metadata_from_runtime
from ios16_gpu_power import check_request as check_gpu_power_request
from ios16_gpu_gunyah import check_request as check_gpu_gunyah_request
from ios16_trust_catalog import modules
from ios16_payload_budget import MIB, fat_geometry, file_budget, loader_budget

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('folder',type=Path)
parser.add_argument('--payload',type=Path,help='Exact FAT image; defaults to folder/payload.img')
parser.add_argument('--payload-report',type=Path,help='Exact payload report; defaults to folder/payload.json')
parser.add_argument('--report',type=Path,help='New machine-readable verification report')
arguments=parser.parse_args()
if arguments.report and arguments.report.exists():parser.error('--report must be a new file')
folder=arguments.folder.resolve()
report_path=(arguments.payload_report or folder/'payload.json').resolve()
report_bytes=report_path.read_bytes()
report=json.loads(report_bytes)
image=(arguments.payload or folder/'payload.img').resolve()
if type(report.get('debug_hotload_enabled',False)) is not bool:
    raise SystemExit('Payload hotload enable field must be a boolean')
if report.get('debug_hotload_enabled') and (type(report.get('schema')) is not int or report['schema']!=1 or report.get('status')!='pass'):
    raise SystemExit('Hotload payload requires the passing schema1 source-bound report')
def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f,'sha256').hexdigest()

def verify_hotload_sources(kernel,binding):
    from ios16_hotload import check_hotload_kernel
    if not isinstance(binding,dict) or binding.get('source_snapshot')!='hotload-binding/snapshot.json':
        raise SystemExit('Hotload requires its external, source-only snapshot')
    snapshot_root=folder/'hotload-binding'
    if snapshot_root.is_symlink():raise SystemExit('Hotload source snapshot root cannot be a symlink')
    snapshot_path=snapshot_root/'snapshot.json'
    if digest(snapshot_path)!=binding.get('source_snapshot_sha256'):
        raise SystemExit('Hotload source snapshot descriptor digest mismatch')
    snapshot=json.loads(snapshot_path.read_text(encoding='utf-8'))
    fixed={'schema':1,'status':'pass','source_manifest':'source-manifest.json',
           'linked_report':'linked-report.json','kit_root':'kit','device_root':'device',
           'proprietary_firmware_included':False}
    if not isinstance(snapshot,dict) or any(type(snapshot.get(k)) is not type(v) or snapshot[k]!=v for k,v in fixed.items()):
        raise SystemExit('Unsupported hotload source snapshot schema/layout')
    files=snapshot.get('files')
    if not isinstance(files,dict) or not files:
        raise SystemExit('Hotload snapshot lacks exact input hashes')
    for name,sha in files.items():
        if not isinstance(name,str) or not isinstance(sha,str) or not re.fullmatch('[0-9a-f]{64}',sha):
            raise SystemExit('Invalid hotload snapshot input identity')
        relative=PurePosixPath(name);path=snapshot_root/name
        if (relative.is_absolute() or '..' in relative.parts or str(relative)!=name or
                '\\' in name or ':' in name or path.is_symlink() or
                not path.resolve().is_relative_to(snapshot_root.resolve())):
            raise SystemExit('Unsafe hotload snapshot input path')
        if relative.suffix not in ('.h','.c','.cpp','.inc','.py','.json','.ld','.md','.sh') and relative.name!='LICENSE':
            raise SystemExit('Hotload snapshot contains a non-source input')
        data=path.read_bytes()
        if b'\0' in data or hashlib.sha256(data).hexdigest()!=sha:
            raise SystemExit('Hotload source snapshot input changed: '+name)
        data.decode('utf-8')
    inventory={p.relative_to(snapshot_root).as_posix() for p in snapshot_root.rglob('*') if p.is_file()}
    if inventory!=set(files)|{'snapshot.json'} or any(p.is_symlink() for p in snapshot_root.rglob('*')):
        raise SystemExit('Hotload snapshot contains unrecorded files/symlinks')
    manifest=json.loads((snapshot_root/'source-manifest.json').read_text(encoding='utf-8'))
    expected={'source-manifest.json':binding.get('source_manifest_sha256'),
              'linked-report.json':binding.get('linked_source_report_sha256'),
              'verifier/check-ios16-hotload-kernel.py':binding.get('verifier_sha256')}
    for name,sha in manifest['files'].items():expected['kit/'+name]=sha
    for name,sha in manifest['device_files'].items():expected['device/'+name]=sha
    for name in ('make-ios16-payload.py','verify-payload.py','pack-payload.py','build-ios16-wifi-bringup.sh'):
        expected['tooling/'+name]=files.get('tooling/'+name)
    if expected!=files:
        raise SystemExit('Hotload snapshot must exactly cover manifest, linked report, kit and device source closure')
    linked=json.loads((snapshot_root/'linked-report.json').read_text(encoding='utf-8'))
    checked=check_hotload_kernel(kernel,linked,manifest=snapshot_root/'source-manifest.json',
        source=snapshot_root/'kit',device_root=snapshot_root/'device')
    if (set(binding)!=set(checked)|{'linked_source_report_sha256','source_snapshot','source_snapshot_sha256'} or
            any(type(binding.get(k)) is not type(v) or binding[k]!=v for k,v in checked.items()) or
            any(snapshot.get(k)!=checked[k] for k in ('kernel_sha256','gateway_source_sha256','gateway_selected'))):
        raise SystemExit('Embedded hotload code/source/call identity differs from payload binding')
    # Recheck after the linked-code walk; do not certify concurrently modified
    # source snapshots or an exchanged descriptor.
    if digest(snapshot_path)!=binding['source_snapshot_sha256'] or any(digest(snapshot_root/name)!=sha for name,sha in files.items()):
        raise SystemExit('Hotload source snapshot changed during verification')
    return binding

def use_hotload_resource_snapshot(kernel,binding):
    """Use trusted checker code with frozen data roots, not snapshot code."""
    import ios16_wifi_runtime
    import ios16_gpu_runtime
    import ios16_audio_runtime
    import ios16_modem
    frozen=folder/'hotload-binding/device'
    selected=binding['gateway_selected']
    checkers=(('wifi',ios16_wifi_runtime,b'GIULIA WIFI: PCIe/MHI/QMI/WMI ready;'),
              ('gpu',ios16_gpu_runtime,b'GIULIA GPU: host preparation complete;'),
              ('audio',ios16_audio_runtime,b'GIULIA AUDIO: compiled runtime disabled;'),
              ('modem',ios16_modem,ios16_modem.RUNTIME_MARKER))
    for name,module,marker in checkers:
        if marker in kernel and name not in selected:
            raise SystemExit('Hotload source manifest omits a compiled hardware runtime: '+name)
        if name in selected:
            # Their pin readers resolve data relative to __file__. Retain the
            # already imported, supported implementation; execute no .py from
            # an artifact. The target script and pins were hashed above.
            module.__file__=str(frozen/'scripts'/('ios16_'+name+'_runtime.py' if name!='modem' else 'ios16_modem.py'))
    if 'modem' in selected:
        original_binding=ios16_modem.build_binding
        def frozen_modem_binding(root=None):
            return original_binding(frozen if root is None else root)
        ios16_modem.build_binding=frozen_modem_binding
if image.stat().st_size!=report['payload_bytes'] or digest(image)!=report['payload_sha256']:
    raise SystemExit('FAT payload digest/size mismatch')
payload=image.read_bytes()
geometry=fat_geometry(payload)
crc=f'{zlib.crc32(payload):08x}'
if report.get('payload_crc32',crc)!=crc:
    raise SystemExit('FAT payload CRC32 differs from build report')
if 'payload_mib' in report and (report['payload_mib'] not in range(64,225,16) or
                                report['payload_mib']*MIB!=len(payload)):
    raise SystemExit('Requested FAT capacity differs from actual image length')
if 'payload_geometry' in report and report['payload_geometry']!=geometry:
    raise SystemExit('FAT allocation geometry differs from build report')
identity_path=folder/'identity.json'
if identity_path.is_file():
    identity=json.loads(identity_path.read_text())
    if any(identity.get(key)!=report.get(key) for key in
           ('payload_bytes','payload_sha256','payload_crc32')):
        raise SystemExit('UEFI payload identity differs from the payload report')
with tempfile.TemporaryDirectory() as directory:
    temporary=Path(directory)
    trust_path=None
    ramdisk_path=None
    for name in ['kernel.macho','kernel.macho.adt']:
        subprocess.run(['mcopy','-i',str(image),f'::/EFI/UniXNU/Payloads/{name}',str(temporary/name)],check=True)
    subprocess.run(['mcopy','-i',str(image),'::/EFI/UniXNU/unixnu.toml',str(temporary/'unixnu.toml')],check=True)
    if digest(temporary/'kernel.macho')!=report['payload_kernel_sha256']:
        raise SystemExit('Embedded kernel digest mismatch')
    actual_kernel_sha256=digest(temporary/'kernel.macho')
    if 'kernel_sha256' in report and report['kernel_sha256']!=actual_kernel_sha256:
        raise SystemExit('Kernel identity alias differs from the actual FAT kernel')
    if report.get('debug_hotload_enabled'):
        verified_binding=verify_hotload_sources((temporary/'kernel.macho').read_bytes(),report.get('hotload_binding'))
        use_hotload_resource_snapshot((temporary/'kernel.macho').read_bytes(),verified_binding)
    if digest(temporary/'kernel.macho.adt')!=report['adt_sha256']:
        raise SystemExit('Embedded ADT digest mismatch')
    wifi=check_wifi_platform(read_nodes((temporary/'kernel.macho.adt').read_bytes()))
    if wifi!=report.get('wifi_bringup'):
        raise SystemExit('Wi-Fi platform declaration differs from build report')
    if wifi:
        if not report.get('root_finder_id') or not report.get('complete_boot_logging'):
            raise SystemExit('Wi-Fi service bring-up requires a root entry and full logging')
        print('WCN7850 platform declaration verified')
    runtime_report=report.get('wifi_runtime')
    runtime_meta={'wifi_runtime_included':bool(runtime_report),
                  'wifi_firmware':runtime_report.get('firmware') if runtime_report else None}
    runtime=check_runtime((temporary/'kernel.macho').read_bytes(),runtime_meta)
    if runtime!=runtime_report:
        raise SystemExit('Wi-Fi runtime/firmware/display declaration differs from the embedded kernel')
    if runtime:print('Compiled Wi-Fi runtime, pinned firmware segment and native-panel implementation verified; hardware validation pending')
    gpu_report=report.get('gpu_runtime')
    gpu_meta={'gpu_runtime_included':bool(gpu_report),
              'gpu_firmware':gpu_report.get('firmware') if gpu_report else None}
    gpu_runtime=check_gpu_runtime((temporary/'kernel.macho').read_bytes(),gpu_meta)
    if gpu_runtime!=gpu_report:
        raise SystemExit('GPU firmware/service binding differs from the embedded kernel')
    audio_report=report.get('audio_runtime')
    audio_meta={'audio_runtime_included':bool(audio_report),
                'audio_firmware':audio_report.get('firmware') if audio_report else None}
    audio_runtime=check_audio_runtime((temporary/'kernel.macho').read_bytes(),audio_meta)
    if audio_runtime!=audio_report:
        raise SystemExit('Audio firmware/coordinator binding differs from the embedded kernel')
    modem_report=report.get('modem_contract')
    modem_actual=check_modem_contract((temporary/'kernel.macho').read_bytes(),
        metadata_from_runtime(modem_report),(temporary/'kernel.macho.adt').read_bytes(),
        {'cellular_policy':report.get('modem_root_policy',{})},report.get('modem_mode'))
    if modem_actual!=modem_report:
        raise SystemExit('Modem runtime/ADT/root policy differs from the embedded payload')
    if report.get('modem_mode') and (not report.get('root_finder_id') or not report.get('platform_match',{}).get('gui')):
        raise SystemExit('Modem mode requires a matching GUI root')
    if report.get('audio_prepare') and (not audio_runtime or not report.get('root_finder_id') or
                                      not report.get('platform_match',{}).get('gui')):
        raise SystemExit('Audio preparation requires the verified GUI root and audio firmware')
    if report.get('gpu_mode') and (report['gpu_mode'] not in (1,2,3) or not gpu_runtime or
                                  not report.get('root_finder_id') or not report.get('platform_match',{}).get('gui')):
        raise SystemExit('GPU mode requires the verified GUI root and GPU firmware')
    gpu_power=check_gpu_power_request((temporary/'kernel.macho.adt').read_bytes(),
        report.get('gpu_mode'),(temporary/'kernel.macho').read_bytes())
    if gpu_power!=report.get('gpu_power_handoff'):
        raise SystemExit('GPU power request differs from the embedded ADT/kernel/report')
    gpu_gunyah=check_gpu_gunyah_request((temporary/'kernel.macho.adt').read_bytes(),
        report.get('gpu_mode'),(temporary/'kernel.macho').read_bytes())
    if gpu_gunyah!=report.get('gpu_gunyah_handoff'):
        raise SystemExit('Gunyah boot request differs from the embedded ADT/kernel/report')
    if report.get('wifi_pcie_handoff') and not wifi:
        raise SystemExit('PCIe handoff lacks the verified Qualcomm Wi-Fi platform')
    if report.get('wifi_stock_handoff') and not report.get('wifi_cesta_handoff'):
        raise ValueError('Stock PCIe initialization requires the CESTA resource handoff')
    if report.get('wifi_cesta_handoff') and (not report.get('wifi_pcie_handoff') or report.get('wifi_pcie_report_version')!=5):
        raise SystemExit('CESTA resources require the version-5 PCIe handoff')
    with (temporary/'kernel.macho').open('rb') as f:
        magic,cpu,subtype=struct.unpack('<3I',f.read(12))
    if (magic,cpu,subtype)!=(0xfeedfacf,0x100000c,0):
        raise SystemExit('Unexpected kernel ABI')
    if report.get('platform_match'):
        match=check_platform((temporary/'kernel.macho.adt').read_bytes(),(temporary/'kernel.macho').read_bytes())
        if match!=report['platform_match']:raise SystemExit('Platform binding differs from build report')
        print('Embedded ADT matches the built-in platform and CPU drivers')
    config=tomllib.loads((temporary/'unixnu.toml').read_text())
    debug_args=report['kernel_args'].split()
    if bool(report.get('debug_terminal_enabled')) != ('giulia_debug=1' in debug_args):
        raise SystemExit('Debug terminal boot argument/report mismatch')
    if bool(report.get('debug_mmio_enabled')) != ('giulia_mmio=1' in debug_args):
        raise SystemExit('MMIO boot argument/report mismatch')
    if report.get('debug_mmio_enabled') and not report.get('debug_terminal_enabled'):
        raise SystemExit('MMIO requires debug terminal mode')
    if bool(report.get('debug_hotload_enabled')) != ('unixnu_hotload=1' in debug_args):
        raise SystemExit('Hotload boot argument/report mismatch')
    if report.get('debug_hotload_enabled'):
        if not report.get('debug_terminal_enabled'):
            raise SystemExit('Hotload requires debug terminal mode')
    if bool(report.get('wifi_terminal_wait')) != ('giulia_wifi_wait=1' in debug_args):
        raise SystemExit('Wi-Fi terminal wait/report mismatch')
    if report.get('wifi_terminal_wait') and (not report.get('debug_terminal_enabled') or
        not report.get('wifi_pcie_handoff') or
        b'GIULIA WIFI: awaiting wifi.start before DMA setup' not in (temporary/'kernel.macho').read_bytes()):
        raise SystemExit('Wi-Fi wait requires a matching runtime and debug terminal')
    if report.get('debug_terminal_enabled') and b'GIULIA SHELL: /dev/giulia-debug ready;' not in (temporary/'kernel.macho').read_bytes():
        raise SystemExit('Missing debug terminal kernel implementation')
    entry=next(e for e in config['entry'] if e['id']=='xnu')
    if entry['type']!='xnu' or entry['args']!=report['kernel_args']:
        raise SystemExit('Unexpected boot entry')
    if report.get('root_finder_id'):
        roots=[e for e in config['entry'] if e['id']=='ios16']
        expected=report['kernel_args'].replace(' giulia_ufs_probe=1','')
        expected+=' vermeer_ufs_rw=1 vermeer_root_uuid='+report['root_finder_id']
        if report.get('ios_platform_launch_compat'):
            expected+=' unentitled_ios_sim_launch=1'
            if b'unentitled_ios_sim_launch\0' not in (temporary/'kernel.macho').read_bytes():
                raise SystemExit('iOS platform-launch option is not supported or not bound to the root entry')
        if report.get('launchd_job_cache_compat'):
            expected+=' launchd_unsecure_cache=1'
        if report.get('platform_match',{}).get('gui'):
            expected+=' sweetfb_hz=30 sweetfb_noconsole=1'
        if report.get('trace_services'):
            if not report.get('platform_match',{}).get('gui') or b'sweetsample\0' not in (temporary/'kernel.macho').read_bytes():
                raise SystemExit('Service sampler is not present in the GUI kernel')
            names=report.get('service_sample_names')
            if names is None:
                names=report['root_kernel_args'].split(' sweetsample=',1)[1].split()[0]
            if names not in (
                '*',
                'SpringBoard,backboardd,lsd,containermanager,installd,installcoordinat,accountsd,securityd,cfprefsd,appstored',
                'SpringBoard,backboardd,com.apple.datami,com.apple.migrat,appstored,installd,installcoordinat,accountsd,securityd,cfprefsd',
                'SpringBoard,backboardd,com.apple.datami,com.apple.migrat,appstored,installd,installcoordinat',
                'SpringBoard,backboardd,com.apple.datami,com.apple.migrat',
                'SpringBoard,backboardd,datamigrator,com.apple.datamigrator,com.apple.migrationpluginwrappe'):
                raise SystemExit('Unrecognized service-sampling selector set')
            expected+=' sweetsample='+names
        if report.get('service_stderr_trace'):
            if not report.get('trace_services'):raise SystemExit('Service stderr tracing requires service sampling')
            stderr_names=report.get('service_stderr_names','SpringBoard,backboardd,com.apple.datamigrator,com.apple.migrationpluginwrappe')
            if stderr_names not in (
                '*',
                'SpringBoard,backboardd,com.apple.datamigrator,com.apple.migrationpluginwrappe',
                'SpringBoard,backboardd,lsd,containermanagerd,installd,installcoordinationd,accountsd,securityd'):
                raise SystemExit('Unrecognized service-stderr selector set')
            expected+=' sweetnull='+stderr_names
        if report.get('complete_boot_logging'):
            if (report.get('service_sample_names')!='*' or report.get('service_stderr_names')!='*'
                    or b'GIULIA LOG: launchd full activity/stderr logging enabled;' not in (temporary/'kernel.macho').read_bytes()):
                raise SystemExit('Complete log profile/kernel mismatch')
            expected+=' giulia_bootlog=1'
        if report.get('physical_touch_worker'):
            if not report.get('platform_match',{}).get('gui') or b'GIULIA TOUCH: controller result=' not in (temporary/'kernel.macho').read_bytes():
                raise SystemExit('Ace 5 touch worker missing from the GUI kernel')
            expected+=' giulia_touch=1'
        if report.get('touch_gpio'):
            if not report.get('physical_touch_worker') or b'GIULIA TOUCH: GPIO SPI mode 0 active;' not in (temporary/'kernel.macho').read_bytes():
                raise SystemExit('GPIO SPI worker missing from the kernel')
            expected+=' giulia_touch_gpio=1'
        if report.get('gpu_mode'):expected+=' giulia_gpu='+str(report['gpu_mode'])
        if report.get('audio_prepare'):expected+=' giulia_audio=1'
        if report.get('physical_slider_worker'):
            if (not report.get('platform_match',{}).get('gui') or
                not report.get('hall_prepare_handoff') or
                b'GIULIA SLIDER: handoff-missing gh=' not in (temporary/'kernel.macho').read_bytes()):
                raise SystemExit('Guarded AK09970 worker/native Hall handoff missing from the GUI payload')
            expected+=' giulia_slider=1'
        elif report.get('hall_prepare_handoff'):
            raise SystemExit('Hall preparation requires the explicit slider worker profile')
        if report.get('modem_mode'):expected+=' giulia_modem='+str(1 if report['modem_mode']=='diagnostic' else 2)
        if report.get('root_kernel_args',expected)!=expected:
            raise SystemExit('Root compatibility options differ from build report')
        if (len(roots)!=1 or roots[0]['type']!='xnu' or roots[0]['path']!=entry['path']
                or roots[0]['args']!=expected or config['boot']['default']!=report['default_entry']):
            raise SystemExit('Root entry/default/volume identity mismatch')
        print('Root mount entry, explicit write enable, volume identity and default selection verified')
    if report.get('dram_handoff'):
        blob=(temporary/'kernel.macho.adt').read_bytes()
        nodes={}
        def read_node(at,parent,depth=0):
            if depth>64 or at+8>len(blob):raise SystemExit('Invalid ADT node')
            properties,children=struct.unpack_from('<II',blob,at);at+=8
            props={}
            for _ in range(properties):
                if at+36>len(blob):raise SystemExit('Truncated ADT property')
                key,length=struct.unpack_from('<32sI',blob,at);at+=36
                key=key.rstrip(b'\0').decode()
                if key in props or at+length>len(blob):raise SystemExit('Invalid ADT property')
                props[key]=blob[at:at+length];at+=(length+3)&~3
            path=parent+'/'+props['name'].rstrip(b'\0').decode()
            if path in nodes:raise SystemExit('Duplicate ADT node')
            nodes[path]=props
            for _ in range(children):at=read_node(at,path,depth+1)
            return at
        if read_node(0,'')!=len(blob):raise SystemExit('Invalid ADT length')
        chosen=nodes['/device-tree/chosen']
        if report['dram_handoff']!='darwinboot-allocated-window' or any(
                chosen.get(key)!=bytes(8) for key in ['dram-base','dram-size']):
            raise SystemExit('Missing zero-initialized Darwin22 DRAM handoff placeholders')
        memory=struct.unpack('<Q',chosen['darwinboot-memory-size'])[0]
        if not 0x10000000<=memory<=0x200000000 or memory&0x1fffff:
            raise SystemExit('Invalid DarwinBoot memory window')
        print('Darwin22 DRAM placeholders and requested memory window verified')
        placeholder=nodes.get('/device-tree/chosen/memory-map',{}).get('TrustCache')
        if bool(report.get('trust_catalog')) != (placeholder is not None):
            raise SystemExit('Trust catalogue/ADT handoff mismatch')
        if report.get('trust_catalog'):
            if placeholder!=bytes(16):raise SystemExit('TrustCache handoff must start at zero')
            cache=temporary/'kernel.macho.trustcache'
            trust_path=cache
            subprocess.run(['mcopy','-i',str(image),'::/EFI/UniXNU/Payloads/kernel.macho.trustcache',str(cache)],check=True)
            trust=report['trust_catalog']; parsed=modules(cache.read_bytes())
            if digest(cache)!=trust['sha256'] or cache.stat().st_size!=trust['bytes']:
                raise SystemExit('Embedded trust catalogue digest/size mismatch')
            if len({h for m in parsed for h in m['hashes']})!=trust['unique_hashes']:
                raise SystemExit('Embedded trust catalogue inventory mismatch')
            if b'GIULIA AMFI: trust catalogue ready;' not in (temporary/'kernel.macho').read_bytes():
                raise SystemExit('Read-only trust query provider missing')
            print('Official trust catalogue and bounded loader handoff verified')
    if report.get('data_ramdisk_sha256'):
        if entry.get('initrd')!='Payloads/scratch-data.img':
            raise SystemExit('Missing Data initrd configuration')
        data=temporary/'scratch-data.img'
        ramdisk_path=data
        subprocess.run(['mcopy','-i',str(image),'::/EFI/UniXNU/Payloads/scratch-data.img',str(data)],check=True)
        if digest(data)!=report['data_ramdisk_sha256']:
            raise SystemExit('Embedded Data digest mismatch')
        blob=(temporary/'kernel.macho.adt').read_bytes()
        nodes={}
        def walk(at,parent):
            properties,children=struct.unpack_from('<II',blob,at);at+=8
            props={}
            for _ in range(properties):
                key,length=struct.unpack_from('<32sI',blob,at);at+=36
                if at+length>len(blob): raise SystemExit('Truncated ADT')
                props[key.rstrip(b'\0').decode()]=blob[at:at+length];at+=(length+3)&~3
            path=parent+'/'+props['name'].rstrip(b'\0').decode();nodes[path]=props
            for _ in range(children):at=walk(at,path)
            return at
        if walk(0,'')!=len(blob): raise SystemExit('Invalid ADT length')
        if nodes['/device-tree/chosen/memory-map'].get('RAMDisk')!=bytes(16):
            raise SystemExit('Missing zero-initialized RAMDisk handoff placeholder')
        if nodes['/device-tree/chosen'].get('ephemeral-storage')!=struct.pack('<I',1):
            raise SystemExit('Missing volatile Data declaration')
        print('Data seed, initrd configuration and RAMDisk ADT handoff verified')
    if trust_path is None and report.get('trust_cache_sha256'):
        trust_path=temporary/'kernel.macho.trustcache'
        subprocess.run(['mcopy','-i',str(image),'::/EFI/UniXNU/Payloads/kernel.macho.trustcache',str(trust_path)],check=True)
        if digest(trust_path)!=report['trust_cache_sha256']:
            raise SystemExit('Legacy trust cache digest differs from build report')
    files={'EFI/UniXNU/Payloads/kernel.macho':(temporary/'kernel.macho').stat().st_size,
           'EFI/UniXNU/Payloads/kernel.macho.adt':(temporary/'kernel.macho.adt').stat().st_size,
           'EFI/UniXNU/unixnu.toml':(temporary/'unixnu.toml').stat().st_size}
    if trust_path:files['EFI/UniXNU/Payloads/kernel.macho.trustcache']=trust_path.stat().st_size
    if ramdisk_path:files['EFI/UniXNU/Payloads/scratch-data.img']=ramdisk_path.stat().st_size
    allocation=file_budget(files,len(payload),geometry)
    if 'payload_file_budget' in report and report['payload_file_budget']!=allocation:
        raise SystemExit('FAT file allocation budget differs from the extracted payload')
    if report.get('dram_handoff')=='darwinboot-allocated-window' or 'loader_budget' in report:
        loader=loader_budget((temporary/'kernel.macho').read_bytes(),
                             (temporary/'kernel.macho.adt').read_bytes(),
                             trust_path.stat().st_size if trust_path else 0,
                             ramdisk_path.read_bytes() if ramdisk_path else b'')
        if 'loader_budget' in report and report['loader_budget']!=loader:
            raise SystemExit('DarwinBoot loader budget differs from the extracted kernel/ADT/trust/ramdisk')
        print('FAT capacity and Darwin22 file/span/16 KiB handoff bounds verified; contiguous allocation requires device validation')
    else:
        print('FAT file capacity verified for the legacy payload contract')
print('Payload digest, embedded ARM64 kernel, ADT, and boot configuration verified')
if (report_path.read_bytes()!=report_bytes or digest(image)!=report['payload_sha256']):
    raise SystemExit('Payload/report changed during verification')
verification={'schema':1,'status':'pass','verified':True,'payload_verification':True,
              'payload_sha256':report['payload_sha256'],
              'payload_report_sha256':hashlib.sha256(report_bytes).hexdigest(),
              'kernel_sha256':actual_kernel_sha256,'payload_kernel_sha256':actual_kernel_sha256,
              'debug_hotload_enabled':report.get('debug_hotload_enabled',False),'hardware_tested':False}
if report.get('debug_hotload_enabled'):verification['hotload_binding']=report['hotload_binding']
if arguments.report:
    arguments.report.parent.mkdir(parents=True,exist_ok=True)
    with arguments.report.open('x',encoding='utf-8',newline='\n') as stream:stream.write(json.dumps(verification,indent=2)+'\n')
print(json.dumps(verification,sort_keys=True))
