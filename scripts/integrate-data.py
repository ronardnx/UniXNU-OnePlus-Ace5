"""Attach the verified APFS Data seed to a copy of a qualified XNU FAT payload.

This integrates loader handoff, not automatic /private/var mounting. Preserves
the original kernel and baseline payload; writes a new payload and UEFI identity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import tomllib
import zlib
from ios16_platform import read_nodes
from ios16_payload_budget import MIB, fat_geometry, file_budget, loader_budget

ROOT=Path(__file__).resolve().parents[1]
def digest(path):
    with path.open('rb') as f: return hashlib.file_digest(f,'sha256').hexdigest()
def require(ok,message):
    if not ok: raise SystemExit(message)

def data_adt(blob):
    """Add only the Data handoff; retain the source platform and resources."""
    original=read_nodes(blob)
    nodes={path:dict(properties) for path,properties in original.items()}
    chosen=nodes.get('/device-tree/chosen')
    require(chosen is not None,'Base ADT has no chosen node')
    memory=nodes.setdefault('/device-tree/chosen/memory-map',{'name':b'memory-map\0'})
    require(not any(key in memory for key in ('RAMDisk','PackedRAMDisk')),
            'Base ADT already has a ramdisk handoff')
    require(chosen.get('ephemeral-storage',struct.pack('<I',1))==struct.pack('<I',1),
            'Base ADT has a conflicting Data storage policy')
    chosen['ephemeral-storage']=struct.pack('<I',1)
    memory['RAMDisk']=bytes(16)

    def encode(path):
        properties=nodes[path]
        children=[name for name in nodes if name.rsplit('/',1)[0]==path]
        result=struct.pack('<II',len(properties),len(children))
        for key,value in properties.items():
            result+=struct.pack('<32sI',key.encode('ascii'),len(value))+value+bytes(-len(value)%4)
        return result+b''.join(encode(child) for child in children)

    result=encode('/device-tree')
    require(read_nodes(result)==nodes,'Data handoff changed unrelated ADT properties')
    return result

p=argparse.ArgumentParser()
p.add_argument('--base',type=Path,required=True)
p.add_argument('--seed',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--identity',type=Path,required=True)
args=p.parse_args()
base=json.loads((args.base/'payload.json').read_text())
seed=json.loads((ROOT/'hardware/data-seed.json').read_text())
require(digest(args.base/'payload.img')==base['payload_sha256'],'Base payload hash mismatch')
payload=(args.base/'payload.img').read_bytes()
geometry=fat_geometry(payload)
require(len(payload)==base['payload_bytes'] and base.get('payload_geometry',geometry)==geometry,
        'Base payload size/geometry mismatch')
require(base.get('payload_crc32',f'{zlib.crc32(payload):08x}')==f'{zlib.crc32(payload):08x}',
        'Base payload CRC32 mismatch')
require('payload_mib' not in base or base['payload_mib']*MIB==len(payload),
        'Base payload capacity mismatch')
require(args.seed.stat().st_size==seed['bytes'] and digest(args.seed)==seed['sha256'],'Data seed hash/size mismatch')
require(not base.get('data_ramdisk_sha256'),'Base payload already has a Data seed')
require(not args.out.exists(),'Refusing to overwrite existing output directory')
require(args.identity.resolve() not in ((args.base/name).resolve() for name in ('identity.json','payload.json','payload.img')),
        'Identity output must not replace a base payload input')
require(args.identity.resolve() not in (args.seed.resolve(),(args.out/'payload.img').resolve(),
                                      (args.out/'payload.json').resolve()),
        'Identity output must not replace the seed, payload image or payload report')
output=args.out/'payload.img'
with tempfile.TemporaryDirectory() as directory:
    temp=Path(directory)
    for path in ['Payloads/kernel.macho','Payloads/kernel.macho.adt','unixnu.toml']:
        subprocess.run(['mcopy','-i',str(args.base/'payload.img'),f'::/EFI/UniXNU/{path}',str(temp/Path(path).name)],check=True)
    kernel=temp/'kernel.macho'
    require(digest(kernel)==base['payload_kernel_sha256'],'Embedded kernel hash mismatch')
    with kernel.open('rb') as f: image=f.read()
    adt=temp/'kernel.macho.adt'
    require(digest(adt)==base['adt_sha256'],'Embedded ADT hash mismatch')
    adt.write_bytes(data_adt(adt.read_bytes()))
    trust=None
    if base.get('trust_catalog') or base.get('trust_cache_sha256'):
        trust=temp/'kernel.macho.trustcache'
        subprocess.run(['mcopy','-i',str(args.base/'payload.img'),
            '::/EFI/UniXNU/Payloads/kernel.macho.trustcache',str(trust)],check=True)
        expected=base['trust_catalog']['sha256'] if base.get('trust_catalog') else base['trust_cache_sha256']
        require(digest(trust)==expected,'Embedded trust catalogue hash mismatch')
    cfg=temp/'unixnu.toml';text=cfg.read_text();parsed=tomllib.loads(text)
    entry=next(e for e in parsed['entry'] if e['id']=='xnu')
    targets=[e for e in parsed['entry'] if e.get('type')=='xnu' and e.get('path')==entry['path']]
    require(all('initrd' not in e for e in targets),'Base payload already has an initrd')
    require(entry['path']=='Payloads/kernel.macho','Unexpected kernel payload path')
    marker='path = "Payloads/kernel.macho"\n'
    require(text.count(marker)==len(targets),'Unexpected boot configuration')
    text=text.replace(marker,marker+'initrd = "Payloads/scratch-data.img"\n')
    text=text.replace('Ace 5 XNU first entry (no system volume)','Ace 5 XNU + APFS Data handoff')
    cfg.write_text(text,encoding='utf-8',newline='\n')
    files={'EFI/UniXNU/Payloads/kernel.macho':kernel.stat().st_size,
           'EFI/UniXNU/Payloads/kernel.macho.adt':adt.stat().st_size,
           'EFI/UniXNU/unixnu.toml':cfg.stat().st_size,
           'EFI/UniXNU/Payloads/scratch-data.img':args.seed.stat().st_size}
    if trust:files['EFI/UniXNU/Payloads/kernel.macho.trustcache']=trust.stat().st_size
    allocation=file_budget(files,len(payload),geometry)
    prepared=base.get('dram_handoff')=='darwinboot-allocated-window' or 'loader_budget' in base
    if prepared:
        loader=loader_budget(image,adt.read_bytes(),trust.stat().st_size if trust else 0,args.seed.read_bytes())
        used=loader['used_bytes']
    else:
        # Retain the older first-entry allocation contract for legacy inputs.
        ncmd=struct.unpack_from('<I',image,16)[0];at=32;regions=[]
        for _ in range(ncmd):
            cmd,size=struct.unpack_from('<II',image,at)
            require(size>=8 and at+size<=len(image),'Invalid Mach-O command')
            if cmd==0x19:
                va,vm_size=struct.unpack_from('<QQ',image,at+24)
                if vm_size:regions.append((va,va+vm_size))
            at+=size
        span=max(end for _,end in regions)-min(start for start,_ in regions)
        align=lambda n,a:(n+a-1)//a*a
        used=align(align(align(span,16384)+4096+adt.stat().st_size,4096)+align(seed['bytes'],4096),16384)
        require(used<=0x10000000-0x200000,'Data seed exceeds the DarwinBoot allocation window')
    args.out.mkdir(parents=True)
    shutil.copyfile(args.base/'payload.img',output)
    for source,dest in [(cfg,'unixnu.toml'),(adt,'Payloads/kernel.macho.adt'),(args.seed,'Payloads/scratch-data.img')]:
        subprocess.run(['mcopy','-o','-i',str(output),str(source),f'::/EFI/UniXNU/{dest}'],check=True)
    adt_sha=digest(adt)
    modem=None
    if base.get('modem_contract'):
        from ios16_modem import check_contract, metadata_from_runtime
        modem=check_contract(image,metadata_from_runtime(base['modem_contract']),adt.read_bytes(),
                             {'cellular_policy':base.get('modem_root_policy',{})},base.get('modem_mode'))
payload=output.read_bytes()
require(fat_geometry(payload)==geometry,'Data integration changed source FAT geometry')
crc=f'{zlib.crc32(payload):08x}'
report=dict(base,data_ramdisk_sha256=seed['sha256'],data_volume_uuid=seed['volume_uuid_reported_by_diskutil'],
    adt_sha256=adt_sha,payload_sha256=hashlib.sha256(payload).hexdigest(),payload_bytes=len(payload),
    payload_crc32=crc,payload_geometry=geometry,payload_file_budget=allocation,
    handoff_used_bytes=used,data_transport='dense RAMDisk',data_mounted=False,hardware_tested=False,
    purpose='XNU with APFS Data seed handoff; Data mounting remains unverified')
if prepared:report['loader_budget']=loader
if modem:report['modem_contract']=modem
(args.out/'payload.json').write_text(json.dumps(report,indent=2)+'\n')
identity=dict(report,data_seed_run=seed['run_id'],data_sha256=seed['sha256'],
              kernel_sha256=report['payload_kernel_sha256'])
(args.out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
if args.identity.resolve()!=(args.out/'identity.json').resolve():
    args.identity.write_text(json.dumps(identity,indent=2)+'\n')
print(json.dumps(report,indent=2))
