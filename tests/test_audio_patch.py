"""Audio uses the platform module independently of peer selection."""
from pathlib import Path
import tempfile
import unittest
from driver_module_fixture import apply_modules


class AudioPatch(unittest.TestCase):
    def test_audio_after_optional_wifi_and_gpu_all_four_combinations(self):
        for wifi in (False, True):
            for gpu in (False, True):
                with self.subTest(wifi=wifi, gpu=gpu), tempfile.TemporaryDirectory() as directory:
                    folder = Path(directory)
                    selected = ['audio'] + (['wifi'] if wifi else []) + (['gpu'] if gpu else [])
                    apply_modules(folder, selected)
                    arm = folder / 'iokit/Kernel/arm'
                    audio = (arm / 'GiuliaAudio.cpp').read_text()
                    self.assertNotIn('giulia_gpu_', audio)
                    self.assertIn('gpr_global_legacy_ipcc_claim(2u,p)', audio)
                    self.assertIn('#define GIULIA_PLATFORM_BROKER_ENABLED 1', audio)
                    self.assertIn('"giulia_scm.h"', (arm / 'giulia_audio_scm.h').read_text())
                    self.assertTrue((arm / 'giulia_sha256.h').is_file())
                    units = (folder / 'iokit/conf/files.arm64').read_text()
                    for name in ('GiuliaAudio.cpp', 'GiuliaPlatformResources.cpp', 'GiuliaPlatformQmp.cpp'):
                        self.assertEqual(units.count(name), 1)


if __name__ == '__main__':
    unittest.main()
