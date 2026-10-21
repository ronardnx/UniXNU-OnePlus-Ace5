"""Verify the downloaded FAT payload and its embedded kernel/ADT against the build report."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tomllib

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
    config=tomllib.loads((temporary/'unixnu.toml').read_text())
    entry=next(e for e in config['entry'] if e['id']=='xnu')
    if entry['type']!='xnu' or entry['args']!=report['kernel_args']:
        raise SystemExit('Unexpected boot entry')
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
