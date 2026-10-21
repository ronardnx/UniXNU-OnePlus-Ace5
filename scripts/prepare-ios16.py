"""Generate Ace 5 patches from the pinned Vermeer integration, not Redmi binaries."""
import argparse
import shutil
import subprocess
import sys
import runpy
import re
import json
import difflib
from pathlib import Path

def edit(path, old, new, count=1):
    text = path.read_text(errors='surrogateescape')
    if text.count(old) != count:
        raise RuntimeError(f'Source drift: {path.name}: {old!r}: {text.count(old)} != {count}')
    path.write_text(text.replace(old, new), errors='surrogateescape')

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--gui', action='store_true', help='Include bounded display/surface/HID compatibility services')
    p.add_argument('--touch', action='store_true', help='Include opt-in Ace 5 TouchCom v1 hardware bring-up')
    a = p.parse_args()
    if a.touch and not a.gui:p.error('--touch requires --gui')
    revision = subprocess.check_output(['git', '-C', str(a.reference), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != 'd68e05df683f863f054b0f9c063c54d2b1342b2a':
        raise RuntimeError('Unpinned reference checkout')
    shutil.copytree(a.reference / 'xnu', a.work)
    v = a.work / 'vermeer'
    usb, probe, block = [v / f'vermeer_{n}.c' for n in ['usb_debug','ufs_probe','ufs_block']]
    # Allocator-owned physically contiguous pages, bounded by the boot memory
    # actually passed to XNU. Never infer an owned range from the K70 addresses.
    for name in ['vermeer_ufs_dma.h','vermeer_usb_protocol.h']:
        path = v / name
        edit(path, '#include <sys/types.h>', '#include <sys/types.h>\nextern unsigned long gPhysBase, gPhysSize;')
    edit(v/'vermeer_ufs_dma.h', 'if (pa >= 0xc0000000ULL && pa <= 0xd0000000ULL - VU_DMA_SIZE) return 1;',
         'if (pa < gPhysBase || gPhysSize < VU_DMA_SIZE || pa - gPhysBase > gPhysSize - VU_DMA_SIZE) return 0;\n    if (pa <= 0x100000000ULL - VU_DMA_SIZE) return 1;')
    edit(v/'vermeer_ufs_dma.h', 'pa >= 0x8c0000000ULL && pa <= 0x940000000ULL - VU_DMA_SIZE',
         'pa <= (1ULL << 36) - VU_DMA_SIZE')
    edit(v/'vermeer_usb_protocol.h', 'pa<0x8c0000000ULL ||\n        pa>=0x940000000ULL || size>0x940000000ULL-pa',
         'pa<gPhysBase ||\n        gPhysSize<size || pa-gPhysBase>gPhysSize-size')
    for path in [usb, probe]:
        edit(path, '0x4c017e69', '0x4c017e6b')
        edit(path, '0x60000046', '0x60000047')
    edit(usb, 'i<105', 'i<107')
    edit(usb, 'cb>=70', 'cb>=71')
    edit(usb, '0xd0000', '0x100000')
    edit(usb, 'K70DBG004', 'ACE5DBG001', 4)
    edit(usb, 'Vermeer Lab', 'Giulia Lab')
    edit(usb, 'Vermeer XNU Debug', 'Ace 5 XNU Debug')
    # SCR0 is logged, not compared to an unobserved K70 value; stream and
    # context policy remains mandatory, including VA64 for high addresses.
    edit(usb, 'read_reg(s,0)!=0x00280406 || ', '')
    edit(usb, 'return vusb_route_identity(route,cbar,sctlr)!=0;',
         'return vusb_route_identity(route,cbar,sctlr)!=0 &&\n        (((route>>16)&3)==1 || (read_reg(s,0x1800+4*cb)&1));')
    edit(probe, '0x90000', '0x100000')
    edit(probe, '61842431U', '61321215U')
    edit(probe, '0x45fbee46U', '0xc319db94U')
    # Embed a C-compatible guarded context handoff; the generated source patch
    # then carries the entire implementation with correct hunk lengths.
    guard = (Path(__file__).resolve().parents[1]/'ios16/giulia_smmu.h').read_text()
    guard = guard.replace('#include <stdint.h>', '')
    edit(probe, 'static int identity_candidate(void)', guard + '\nstatic uint32_t gs_r(unsigned o) { return rd(SMMU+o); }\n'
         'static void gs_w(unsigned o,uint32_t v) { *(volatile uint32_t *)(SMMU+o)=v; __asm__ volatile("dsb sy" ::: "memory"); }\n'
         'static int giulia_take(void) { return gs_take(gs_r,gs_w,!rd(UFS+0x60)&&!rd(UFS+0x58)&&!rd(UFS+0x78)); }\n'
         'static int giulia_restore(void) { return gs_restore(gs_r,gs_w,!rd(UFS+0x60)&&!rd(UFS+0x58)&&!rd(UFS+0x78)); }\n'
         'static int identity_candidate(void)')
    text=probe.read_text(); start=text.index('    if (rd(SMMU + 0x20)',text.index('static int identity_candidate'))
    end=text.index('    /* Static buffer range',start)
    text=text[:start]+'    if (gs_plan(gs_r)) return 1;\n'+text[end:]; probe.write_text(text)
    edit(probe,'    wr(0x50, vu_lo(dma_pa));','    if (giulia_take()) return ENXIO;\n    wr(0x50, vu_lo(dma_pa));')
    edit(probe,'            wr(0x50, oldlo);','            if (giulia_restore()) return ENXIO;\n            wr(0x50, oldlo);')
    edit(probe,'0x80058','0xc6058')
    edit(block,'    wr(0x50, vu_lo(dma_pa));','    if (giulia_take()) return ENXIO;\n    wr(0x50, vu_lo(dma_pa));')
    edit(block,'    wr(0x50, vb.oldlo);','    if (giulia_restore()) { vb.failed=true; return; }\n    wr(0x50, vb.oldlo);')
    edit(v/'vermeer_media_policy.h','2570304ULL','3920350ULL')
    edit(v/'vermeer_media_policy.h','59272123ULL','57400861ULL')
    edit(block,'g.first != 2570304 || g.blocks != 59272123','g.first != 3920350 || g.blocks != 57400861')
    # Start the scheduler-based CDC worker at root selection, BEFORE UFS I/O.
    hook=v/'0021-vermeer-ios-root-hook.patch'
    edit(hook,'+766,22','+766,33')
    edit(hook,'+\t\tint error = vermeer_ufs_ios_rootdev(root);',
         '+\t\textern void vermeer_usb_debug_start(void);\n+\t\tvermeer_usb_debug_start();\n'
         '+\t\tint giulia_probe = 0;\n+\t\tPE_parse_boot_argn("giulia_ufs_probe", &giulia_probe, sizeof(giulia_probe));\n'
         '+\t\tif (giulia_probe == 1) {\n+\t\t\textern int vermeer_ufs_probe(void);\n'
         '+\t\t\tint probe_error = vermeer_ufs_probe();\n'
         '+\t\t\tIOLog("GIULIA: UFS read probe result %d; USB console remains active\\n", probe_error);\n'
         '+\t\t\t/* Keep the scheduler alive for CDC enumeration after a failed read. */\n'
         '+\t\t\tfor (;;) IOSleep(1000);\n+\t\t}\n'
         '+\t\tint error = vermeer_ufs_ios_rootdev(root);')
    if a.gui:
        edit(usb,'static void command_line(void)','extern int sweet_sample_request(void);\nstatic void command_line(void)')
        edit(usb,'    else if (!strcmp(u.command,"status"))',
             '    else if (!strcmp(u.command,"sample")) printf("GIULIA DEBUG: sample wake requested=%d\\n",sweet_sample_request());\n'
             '    else if (!strcmp(u.command,"status"))')
        edit(usb,'help/status/ping/replay','help/status/ping/replay/sample')
        edit(usb,'commands help status ping replay;','commands help status ping replay sample;')
    sys.argv = [str(a.reference/'tools/prepare_vermeer.py'),str(a.work/'patches'),str(a.out),'--ios-gui' if a.gui else '--ios']
    runpy.run_path(sys.argv[0], run_name='__main__')
    for path in a.out.glob('*.patch'):
        text=path.read_text(errors='surrogateescape')
        for old,new in [('0xb8000000','0xd5100000'),('1440u','1264u'),('3200u','2780u'),
                        ('5760','5056'),('3168','2748'),('arm,cortex-a510','qcom,kryo'),('VERMEER USB:','GIULIA USB:'),
                        ('xiaomi,sweet','oneplus,giulia'),
                        ('strlcpy(name, "sweet",','strlcpy(name, "giulia",'),
                        ('strlcpy(name, "Redmi Note 10 Pro",','strlcpy(name, "OnePlus Ace 5",')]:
            text=text.replace(old,new)
        if a.gui:
            # Rewrite added, removed and context lines alike so the complete
            # display patch series still applies atomically. These standalone
            # decimal constants describe the reference panel/HID coordinates.
            text=re.sub(r'\b1440\b','1264',text)
            text=re.sub(r'\b3200\b','2780',text)
            text=text.replace('1440x3200','1264x2780').replace('VERMEER FB:','GIULIA FB:')
            text=text.replace('x > 1439 ? 1439','x > 1263 ? 1263').replace('y > 3199 ? 3199','y > 2779 ? 2779')
            text=text.replace('K70 framebuffer','Ace 5 framebuffer').replace('K70 panel','Ace 5 panel')
        path.write_text(text,errors='surrogateescape')
    if a.touch:
        patch=('--- a/bsd/conf/files\n+++ b/bsd/conf/files\n@@ -109,4 +109,5 @@\n'
               ' bsd/dev/vermeer_usb_debug.c\t\tstandard\n'
               ' bsd/dev/sweet_sample.c\t\t\tstandard\n+bsd/dev/giulia_touch.c\t\t\tstandard\n'
               ' bsd/dev/memdev.c\t\t\tstandard\n bsd/dev/mem.c\t\t\t\tstandard\n'
               '--- a/bsd/kern/bsd_init.c\n+++ b/bsd/kern/bsd_init.c\n@@ -1106,5 +1106,7 @@\n'
               ' \tvermeer_usb_debug_start();\n \textern void sweet_sample_start(void);\n'
               ' \tsweet_sample_start();\n+\textern void giulia_touch_start(void);\n+\tgiulia_touch_start();\n'
               ' #endif\n \n')
        for name in ('giulia_touch.c','giulia_touch_report.h','giulia_touch_wire.h','giulia_touch_gpio.h'):
            data=(Path(__file__).resolve().parents[1]/'ios16'/name).read_text()
            data=data.replace('#include <stdint.h>\n','').replace('#include <stddef.h>\n','')
            patch+=''.join(difflib.unified_diff([],data.splitlines(True),fromfile='/dev/null',tofile='b/bsd/dev/'+name))
        (a.out/'0069-giulia-touch.patch').write_text(patch)
        shutil.copyfile(Path(__file__).resolve().parents[1]/'ios16/0070-giulia-service-images.patch',
                        a.out/'0070-giulia-service-images.patch')
    capture=(a.reference/'tools/capture-debug.ps1').read_text().replace('K70DBG004','ACE5DBG001')
    if a.gui:
        shutil.copyfile(Path(__file__).resolve().parents[1]/'ios16/0071-giulia-on-demand-samples.patch',
                        a.out/'0071-giulia-on-demand-samples.patch')
        patch = ('--- a/iokit/conf/files.arm64\n+++ b/iokit/conf/files.arm64\n'
                 '@@ -13,3 +13,4 @@\n'
                 ' iokit/Kernel/arm/SweetEntitlements.cpp                           optional iokitcpp\n'
                 ' iokit/Kernel/arm/SweetKeyStore.cpp                               optional iokitcpp\n'
                 '+iokit/Kernel/arm/GiuliaAMFI.cpp                                  optional iokitcpp\n'
                 ' iokit/Kernel/arm/SweetWatchdog.cpp                               optional iokitcpp\n'
                 '--- a/iokit/KernelConfigTables.cpp\n+++ b/iokit/KernelConfigTables.cpp\n'
                 '@@ -51,3 +51,8 @@\n'
                 '     "     \'IOMatchCategory\' = AppleKeyStore;"\n'
                 '     "   },"\n'
                 '+    "   {"\n'
                 '+    "     \'IOClass\'         = AppleMobileFileIntegrity;"\n'
                 '+    "     \'IOProviderClass\' = IOResources;"\n'
                 '+    "     \'IOMatchCategory\' = AppleMobileFileIntegrity;"\n'
                 '+    "   },"\n'
                 '     "   {"\n')
        for name in ('GiuliaAMFI.cpp', 'giulia_trust_catalog.h'):
            data = (Path(__file__).resolve().parents[1]/'ios16'/name).read_text()
            data = data.replace('#include <stdint.h>\n', '').replace('#include <stddef.h>\n', '')
            patch += ''.join(difflib.unified_diff([], data.splitlines(True),
                              fromfile='/dev/null', tofile='b/iokit/Kernel/arm/'+name))
        (a.out/'0072-giulia-read-only-trust-catalog.patch').write_text(patch)
        capture=capture.replace("[ValidateSet('help','status','ping','replay')]","[ValidateSet('help','status','ping','replay','sample')]")
    (a.out/'capture-debug.ps1').write_text(capture)
    checker=(a.reference/'tools/check_ios_kernel.py').read_text().replace('K70DBG004','ACE5DBG001').replace('VERMEER USB:','GIULIA USB:')
    if a.gui:checker=checker.replace('VERMEER FB:','GIULIA FB:').replace('1440x3200','1264x2780')
    (a.out/'check-kernel.py').write_text(checker)
    (a.out/'profile.json').write_text(json.dumps({'device':'OnePlus Ace 5 PKG110',
        'profile':'gui' if a.gui else 'base','requires_local_gui_adt':a.gui,
        'physical_touch_driver_included':a.touch,'touch_protocol':'TouchCom v1 legacy/FIFO or GPIO SPI' if a.touch else None,
        'read_only_trust_catalog_query':a.gui,
        'hardware_tested':False},indent=2)+'\n')

if __name__=='__main__': main()
