"""Link the opt-in audio coordinator; platform ownership remains explicit."""
import difflib


def audio_patch(out, root):
    patch = ('--- a/iokit/conf/files.arm64\n+++ b/iokit/conf/files.arm64\n'
             '@@ -15,2 +15,3 @@\n'
             ' iokit/Kernel/arm/GiuliaAMFI.cpp                                  optional iokitcpp\n'
             '+iokit/Kernel/arm/GiuliaAudio.cpp                                 optional iokitcpp\n'
             ' iokit/Kernel/arm/SweetWatchdog.cpp                               optional iokitcpp\n'
             '--- a/iokit/KernelConfigTables.cpp\n+++ b/iokit/KernelConfigTables.cpp\n'
             '@@ -55,3 +55,9 @@\n'
             '     "     \'IOMatchCategory\' = AppleMobileFileIntegrity;"\n'
             '     "   },"\n'
             '+    "   {"\n'
             '+    "     \'IOClass\'         = GiuliaAudio;"\n'
             '+    "     \'IOProviderClass\' = IOResources;"\n'
             '+    "     \'IOResourceMatch\' = IOBSD;"\n'
             '+    "     \'IOMatchCategory\' = GiuliaAudio;"\n'
             '+    "   },"\n'
             '     "   {"\n')
    # With Wi-Fi, its earlier patch inserts one line at the same AMFI anchor.
    if (out/'0076-giulia-wifi-runtime.patch').exists():
        patch = patch.replace(
            ' iokit/Kernel/arm/SweetWatchdog.cpp                               optional iokitcpp\n',
            ' iokit/Kernel/arm/GiuliaWiFi.cpp                                  optional iokitcpp\n', 1)
    sources = {p.name:p.read_text() for p in (root/'ios16').glob('giulia_audio_*.h')}
    sources['GiuliaAudio.cpp'] = (root/'ios16/GiuliaAudio.cpp').read_text()
    if (out/'0077-giulia-gpu-runtime.patch').exists():
        anchor = '#define GIULIA_GPU_IPCC_GATE 0'
        if sources['GiuliaAudio.cpp'].count(anchor) != 1:
            raise ValueError('Audio GPU IPCC gate source drift')
        sources['GiuliaAudio.cpp'] = sources['GiuliaAudio.cpp'].replace(
            anchor, '#define GIULIA_GPU_IPCC_GATE 1')
    sources['giulia_audio_sha256.h'] = (root/'ios16/giulia_wifi_sha256.h').read_text()
    sources['giulia_audio_smc_transport.h'] = (root/'ios16/giulia_gpu_scm.h').read_text()
    for name, data in sorted(sources.items()):
        data = data.replace('"giulia_gpu_scm.h"', '"giulia_audio_smc_transport.h"')
        data = data.replace('#include <stdint.h>\n','').replace('#include <stddef.h>\n','')
        patch += ''.join(difflib.unified_diff([], data.splitlines(True),
            fromfile='/dev/null', tofile='b/iokit/Kernel/arm/'+name))
    (out/'0084-giulia-audio-runtime.patch').write_text(patch, encoding='utf-8', newline='\n')
