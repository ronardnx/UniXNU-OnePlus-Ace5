"""Offline PMF protocol/crypto tests; no kernel or image is built."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='giulia-pmf-') as output:
    target = Path(output) / 'pmf'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', str(root / 'tests/ios16_wifi_pmf.c'), '-lcrypto', '-o', str(target)], check=True)
    subprocess.run([str(target)], check=True)
