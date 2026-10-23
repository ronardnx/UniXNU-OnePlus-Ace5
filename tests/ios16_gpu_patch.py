"""Apply GPU/audio integration with each optional Wi-Fi service profile."""
from itertools import product
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from ios16_gpu_patch import gpu_patch
from ios16_audio_patch import audio_patch

for wifi,gpu,audio in product((False,True),repeat=3):
    with tempfile.TemporaryDirectory() as directory:
        folder=Path(directory)
        (folder/'iokit/conf').mkdir(parents=True)
        files=('\n'*13+'iokit/Kernel/arm/SweetKeyStore.cpp                               optional iokitcpp\n'
               'iokit/Kernel/arm/GiuliaAMFI.cpp                                  optional iokitcpp\n')
        if wifi:files+='iokit/Kernel/arm/GiuliaWiFi.cpp                                  optional iokitcpp\n'
        files+='iokit/Kernel/arm/SweetWatchdog.cpp                               optional iokitcpp\n'
        (folder/'iokit/conf/files.arm64').write_text(files)
        config=('\n'*54+'    "     \'IOMatchCategory\' = AppleMobileFileIntegrity;"\n'
                '    "   },"\n    "   {"\n')
        if wifi:config+='    "     \'IOClass\'         = IO80211Controller;"\n'
        config+='    "   },"\n'
        (folder/'iokit/KernelConfigTables.cpp').write_text(config)
        if wifi:
            (folder/'0076-giulia-wifi-runtime.patch').write_text('')
            (folder/'iokit/Kernel/arm').mkdir(parents=True)
            (folder/'iokit/Kernel/arm/GiuliaWiFi.cpp').write_text((ROOT/'ios16/GiuliaWiFi.cpp').read_text())
        patches=[]
        if gpu:
            gpu_patch(folder,ROOT)
            patches.append('0077-giulia-gpu-runtime.patch')
        if audio:
            audio_patch(folder,ROOT)
            patches.append('0084-giulia-audio-runtime.patch')
        for patch in patches:
            subprocess.run(['git','apply','--check',str(folder/patch)],cwd=folder,check=True)
            subprocess.run(['git','apply',str(folder/patch)],cwd=folder,check=True)
        actual=(folder/'iokit/conf/files.arm64').read_text()
        assert actual.count('GiuliaGPU.cpp')==int(gpu)
        assert actual.count('GiuliaAudio.cpp')==int(audio)
        assert ('GiuliaWiFi.cpp' in actual)==wifi
        if gpu:
            assert 'giulia_gpu_sha256.h' in (folder/'iokit/Kernel/arm/giulia_gpu_pack.h').read_text()
            assert 'giulia_gpu_power_report.h' in (folder/'iokit/Kernel/arm/giulia_gpu_power_handoff.h').read_text()
            for name in ('giulia_gpu_memstore.h','giulia_gpu_memstore_mapping.h','giulia_gpu_secure_mapping.h',
                         'giulia_gpu_ipcc_owner.h','giulia_gpu_fence.h','giulia_gpu_fence_provider.h',
                         'giulia_gpu_fence_mapping.h','giulia_gpu_fence_impl.h'):
                assert (folder/'iokit/Kernel/arm'/name).is_file()
        if wifi:
            assert f'#define GIULIA_GPU_QMP_GATE {int(gpu)}' in (folder/'iokit/Kernel/arm/GiuliaWiFi.cpp').read_text()
        if audio:
            assert f'#define GIULIA_GPU_IPCC_GATE {int(gpu)}' in (folder/'iokit/Kernel/arm/GiuliaAudio.cpp').read_text()
print('GPU/audio patches apply with default git context rules in all eight GUI profiles')
