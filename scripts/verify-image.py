"""Verify Android v4 header, gzip payload, BootShim, FD and appended DTB."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tomllib
import zlib

def require(condition, message):
    if not condition:
        raise SystemExit(message)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('uefi', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    uefi = args.uefi
    image = (uefi / 'Mu-giulia-0.img').read_bytes()
    require(image[:8] == b'ANDROID!', 'Bad Android magic')
    u32 = lambda offset: struct.unpack_from('<I', image, offset)[0]
    require(u32(40) == 4, 'Expected boot header v4')
    require(u32(20) == 1584, 'Unexpected boot header size')
    require(len(image) >= 4096 + u32(8), 'Truncated kernel payload')
    kernel = image[4096:4096 + u32(8)]
    dec = zlib.decompressobj(16 + zlib.MAX_WBITS)
    raw = dec.decompress(kernel) + dec.flush()
    require(dec.eof, 'Truncated gzip')
    shim = (uefi / 'BootShim/BootShim.bin').read_bytes()
    fd = (uefi / 'Build/giuliaPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd').read_bytes()
    dtb = (uefi / 'Resources/DTBs/giulia.dtb').read_bytes()
    cfg = tomllib.loads((uefi / 'Resources/Configs/giulia.toml').read_text())
    version = [int(v) for v in cfg['boot_image']['os_version'].split('.')]
    year, month = [int(v) for v in cfg['boot_image']['os_patch_level'].split('-')]
    os_field = (version[0] << 25) | (version[1] << 18) | (version[2] << 11) | ((year - 2000) << 4) | month
    require(u32(16) == os_field, 'OS version/patch field differs from board config')
    ramdisk_start = 4096 + ((u32(8) + 4095) // 4096) * 4096
    ramdisk = (uefi / 'Resources/ramdisk').read_bytes()
    require(u32(12) == len(ramdisk), 'Unexpected ramdisk size')
    require(image[ramdisk_start:ramdisk_start + len(ramdisk)] == ramdisk, 'Ramdisk mismatch')
    require(len(fd) == cfg['uefi_fd']['size'], 'FD size mismatch')
    require(shim[56:60] == b'ARM\x64', 'Missing ARM64 image header')
    require(raw == shim + fd, 'BootShim/FD byte mismatch')
    require(dec.unused_data == dtb, 'Appended DTB mismatch')
    require(dtb[:4] == b'\xd0\x0d\xfe\xed', 'Bad FDT magic')
    require(struct.unpack_from('>I', dtb, 4)[0] == len(dtb), 'FDT length mismatch')
    report = {'status': 'offline-image-checks-passed', 'device': 'PKG110', 'model': 0,
              'header_version': 4, 'os_version_field': hex(u32(16)), 'size': len(image), 'fd_size': len(fd),
              'sha256': hashlib.sha256(image).hexdigest(), 'hardware_tested': False,
              'contains_xnu_kernel': False}
    binaries=uefi/'Build/giuliaPkg/DEBUG_CLANGPDB/AARCH64'
    wifi=binaries/'GiuliaInitrdDxe.efi'
    loader=binaries/'DarwinBoot.efi'
    report['wifi_pcie_handoff_available']=False
    if wifi.is_file() and loader.is_file():
        for version,driver_marker,loader_marker in (
            (1,b'GIULIA WIFI: prepare protocol installed:',b' giulia_wifi='),
            (2,b'GIULIA WIFI: prepare protocol v2 installed:',b' giulia_wifi2='),
            (3,b'GIULIA WIFI: prepare protocol v3 installed:',b' giulia_wifi3='),
            (4,b'GIULIA WIFI: prepare protocol v4 installed:',b' giulia_wifi4='),
            (5,b'GIULIA WIFI: prepare protocol v5 installed:',b' giulia_wifi5=')):
            if driver_marker in wifi.read_bytes() and loader_marker in loader.read_bytes():
                report['wifi_pcie_handoff_available']=True
                report['wifi_pcie_report_version']=version
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
