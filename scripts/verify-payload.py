"""Verify the downloaded FAT payload and its embedded kernel/ADT against the build report."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tomllib
from ios16_platform import check_platform

folder=Path(sys.argv[1]).resolve()
report=json.loads((folder/'payload.json').read_text())
image=folder/'payload.img'
def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f,'sha256').hexdigest()
if image.stat().st_size!=report['payload_bytes'] or digest(image)!=report['payload_sha256']:
    raise SystemExit('FAT payload digest/size mismatch')
with tempfile.TemporaryDirectory() as directory:
    temporary=Path(directory)
    for name in ['kernel.macho','kernel.macho.adt']:
        subprocess.run(['mcopy','-i',str(image),f'::/EFI/UniXNU/Payloads/{name}',str(temporary/name)],check=True)
    subprocess.run(['mcopy','-i',str(image),'::/EFI/UniXNU/unixnu.toml',str(temporary/'unixnu.toml')],check=True)
    if digest(temporary/'kernel.macho')!=report['payload_kernel_sha256']:
        raise SystemExit('Embedded kernel digest mismatch')
    if digest(temporary/'kernel.macho.adt')!=report['adt_sha256']:
        raise SystemExit('Embedded ADT digest mismatch')
    with (temporary/'kernel.macho').open('rb') as f:
        magic,cpu,subtype=struct.unpack('<3I',f.read(12))
    if (magic,cpu,subtype)!=(0xfeedfacf,0x100000c,0):
        raise SystemExit('Unexpected kernel ABI')
    if report.get('platform_match'):
        match=check_platform((temporary/'kernel.macho.adt').read_bytes(),(temporary/'kernel.macho').read_bytes())
        if match!=report['platform_match']:raise SystemExit('Platform binding differs from build report')
        print('Embedded ADT matches the built-in platform and CPU drivers')
    config=tomllib.loads((temporary/'unixnu.toml').read_text())
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
            expected+=' sweetsample=SpringBoard,backboardd,datamigrator,com.apple.datamigrator,com.apple.migrationpluginwrappe'
        if report.get('physical_touch_worker'):
            if not report.get('platform_match',{}).get('gui') or b'GIULIA TOUCH: controller result=' not in (temporary/'kernel.macho').read_bytes():
                raise SystemExit('Ace 5 touch worker missing from the GUI kernel')
            expected+=' giulia_touch=1'
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
    if report.get('data_ramdisk_sha256'):
        if entry.get('initrd')!='Payloads/scratch-data.img':
            raise SystemExit('Missing Data initrd configuration')
        data=temporary/'scratch-data.img'
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
print('Payload digest, embedded ARM64 kernel, ADT, and boot configuration verified')
