"""Offline RSN unit test only. No kernel, image, or firmware build/access.

OpenSSL development headers/library and a host C compiler are required.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="wifi-rsn-") as folder:
    binary = str(Path(folder) / "rsn")
    subprocess.run([
        os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-Wno-misleading-indentation", "-Wno-deprecated-declarations",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        str(ROOT / "tests/ios16_wifi_rsn.c"), "-lcrypto", "-o", binary,
    ], check=True)
    subprocess.run([binary], check=True)
