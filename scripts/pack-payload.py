"""Pack a verified Android-v4 UEFI image with a verified GIULIAESP FAT payload.

Only writes a new host file. Does not flash or invoke adb/fastboot.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import gzip
import subprocess
import tempfile
import zlib

def require(value, message):
    if not value:
        raise SystemExit(message)

def sha(data):
    return hashlib.sha256(data).hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--firmware', type=Path, required=True)
    p.add_argument('--firmware-report', type=Path, required=True)
    p.add_argument('--payload', type=Path, required=True)
    p.add_argument('--payload-report', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    require(args.output.resolve() not in (args.firmware.resolve(), args.payload.resolve()), 'Output must be a new path')
    firmware = args.firmware.read_bytes()
    fw_report = json.loads(args.firmware_report.read_text())
    require(sha(firmware) == fw_report['sha256'], 'Firmware SHA256 mismatch')
    require(fw_report['status'] == 'offline-image-checks-passed', 'Firmware was not verified')
    require(len(firmware) >= 4096 and firmware[:8] == b'ANDROID!', 'Not an Android boot image')
    require(struct.unpack_from('<I',firmware,40)[0] == 4, 'Requires header v4')
    signature_size = struct.unpack_from('<I',firmware,1580)[0]
    require(signature_size in (0,4096), 'Unexpected boot signature size')
    if signature_size:
        require(firmware[-signature_size:] == bytes(signature_size),
                'Refusing to modify a signed boot image')
    kernel_size = struct.unpack_from('<I',firmware,8)[0]
    require(0 < kernel_size <= len(firmware)-4096, 'Invalid kernel size')
    payload = args.payload.read_bytes()
    pr = json.loads(args.payload_report.read_text())
    require(len(payload) == pr['payload_bytes'] and sha(payload) == pr['payload_sha256'], 'Payload SHA256/size mismatch')
    require(512 <= len(payload) <= 224*1024*1024, 'Payload exceeds first-entry memory budget')
    require(payload[510:512] == b'\x55\xaa' and payload[82:90] == b'FAT32   ', 'Not a FAT32 payload')
    require(payload[71:82] == b'GIULIAESP  ', 'Unexpected FAT label')
    require(struct.unpack_from('<H',payload,11)[0] == 512, 'Unexpected FAT sector size')
    require(struct.unpack_from('<I',payload,32)[0]*512 == len(payload), 'FAT size mismatch')
    header = bytearray(firmware[:4096])
    struct.pack_into('<I',header,12,len(payload))
    old_kernel = firmware[4096:4096+kernel_size]
    decoder = zlib.decompressobj(16+zlib.MAX_WBITS)
    raw = decoder.decompress(old_kernel)+decoder.flush()
    require(decoder.eof and decoder.unused_data[:4]==b'\xd0\x0d\xfe\xed', 'Missing gzip/FDT kernel layout')
    # The firmware receives a DT whose Android initrd range can be stale.
    # Pin the exact payload size/CRC for a bounded, read-only reserved-RAM scan.
    crc = zlib.crc32(payload)
    with tempfile.TemporaryDirectory() as directory:
        dtb = Path(directory)/'giulia.dtb'
        dtb.write_bytes(decoder.unused_data)
        for name,value in [('unixnu,payload-bytes',len(payload)),('unixnu,payload-crc32',crc)]:
            subprocess.run(['fdtput','-t','x',str(dtb),'/chosen',name,f'{value:x}'],check=True)
        kernel = gzip.compress(raw,mtime=0)+dtb.read_bytes()
    struct.pack_into('<I',header,8,len(kernel))
    result = bytes(header)+kernel+bytes(-len(kernel)%4096)+payload+bytes(-len(payload)%4096)+bytes(signature_size)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('xb') as f:
        f.write(result)
    report = {'firmware_sha256': sha(firmware), 'payload_sha256': sha(payload),
              'sha256': sha(result), 'bytes': len(result), 'header_version': 4,
              'payload_crc32':f'{crc:08x}',
              'purpose': pr.get('purpose', 'XNU first entry; no system volume'), 'hardware_tested': False}
    args.output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__ == '__main__':
    main()
