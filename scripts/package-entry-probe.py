"""Package an audited diagnostic kernel without changing Data or the ADT."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import tomllib
import zlib
from ios16_payload_budget import MIB, fat_geometry, file_budget, loader_budget

p=argparse.ArgumentParser()
p.add_argument('--base',type=Path,required=True)
p.add_argument('--kernel',type=Path,required=True)
p.add_argument('--audit',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--identity',type=Path,required=True)
a=p.parse_args()
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
r=json.loads((a.base/'payload.json').read_text());audit=json.loads(a.audit.read_text())
if sha(a.base/'payload.img')!=r['payload_sha256']:raise SystemExit('Base payload mismatch')
payload=(a.base/'payload.img').read_bytes()
geometry=fat_geometry(payload)
if len(payload)!=r['payload_bytes'] or r.get('payload_geometry',geometry)!=geometry:
    raise SystemExit('Base payload size/geometry mismatch')
if r.get('payload_crc32',f'{zlib.crc32(payload):08x}')!=f'{zlib.crc32(payload):08x}':
    raise SystemExit('Base payload CRC32 mismatch')
if 'payload_mib' in r and r['payload_mib']*MIB!=len(payload):
    raise SystemExit('Base payload capacity mismatch')
if audit['original_sha256']!=r['payload_kernel_sha256'] or sha(a.kernel)!=audit['patched_sha256']:
    raise SystemExit('Kernel probe audit mismatch')
if a.out.exists():raise SystemExit('Refusing to overwrite output directory')
if a.identity.resolve() in ((a.base/name).resolve() for name in ('identity.json','payload.json','payload.img')):
    raise SystemExit('Identity output must not replace a base payload input')
if a.identity.resolve() in (a.kernel.resolve(),a.audit.resolve(),(a.out/'payload.img').resolve(),
                            (a.out/'payload.json').resolve()):
    raise SystemExit('Identity output must not replace the kernel, audit, image or payload report')

image=a.out/'payload.img'
with tempfile.TemporaryDirectory() as directory:
    temp=Path(directory);cfg=temp/'unixnu.toml';adt=temp/'kernel.macho.adt'
    for source,target in [('unixnu.toml',cfg),('Payloads/kernel.macho.adt',adt)]:
        subprocess.run(['mcopy','-i',str(a.base/'payload.img'),f'::/EFI/UniXNU/{source}',str(target)],check=True)
    if sha(adt)!=r['adt_sha256']:raise SystemExit('Base ADT mismatch')
    parsed=tomllib.loads(cfg.read_text())
    entry=next(e for e in parsed['entry'] if e['id']=='xnu')
    if entry.get('path')!='Payloads/kernel.macho':raise SystemExit('Unexpected kernel payload path')
    trust=None
    if r.get('trust_catalog') or r.get('trust_cache_sha256'):
        trust=temp/'kernel.macho.trustcache'
        subprocess.run(['mcopy','-i',str(a.base/'payload.img'),
            '::/EFI/UniXNU/Payloads/kernel.macho.trustcache',str(trust)],check=True)
        expected=r['trust_catalog']['sha256'] if r.get('trust_catalog') else r['trust_cache_sha256']
        if sha(trust)!=expected:raise SystemExit('Base trust catalogue mismatch')
    ramdisk=None
    if r.get('data_ramdisk_sha256'):
        if entry.get('initrd')!='Payloads/scratch-data.img':raise SystemExit('Unexpected Data initrd path')
        ramdisk=temp/'scratch-data.img'
        subprocess.run(['mcopy','-i',str(a.base/'payload.img'),
            '::/EFI/UniXNU/Payloads/scratch-data.img',str(ramdisk)],check=True)
        if sha(ramdisk)!=r['data_ramdisk_sha256']:raise SystemExit('Base Data seed mismatch')
    elif entry.get('initrd'):
        raise SystemExit('Base initrd has no verified Data seed report')
    text=cfg.read_text().replace('Ace 5 XNU + APFS Data handoff','Ace 5 XNU early-entry diagnostics')
    cfg.write_text(text,encoding='utf-8',newline='\n')
    files={'EFI/UniXNU/Payloads/kernel.macho':a.kernel.stat().st_size,
           'EFI/UniXNU/Payloads/kernel.macho.adt':adt.stat().st_size,
           'EFI/UniXNU/unixnu.toml':cfg.stat().st_size}
    if trust:files['EFI/UniXNU/Payloads/kernel.macho.trustcache']=trust.stat().st_size
    if ramdisk:files['EFI/UniXNU/Payloads/scratch-data.img']=ramdisk.stat().st_size
    allocation=file_budget(files,len(payload),geometry)
    prepared=r.get('dram_handoff')=='darwinboot-allocated-window' or 'loader_budget' in r
    if prepared:
        loader=loader_budget(a.kernel.read_bytes(),adt.read_bytes(),trust.stat().st_size if trust else 0,
                             ramdisk.read_bytes() if ramdisk else b'')
        r['loader_budget']=loader
        if 'handoff_used_bytes' in r:r['handoff_used_bytes']=loader['used_bytes']
    if r.get('modem_contract'):
        from ios16_modem import check_contract, metadata_from_runtime
        r['modem_contract']=check_contract(a.kernel.read_bytes(),metadata_from_runtime(r['modem_contract']),
            adt.read_bytes(),{'cellular_policy':r.get('modem_root_policy',{})},r.get('modem_mode'))
    a.out.mkdir(parents=True);shutil.copyfile(a.base/'payload.img',image)
    subprocess.run(['mcopy','-o','-i',str(image),str(a.kernel),'::/EFI/UniXNU/Payloads/kernel.macho'],check=True)
    subprocess.run(['mcopy','-o','-i',str(image),str(cfg),'::/EFI/UniXNU/unixnu.toml'],check=True)
payload=image.read_bytes()
if fat_geometry(payload)!=geometry:raise SystemExit('Probe packaging changed source FAT geometry')
r.update(payload_kernel_sha256=audit['patched_sha256'],kernel_sha256=audit['patched_sha256'],
         payload_sha256=hashlib.sha256(payload).hexdigest(),payload_bytes=len(payload),
         payload_crc32=f'{zlib.crc32(payload):08x}',payload_geometry=geometry,payload_file_budget=allocation,
         purpose='XNU entry/MMU/panic diagnostics with unchanged Data seed',entry_probe=audit,
         panic_screen=audit['panic_screen'],hardware_tested=False)
(a.out/'payload.json').write_text(json.dumps(r,indent=2)+'\n')
identity=dict(r)
if r.get('data_ramdisk_sha256'):identity['data_sha256']=r['data_ramdisk_sha256']
(a.out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
if a.identity.resolve()!=(a.out/'identity.json').resolve():
    a.identity.write_text(json.dumps(identity,indent=2)+'\n')
print(json.dumps(identity,indent=2))
