"""Build a verified iOS16 kernel/ADT FAT payload for the matching UEFI build."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import tomllib
import zlib

def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--kernel',type=Path,required=True)
    p.add_argument('--adt',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--root-report',type=Path,help='Include HFSX boot entry for an already prepared root')
    p.add_argument('--default-root',action='store_true')
    a=p.parse_args()
    if a.default_root and not a.root_report: p.error('Default root requires a root report')
    if a.out.exists():raise ValueError('Refusing to overwrite output')
    report=json.loads(a.kernel.with_suffix('.json').read_text())
    if report['output_sha256']!=sha(a.kernel) or report['boot_args_bytes']!=736:
        raise ValueError('Kernel handoff verification missing')
    args='-v serial=3 debug=0x14e cpus=1 rd=vermeerufs0 vermeer_usb=1 vermeer_ufs_highdma=1'
    probe=args+' giulia_ufs_probe=1'
    config=('version = 1\n[boot]\ntimeout = 3\ndefault = "xnu"\n[kits]\nautoload = false\n'
            '[[entry]]\nid = "xnu"\ntitle = "Ace 5 iOS16 UFS + USB"\ntype = "xnu"\n'
            'path = "Payloads/kernel.macho"\nargs = '+json.dumps(probe)+'\n')
    if a.root_report:
        root=json.loads(a.root_report.read_text())
        ident=root['hfs_finder_id']
        if root['bytes']!=16*1024**3 or root['block_size']!=4096 or len(ident)!=16 or int(ident,16)==0:
            raise ValueError('Unexpected HFSX root identity')
        config+=('[[entry]]\nid = "ios16"\ntitle = "Ace 5 iOS16 HFSX root"\ntype = "xnu"\n'
                 'path = "Payloads/kernel.macho"\nargs = '+json.dumps(args+f' vermeer_ufs_rw=1 vermeer_root_uuid={ident}')+'\n')
        if a.default_root:config=config.replace('default = "xnu"','default = "ios16"')
    for e in tomllib.loads(config)['entry']:
        if len(e['args'].encode('ascii'))>=608:raise ValueError('Boot line too long')
    a.out.mkdir(parents=True)
    image=a.out/'payload.img'
    env=dict(os.environ,MTOOLS_SKIP_CHECK='1',TZ='UTC')
    with tempfile.TemporaryDirectory() as td:
        stage=Path(td);payload=stage/'EFI/UniXNU/Payloads';payload.mkdir(parents=True)
        shutil.copyfile(a.kernel,payload/'kernel.macho')
        shutil.copyfile(a.adt,payload/'kernel.macho.adt')
        (payload.parent/'unixnu.toml').write_text(config)
        for path in [stage,*stage.rglob('*')]:os.utime(path,(315532800,315532800))
        subprocess.run(['mformat','-C','-F','-T','131072','-v','GIULIAESP','-N','47495531','-i',str(image),'::'],env=env,check=True)
        subprocess.run(['mcopy','-i',str(image),'-s','-Q','-m',str(stage/'EFI'),'::/'],env=env,check=True)
    data=image.read_bytes()
    if data[82:90]!=b'FAT32   ' or struct.unpack_from('<I',data,32)[0]*512!=len(data):raise ValueError('FAT geometry')
    result={'payload_bytes':len(data),'payload_sha256':sha(image),'payload_crc32':f'{zlib.crc32(data):08x}',
            'payload_kernel_sha256':sha(a.kernel),'kernel_sha256':sha(a.kernel),'adt_sha256':sha(a.adt),
            'kernel_args':probe,'dram_handoff':'darwinboot-allocated-window',
            'hardware_tested':False,'purpose':'iOS16 XNU: UFS read probe and native USB console; no filesystem writes'}
    if a.root_report:
        result.update(root_sha256=root['sha256'],root_finder_id=ident,default_entry='ios16' if a.default_root else 'xnu',
                      purpose='iOS16 HFSX root mount and UFS/USB diagnostic entries; use after manual userdata flash')
    (a.out/'payload.json').write_text(json.dumps(result,indent=2)+'\n')
    (a.out/'identity.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
