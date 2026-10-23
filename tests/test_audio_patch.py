"""Default git-apply compatibility of audio, GPU and Wi-Fi GUI integration."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_audio_patch import audio_patch
from ios16_gpu_patch import gpu_patch


class AudioPatch(unittest.TestCase):
    @staticmethod
    def write(path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8', newline='\n')

    def apply(self, folder, patch):
        # No --unidiff-zero, --ignore-whitespace or --reject: the generated
        # profile has to apply under the build pipeline's default rules.
        for arguments in (['--check'], []):
            result = subprocess.run(['git', 'apply', *arguments, str(patch)], cwd=folder,
                                    capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which('git'), 'git is required for profile patch checks')
    def test_audio_after_optional_wifi_and_gpu_all_four_combinations(self):
        for wifi in (False, True):
            for gpu in (False, True):
                with self.subTest(wifi=wifi, gpu=gpu), tempfile.TemporaryDirectory() as directory:
                    folder = Path(directory)
                    units = ('\n' * 13 +
                             'iokit/Kernel/arm/SweetKeyStore.cpp                               optional iokitcpp\n'
                             'iokit/Kernel/arm/GiuliaAMFI.cpp                                  optional iokitcpp\n')
                    if wifi:
                        units += 'iokit/Kernel/arm/GiuliaWiFi.cpp                                  optional iokitcpp\n'
                    units += 'iokit/Kernel/arm/SweetWatchdog.cpp                               optional iokitcpp\n'
                    self.write(folder / 'iokit/conf/files.arm64', units)
                    registry = ('\n' * 54 +
                                '    "     \'IOMatchCategory\' = AppleMobileFileIntegrity;"\n'
                                '    "   },"\n'
                                '    "   {"\n')
                    if wifi:
                        registry += ('    "     \'IOClass\'         = IO80211Controller;"\n'
                                     '    "     \'IOMatchCategory\' = IO80211Controller;"\n')
                    else:
                        registry += '    "     \'IOClass\'         = ExistingResource;"\n'
                    registry += '    "   },"\n'
                    self.write(folder / 'iokit/KernelConfigTables.cpp', registry)
                    if wifi:
                        # Simulate the already-applied 0076 profile, including
                        # its marker consumed by both later patch generators.
                        self.write(folder / '0076-giulia-wifi-runtime.patch', '')
                        self.write(folder / 'iokit/Kernel/arm/GiuliaWiFi.cpp',
                                   (ROOT / 'ios16/GiuliaWiFi.cpp').read_text())
                    if gpu:
                        gpu_patch(folder, ROOT)
                        self.apply(folder, folder / '0077-giulia-gpu-runtime.patch')
                    audio_patch(folder, ROOT)
                    self.apply(folder, folder / '0084-giulia-audio-runtime.patch')

                    actual = (folder / 'iokit/conf/files.arm64').read_text()
                    for name, count in (('GiuliaAudio.cpp', 1), ('GiuliaAMFI.cpp', 1),
                                        ('SweetWatchdog.cpp', 1), ('GiuliaGPU.cpp', int(gpu)),
                                        ('GiuliaWiFi.cpp', int(wifi))):
                        self.assertEqual(actual.count(name), count, name)
                    config = (folder / 'iokit/KernelConfigTables.cpp').read_text()
                    for name, count in (('GiuliaAudio', 1), ('GiuliaGPU', int(gpu)),
                                        ('IO80211Controller', int(wifi))):
                        self.assertEqual(config.count("'IOClass'         = " + name + ';'), count)
                        self.assertEqual(config.count("'IOMatchCategory' = " + name + ';'), count)
                    self.assertEqual(config.count("'IOMatchCategory' = AppleMobileFileIntegrity;"), 1)

                    arm = folder / 'iokit/Kernel/arm'
                    self.assertTrue((arm / 'GiuliaAudio.cpp').is_file())
                    self.assertIn('#define GIULIA_GPU_IPCC_GATE ' + str(int(gpu)),
                                  (arm / 'GiuliaAudio.cpp').read_text())
                    for source in (ROOT / 'ios16').glob('giulia_audio_*.h'):
                        self.assertTrue((arm / source.name).is_file(), source.name)
                    scm = (arm / 'giulia_audio_scm.h').read_text()
                    self.assertIn('"giulia_audio_smc_transport.h"', scm)
                    self.assertNotIn('"giulia_gpu_scm.h"', scm)
                    self.assertTrue((arm / 'giulia_audio_smc_transport.h').is_file())
                    self.assertTrue((arm / 'giulia_audio_sha256.h').is_file())
                    for header in arm.glob('giulia_audio_*.h'):
                        source = header.read_text()
                        self.assertNotIn('#include <stdint.h>', source)
                        self.assertNotIn('#include <stddef.h>', source)
                    if wifi and gpu:
                        self.assertIn('#define GIULIA_GPU_QMP_GATE 1',
                                      (arm / 'GiuliaWiFi.cpp').read_text())


if __name__ == '__main__':
    unittest.main()
