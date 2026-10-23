"""Compile and execute the modem wire fixtures; no hardware or modem access."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ModemQmi(unittest.TestCase):
    def compile_and_run(self, compiler, flags):
        with tempfile.TemporaryDirectory(prefix='giulia-modem-qmi-') as folder:
            binary = Path(folder) / 'modem-qmi'
            build = subprocess.run(
                [compiler, *flags, '-Wall', '-Wextra', '-Werror',
                 str(ROOT / 'tests/ios16_modem_qmi.c'), '-o', str(binary)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_c99_with_sanitizers(self):
        compiler = shutil.which('clang') or shutil.which('gcc')
        if not compiler:
            self.skipTest('C compiler unavailable; run the modem suite in WSL/Linux')
        self.compile_and_run(compiler, ['-std=c99', '-g',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all'])

    def test_cpp14_compatibility(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('C++ compiler unavailable; run the modem suite in WSL/Linux')
        self.compile_and_run(compiler, ['-x', 'c++', '-std=c++14'])


if __name__ == '__main__':
    unittest.main()
