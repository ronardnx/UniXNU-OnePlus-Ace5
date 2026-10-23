"""Compile/run only an offline SAE host test; never a kernel/image build.

Uses the same immutable BearSSL source as SWEET staging. Set BEARSSL_SOURCE
to a checkout at PIN, or populate .cache/wifi-sae-bearssl at that commit.
"""
from pathlib import Path
import json
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PIN = "7bea48e5e850ab4cafbe68d3765cdaba13a86d6f"
BEAR = Path(os.environ.get("BEARSSL_SOURCE", ROOT / ".cache/wifi-sae-bearssl"))
assert subprocess.check_output(["git", "-C", str(BEAR), "rev-parse", "HEAD"], text=True).strip() == PIN
sources = ["ec/ec_p256_m31.c", "hash/sha2small.c", "codec/ccopy.c", "codec/dec32be.c", "codec/enc32be.c"]
sources += ["int/" + n + ".c" for n in (
    "i31_add", "i31_sub", "i31_bitlen", "i31_decmod", "i31_decode", "i31_decred",
    "i31_encode", "i31_ninv31", "i31_montmul", "i31_muladd", "i31_tmont",
    "i31_modpow", "i31_rshift", "i32_div32")]
vectors = json.loads((ROOT / "tests/fixtures/sae-group19.json").read_text())
with tempfile.TemporaryDirectory(prefix="wifi-sae-") as folder:
    path = Path(folder)
    names = ("addr1", "addr2", "local_rand", "local_mask", "local_commit", "peer_commit",
             "kck", "pmk", "pmkid", "pwe_19_x", "pwe_19_y", "addr1b", "addr2b")
    (path / "sae_vectors.inc").write_text("\n".join(
        "static const uint8_t v_" + name + "[]={" + ",".join("0x" + vectors[name][i:i+2]
        for i in range(0, len(vectors[name]), 2)) + "};" for name in names))
    binary = path / "sae"
    subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-misleading-indentation", "-Wno-deprecated-declarations", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-I", str(BEAR / "inc"), "-I", str(BEAR / "src"), "-I", str(path),
        str(ROOT / "tests/ios16_wifi_sae.c"), str(ROOT / "ios16/giulia_wifi_sae_crypto.c"),
        *[str(BEAR / "src" / p) for p in sources], "-lcrypto", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
