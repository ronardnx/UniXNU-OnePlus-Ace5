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
from ios16_payload_budget import fat_geometry

def require(value, message):
    if not value:
        raise SystemExit(message)

def sha(data):
    return hashlib.sha256(data).hexdigest()

def positive_bytes(value):
    try:
        amount = int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError('Expected a positive byte count') from error
    if amount <= 0:
        raise argparse.ArgumentTypeError('Expected a positive byte count')
    return amount

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--firmware', type=Path, required=True)
    p.add_argument('--firmware-report', type=Path, required=True)
    p.add_argument('--payload', type=Path, required=True)
    p.add_argument('--payload-report', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--header-version', type=int, choices=[3,4], default=4,
                   help='3 is an explicit temporary-boot compatibility experiment')
    p.add_argument('--max-image-bytes', type=positive_bytes,
                   help='Reject a complete Android image larger than this independently verified partition capacity')
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
    if pr.get('hall_prepare_handoff'):
        require(pr.get('physical_slider_worker') is True and
                fw_report.get('hall_prepare_handoff_available') is True,
                'Slider requires matched native Hall preparation and bounded loader handoff')
    if pr.get('modem_contract'):
        mc=pr['modem_contract']
        from ios16_modem import build_binding
        require(mc.get('build_binding')==build_binding(),'Modem firmware/resource/Apple evidence binding mismatch')
        require(mc.get('schema')==1 and mc.get('runtime_abi')==1 and
                mc.get('board')=='giulia' and mc.get('system_build')=='20H392' and
                mc.get('platform_broker_included') is True,
                'Modem payload contract mismatch')
        require(mc.get('mode') in ('disabled','diagnostic') and
                mc.get('native_provider_included') is False and
                mc.get('cellular_policy')=='disabled' and not mc.get('baseband_paths'),
                'Native modem firmware handoff is not qualified by this UEFI packer')
    if pr.get('gpu_gunyah_handoff'):
        require(pr.get('gpu_mode') in (1,2,3) and fw_report.get('gpu_gunyah_handoff_available') is True,
                'GPU Gunyah resources require the matching UEFI capture and loader handoff')
    if pr.get('gpu_power_handoff'):
        require(pr.get('gpu_mode')==3 and fw_report.get('gpu_power_handoff_available') is True,
                'GPU mode 3 requires the matched GPU prepare protocol and fixed ADT loader')
    if pr.get('wifi_pcie_handoff'):
        if pr.get('wifi_stock_handoff'):
            require(fw_report.get('wifi_stock_handoff_available') is True,
                    'Payload requires the PKG110 stock PCIe initialization profile')
        if pr.get('wifi_cesta_handoff'):
            require(fw_report.get('wifi_cesta_handoff_available') is True,
                    'Payload requires the CESTA/CRM PCIe resource handoff')
        require(fw_report.get('wifi_pcie_handoff_available') is True,
                'PCIe payload requires the matching native Wi-Fi firmware adapter and loader')
        require(pr.get('wifi_pcie_report_version',1)==fw_report.get('wifi_pcie_report_version',1),
                'PCIe firmware/loader report ABI does not match the payload profile')
    require(len(payload) == pr['payload_bytes'] and sha(payload) == pr['payload_sha256'], 'Payload SHA256/size mismatch')
    geometry = fat_geometry(payload)
    require(pr.get('payload_geometry', geometry) == geometry, 'FAT geometry differs from payload report')
    crc = zlib.crc32(payload)
    require(pr.get('payload_crc32', f'{crc:08x}') == f'{crc:08x}', 'Payload CRC32 mismatch')
    # The current firmware has no independently exported payload-identity
    # marker. A relink input manifest can bind the header and resulting image,
    # but must not be described as verification of constants in the binary.
    firmware_identity = fw_report.get('payload_identity')
    if firmware_identity is not None:
        require(isinstance(firmware_identity, dict) and
                firmware_identity.get('firmware_sha256') == sha(firmware) and
                firmware_identity.get('payload_bytes') == len(payload) and
                firmware_identity.get('payload_crc32') == f'{crc:08x}',
                'Firmware relink input identity differs from the actual firmware/payload')
    header = bytearray(firmware[:4096])
    if args.header_version == 3:
        # Boot v3 carries its generic ramdisk in boot.img. Some v4 loaders
        # prefer a separate init_boot partition even for a downloaded image.
        struct.pack_into('<I',header,20,1580)
        struct.pack_into('<I',header,40,3)
        struct.pack_into('<I',header,1580,0)
        signature_size = 0
    struct.pack_into('<I',header,12,len(payload))
    old_kernel = firmware[4096:4096+kernel_size]
    decoder = zlib.decompressobj(16+zlib.MAX_WBITS)
    raw = decoder.decompress(old_kernel)+decoder.flush()
    require(decoder.eof and decoder.unused_data[:4]==b'\xd0\x0d\xfe\xed', 'Missing gzip/FDT kernel layout')
    # The firmware receives a DT whose Android initrd range can be stale.
    # Pin the exact payload size/CRC for a bounded, read-only reserved-RAM scan.
    with tempfile.TemporaryDirectory() as directory:
        dtb = Path(directory)/'giulia.dtb'
        dtb.write_bytes(decoder.unused_data)
        for name,value in [('unixnu,payload-bytes',len(payload)),('unixnu,payload-crc32',crc)]:
            subprocess.run(['fdtput','-t','x',str(dtb),'/chosen',name,f'{value:x}'],check=True)
        kernel = gzip.compress(raw,mtime=0)+dtb.read_bytes()
    struct.pack_into('<I',header,8,len(kernel))
    result = bytes(header)+kernel+bytes(-len(kernel)%4096)+payload+bytes(-len(payload)%4096)+bytes(signature_size)
    require(args.max_image_bytes is None or len(result) <= args.max_image_bytes,
            f'Complete Android image ({len(result)} bytes) exceeds the supplied partition capacity ({args.max_image_bytes} bytes)')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('xb') as f:
        f.write(result)
    report = {'firmware_sha256': sha(firmware), 'payload_sha256': sha(payload),
              'sha256': sha(result), 'bytes': len(result), 'header_version': args.header_version,
              'payload_bytes':len(payload), 'payload_crc32':f'{crc:08x}',
              'payload_geometry':geometry, 'max_image_bytes':args.max_image_bytes,
              'image_capacity_checked':args.max_image_bytes is not None,
              'compiled_payload_identity_verified':False,
              'purpose': pr.get('purpose', 'XNU first entry; no system volume'), 'hardware_tested': False}
    if firmware_identity is not None:
        report['firmware_payload_identity']=firmware_identity
        report['firmware_payload_identity_source']='firmware report relink input manifest'
    if pr.get('loader_budget'):report['loader_budget']=pr['loader_budget']
    if pr.get('modem_contract'):report['modem_contract']=pr['modem_contract']
    args.output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__ == '__main__':
    main()
