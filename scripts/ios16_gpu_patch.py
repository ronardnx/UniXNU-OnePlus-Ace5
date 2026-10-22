"""Emit an independent Darwin 22 GPU bring-up service patch."""
import difflib


def gpu_patch(out, root):
    # Anchor on AMFI, present in every GUI profile, regardless of Wi-Fi selection.
    units = [p.name for p in sorted((root/'ios16').glob('giulia_gpu_*.c'))]
    added = ['iokit/Kernel/arm/GiuliaGPU.cpp                                   optional iokitcpp\n']
    added += ['iokit/Kernel/arm/'+name+' optional iokitcpp\n' for name in units]
    patch = ('--- a/iokit/conf/files.arm64\n+++ b/iokit/conf/files.arm64\n'
             f'@@ -14,2 +14,{2+len(added)} @@\n'
             ' iokit/Kernel/arm/SweetKeyStore.cpp                               optional iokitcpp\n'
             + ''.join('+'+line for line in added) +
             ' iokit/Kernel/arm/GiuliaAMFI.cpp                                  optional iokitcpp\n'
             '--- a/iokit/KernelConfigTables.cpp\n+++ b/iokit/KernelConfigTables.cpp\n'
             '@@ -55,3 +55,9 @@\n'
             '     "     \'IOMatchCategory\' = AppleMobileFileIntegrity;"\n'
             '     "   },"\n'
             '+    "   {"\n'
             '+    "     \'IOClass\'         = GiuliaGPU;"\n'
             '+    "     \'IOProviderClass\' = IOResources;"\n'
             '+    "     \'IOResourceMatch\' = IOBSD;"\n'
             '+    "     \'IOMatchCategory\' = GiuliaGPU;"\n'
             '+    "   },"\n'
             '     "   {"\n')
    names = ['GiuliaGPU.cpp', *units, *[p.name for p in sorted((root/'ios16').glob('giulia_gpu_*.h'))]]
    # Shared SHA implementation is copied under a GPU-local include name so
    # enabling Wi-Fi later never creates duplicate-file patch conflicts.
    sources = {name: (root/'ios16'/name).read_text() for name in names}
    sources['giulia_gpu_power_report.h'] = (root/'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/GpuPowerReport.h').read_text()
    sources['giulia_gpu_sha256.h'] = (root/'ios16/giulia_wifi_sha256.h').read_text()
    for path in sorted((root/'ios16/vendor/ArmSmmu4K').glob('*')):
        if path.name.startswith('test_') or (path.suffix not in ('.c','.h') and path.name not in ('LICENSE','SOURCE.json')): continue
        sources['vendor/ArmSmmu4K/'+path.name] = path.read_text()
    for name, data in sources.items():
        data = data.replace('"giulia_wifi_sha256.h"', '"giulia_gpu_sha256.h"')
        data = data.replace('"../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/GpuPowerReport.h"', '"giulia_gpu_power_report.h"')
        data = data.replace('#include <stdint.h>\n', '').replace('#include <stddef.h>\n', '')
        patch += ''.join(difflib.unified_diff([], data.splitlines(True),
                         fromfile='/dev/null', tofile='b/iokit/Kernel/arm/'+name))
    if (out/'0076-giulia-wifi-runtime.patch').exists():
        before=(root/'ios16/GiuliaWiFi.cpp').read_text()
        anchor='#define GIULIA_GPU_QMP_GATE 0'
        if before.count(anchor)!=1:
            raise ValueError('Wi-Fi GPU handoff gate source drift')
        after=before.replace(anchor,'#define GIULIA_GPU_QMP_GATE 1')
        patch+=''.join(difflib.unified_diff(before.splitlines(True),after.splitlines(True),
            fromfile='a/iokit/Kernel/arm/GiuliaWiFi.cpp',tofile='b/iokit/Kernel/arm/GiuliaWiFi.cpp'))
    (out/'0077-giulia-gpu-runtime.patch').write_text(patch, encoding='utf-8', newline='\n')
