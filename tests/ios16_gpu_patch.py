"""Apply all eight GPU/audio/Wi-Fi selections using the production composer."""
from pathlib import Path
from itertools import product
import tempfile
from driver_module_fixture import apply_modules

for flags in product((False, True), repeat=3):
    selected = [name for name, enabled in zip(('wifi', 'gpu', 'audio'), flags) if enabled]
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        apply_modules(folder, selected)
        units = (folder / 'iokit/conf/files.arm64').read_text()
        for name, unit in (('wifi', 'GiuliaWiFi.cpp'), ('gpu', 'GiuliaGPU.cpp'), ('audio', 'GiuliaAudio.cpp')):
            assert units.count(unit) == int(name in selected)
        assert units.count('GiuliaPlatformResources.cpp') == bool(selected)
        if 'gpu' in selected:
            arm = folder / 'iokit/Kernel/arm'
            assert 'giulia_sha256.h' in (arm / 'giulia_gpu_pack.h').read_text()
            assert 'giulia_platform_power_report.h' in (arm / 'giulia_platform_power.h').read_text()
            for source in (Path(__file__).resolve().parents[1] / 'ios16/gpu').iterdir():
                assert (arm / source.name).is_file(), source.name
print('All eight GPU/audio/Wi-Fi profiles apply with unique shared platform registration')
