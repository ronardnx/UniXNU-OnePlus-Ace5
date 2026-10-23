"""Module selection, source boundaries, registration and failure isolation."""
import importlib.util
from itertools import product
from pathlib import Path
import re
import shutil
import tempfile
import unittest
from driver_module_fixture import ROOT, apply_modules, write
from ios16_modules import emit_optional_modules, emit_input_modules
from ios16_modem import kernel_declaration, check_runtime, BROKER_MARKER


class DriverModules(unittest.TestCase):
    def test_dependency_boundaries(self):
        spec = importlib.util.spec_from_file_location('driver_boundaries', ROOT / 'scripts/check-driver-modules.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertGreater(module.validate(), 140)

    def test_all_sixteen_optional_profiles(self):
        for flags in product((False, True), repeat=4):
            selected = [name for name, enabled in zip(('wifi', 'gpu', 'audio', 'modem'), flags) if enabled]
            with self.subTest(selected=selected), tempfile.TemporaryDirectory() as temp:
                folder = Path(temp)
                registry = apply_modules(folder, selected)
                for patch in folder.glob('*.patch'):
                    # The upstream strict loader only protects four-digit numbered patches.
                    self.assertRegex(patch.name, r'^\d{4}-')
                units = (folder / 'iokit/conf/files.arm64').read_text()
                config = (folder / 'iokit/KernelConfigTables.cpp').read_text()
                for name, unit, cls in (('wifi', 'GiuliaWiFi.cpp', 'IO80211Controller'),
                                       ('gpu', 'GiuliaGPU.cpp', 'GiuliaGPU'),
                                       ('audio', 'GiuliaAudio.cpp', 'GiuliaAudio'),
                                       ('modem', 'GiuliaModem.cpp', 'GiuliaModem')):
                    self.assertEqual(units.count(unit), int(name in selected))
                    self.assertEqual(len(re.findall(r"'IOClass'\s*=\s*" + cls + ';', config)), int(name in selected))
                    if name not in selected:
                        self.assertFalse((folder / 'iokit/Kernel/arm' / unit).exists())
                for unit in ('GiuliaPlatformResources.cpp', 'GiuliaPlatformQmp.cpp'):
                    self.assertEqual(units.count(unit), bool(selected))
                if registry:
                    for unit in registry['translation_units']:
                        self.assertTrue((folder / unit).is_file(), unit)
                    # All generated local includes resolve in the staged layout.
                    arm = folder / 'iokit/Kernel/arm'
                    for path in arm.rglob('*'):
                        if path.suffix not in ('.h', '.c', '.cpp'):
                            continue
                        for include in re.findall(r'^#include "([^"\n]+)"', path.read_text(), re.M):
                            self.assertTrue((path.parent / include).is_file(), f'{path.name}: {include}')

    def test_unknown_selection_writes_nothing(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            with self.assertRaises(ValueError):
                emit_optional_modules(folder, ROOT, ['gpu', 'typo'])
            self.assertEqual(list(folder.iterdir()), [])

    def test_input_workers_register_once_with_and_without_touch(self):
        import subprocess
        for touch in (False, True):
            with self.subTest(touch=touch), tempfile.TemporaryDirectory() as temp:
                folder = Path(temp)
                write(folder / 'bsd/conf/files', '\n' * 108 +
                      'bsd/dev/vermeer_usb_debug.c\t\tstandard\n'
                      'bsd/dev/sweet_sample.c\t\t\tstandard\n'
                      'bsd/dev/memdev.c\t\t\tstandard\nbsd/dev/mem.c\t\t\t\tstandard\n')
                write(folder / 'bsd/kern/bsd_init.c', '\n' * 1105 +
                      '\tvermeer_usb_debug_start();\n\textern void sweet_sample_start(void);\n'
                      '\tsweet_sample_start();\n#endif\n\n')
                registry = emit_input_modules(folder, ROOT, touch=touch)
                for patch in sorted(folder.glob('*.patch')):
                    subprocess.run(['git', 'apply', str(patch)], cwd=folder, check=True, capture_output=True)
                units = (folder / 'bsd/conf/files').read_text()
                startup = (folder / 'bsd/kern/bsd_init.c').read_text()
                for name in registry['modules']:
                    self.assertEqual(units.count(f'bsd/dev/giulia_{name}.c'), 1)
                    self.assertEqual(startup.count(f'\tgiulia_{name}_start();'), 1)
                    self.assertTrue((folder / f'bsd/dev/giulia_{name}.c').is_file())
                if not touch:
                    self.assertNotIn('giulia_touch', units + startup)

    def test_boundary_checker_rejects_private_peer_include(self):
        spec = importlib.util.spec_from_file_location('driver_boundaries', ROOT / 'scripts/check-driver-modules.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            shutil.copytree(ROOT / 'scripts', folder / 'scripts', ignore=shutil.ignore_patterns('__pycache__'))
            write(folder / 'ios16/common/giulia_input_provider.h', '')
            write(folder / 'ios16/gpu/giulia_gpu_memory.h', '')
            write(folder / 'ios16/wifi/GiuliaWiFi.cpp', '#include "../gpu/giulia_gpu_memory.h"\n')
            with self.assertRaisesRegex(ValueError, 'private include from gpu'):
                module.validate(folder)

    def test_platform_can_exist_without_modem(self):
        declaration = kernel_declaration(BROKER_MARKER)
        self.assertFalse(declaration['modem_runtime_included'])
        self.assertIsNone(check_runtime(BROKER_MARKER, {}))


if __name__ == '__main__':
    unittest.main()
