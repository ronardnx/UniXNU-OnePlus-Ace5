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
from ios16_platform import check_platform
from ios16_trust_catalog import modules

def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--kernel',type=Path,required=True)
    p.add_argument('--adt',type=Path,required=True)
    p.add_argument('--trust-catalog',type=Path,help='Verified official firmware trust catalogue')
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--root-report',type=Path,help='Include HFSX boot entry for an already prepared root')
    p.add_argument('--default-root',action='store_true')
    p.add_argument('--trace-services',action='store_true',help='Sample GUI and first-boot migration services')
    p.add_argument('--trace-launch-services',action='store_true',help='Sample lsd and container services with their blocked clients')
    p.add_argument('--enable-touch',action='store_true',help='Start the compiled Ace 5 hardware touch worker')
    p.add_argument('--touch-gpio',action='store_true',help='Allow GPIO SPI on the observed GPI-only controller')
    a=p.parse_args()
    if a.trace_launch_services:a.trace_services=True
    if a.default_root and not a.root_report: p.error('Default root requires a root report')
    if a.out.exists():raise ValueError('Refusing to overwrite output')
    report=json.loads(a.kernel.with_suffix('.json').read_text())
    if report['output_sha256']!=sha(a.kernel) or report['boot_args_bytes']!=736:
        raise ValueError('Kernel handoff verification missing')
    platform_match=check_platform(a.adt.read_bytes(),a.kernel.read_bytes())
    catalog=None
    if a.trust_catalog:
        catalog=json.loads((a.trust_catalog.parent/'trust-catalog.json').read_text())
        pinned=json.loads((Path(__file__).resolve().parents[1]/'ios16/firmware.json').read_text())
        parsed=modules(a.trust_catalog.read_bytes())
        if (catalog['sha256']!=sha(a.trust_catalog) or catalog['source_ipsw_sha256']!=pinned['sha256']
                or len(catalog['modules'])!=len(parsed) or not a.root_report
                or not platform_match.get('gui')
                or b'GIULIA AMFI: trust catalogue ready;' not in a.kernel.read_bytes()):
            raise ValueError('Official trust catalogue/kernel binding failed')
    if a.trace_services and (not a.root_report or not platform_match.get('gui')):
        raise ValueError('Service sampling requires the GUI kernel and root entry')
    if a.enable_touch and (not a.root_report or not platform_match.get('gui') or b'GIULIA TOUCH: controller result=' not in a.kernel.read_bytes()):
        raise ValueError('Touch requires a GUI kernel containing the Ace 5 hardware driver')
    if a.touch_gpio and (not a.enable_touch or b'GIULIA TOUCH: GPIO SPI mode 0 active;' not in a.kernel.read_bytes()):
        raise ValueError('GPIO SPI requires the corresponding kernel driver and --enable-touch')
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
        # The pinned DEVELOPMENT kernel is built with XNU_TARGET_OS_OSX. Its
        # exec platform gate requires this existing development option for
        # native iOS executables (OS_REASON_EXEC / WRONG_PLATFORM == 9 / 14).
        # CodeDirectory/page validation is unchanged; this is not cs_enforcement_disable.
        if b'unentitled_ios_sim_launch\0' not in a.kernel.read_bytes():
            raise ValueError('Kernel lacks the iOS platform-launch development option')
        # The original launchd_cache_loader exposes this separate development
        # mode for its job cache when no Apple AMFI IOService is available.
        root_args=args+f' vermeer_ufs_rw=1 vermeer_root_uuid={ident} unentitled_ios_sim_launch=1 launchd_unsecure_cache=1'
        if platform_match.get('gui'):
            # The GUI driver mutes only ordinary screen text after its first
            # presentation. USB logging and panic text remain available.
            root_args+=' sweetfb_hz=30 sweetfb_noconsole=1'
        if a.trace_services:
            if b'sweetsample\0' not in a.kernel.read_bytes():
                raise ValueError('Kernel lacks the process sampler')
            # proc_name copies p_comm[MAXCOMLEN+1]; XNU MAXCOMLEN is 16.
            # The sampler's larger destination buffer does not recover p_name.
            sample_names='SpringBoard,backboardd,com.apple.datami,com.apple.migrat,appstored,installd,installcoordinat,accountsd,securityd,cfprefsd'
            if a.trace_launch_services:
                sample_names='SpringBoard,backboardd,lsd,containermanager,installd,installcoordinat,accountsd,securityd,cfprefsd,appstored'
            if len(sample_names)>=128:raise ValueError('Service selector buffer exceeded')
            root_args+=' sweetsample='+sample_names
            # The /dev/null tee uses proc_selfname (p_name), unlike proc_name.
            stderr_names='SpringBoard,backboardd,com.apple.datamigrator,com.apple.migrationpluginwrappe'
            if a.trace_launch_services:
                stderr_names='SpringBoard,backboardd,lsd,containermanagerd,installd,installcoordinationd,accountsd,securityd'
            root_args+=' sweetnull='+stderr_names
        if a.enable_touch:root_args+=' giulia_touch=1'
        if a.touch_gpio:root_args+=' giulia_touch_gpio=1'
        config+=('[[entry]]\nid = "ios16"\ntitle = "Ace 5 iOS16 HFSX root"\ntype = "xnu"\n'
                 'path = "Payloads/kernel.macho"\nargs = '+json.dumps(root_args)+'\n')
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
        if a.trust_catalog:shutil.copyfile(a.trust_catalog,payload/'kernel.macho.trustcache')
        (payload.parent/'unixnu.toml').write_text(config)
        for path in [stage,*stage.rglob('*')]:os.utime(path,(315532800,315532800))
        subprocess.run(['mformat','-C','-F','-T','131072','-v','GIULIAESP','-N','47495531','-i',str(image),'::'],env=env,check=True)
        subprocess.run(['mcopy','-i',str(image),'-s','-Q','-m',str(stage/'EFI'),'::/'],env=env,check=True)
    data=image.read_bytes()
    if data[82:90]!=b'FAT32   ' or struct.unpack_from('<I',data,32)[0]*512!=len(data):raise ValueError('FAT geometry')
    result={'payload_bytes':len(data),'payload_sha256':sha(image),'payload_crc32':f'{zlib.crc32(data):08x}',
            'payload_kernel_sha256':sha(a.kernel),'kernel_sha256':sha(a.kernel),'adt_sha256':sha(a.adt),
            'kernel_args':probe,'dram_handoff':'darwinboot-allocated-window','platform_match':platform_match,
            'hardware_tested':False,'purpose':'iOS16 XNU: UFS read probe and native USB console; no filesystem writes'}
    if a.root_report:
        result.update(root_sha256=root['sha256'],root_finder_id=ident,default_entry='ios16' if a.default_root else 'xnu',
                      root_kernel_args=root_args,ios_platform_launch_compat=True,launchd_job_cache_compat=True,
                      purpose='iOS16 HFSX root mount and UFS/USB diagnostic entries; use after manual userdata flash')
        if a.trace_services:
            result['trace_services']=True
            result['service_sample_names']=sample_names
            result['service_stderr_trace']=True
            result['service_stderr_names']=stderr_names
            result['service_trace_profile']='launchservices' if a.trace_launch_services else 'migration'
        if a.enable_touch:
            result['physical_touch_worker']=True
            result['touch_clock_handoff']=True
        if a.touch_gpio:result['touch_gpio']=True
    if catalog:
        result['trust_catalog']={k:catalog[k] for k in ('sha256','bytes','build','source_ipsw_sha256','unique_hashes')}
    (a.out/'payload.json').write_text(json.dumps(result,indent=2)+'\n')
    (a.out/'identity.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
