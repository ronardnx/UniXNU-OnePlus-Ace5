"""Apply GPU integration in GUI profiles both with and without Wi-Fi."""
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from ios16_gpu_patch import gpu_patch

for wifi in (False,True):
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
        gpu_patch(folder,ROOT)
        subprocess.run(['git','apply','--check',str(folder/'0077-giulia-gpu-runtime.patch')],cwd=folder,check=True)
        subprocess.run(['git','apply',str(folder/'0077-giulia-gpu-runtime.patch')],cwd=folder,check=True)
        actual=(folder/'iokit/conf/files.arm64').read_text()
        assert actual.count('GiuliaGPU.cpp')==1
        assert ('GiuliaWiFi.cpp' in actual)==wifi
        assert 'giulia_gpu_sha256.h' in (folder/'iokit/Kernel/arm/giulia_gpu_pack.h').read_text()
        assert 'giulia_gpu_power_report.h' in (folder/'iokit/Kernel/arm/giulia_gpu_power_handoff.h').read_text()
        if wifi:
            assert '#define GIULIA_GPU_QMP_GATE 1' in (folder/'iokit/Kernel/arm/GiuliaWiFi.cpp').read_text()
print('GPU patch applies with default git context rules in both GUI profiles')
