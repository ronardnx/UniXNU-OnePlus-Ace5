"""Execute production Wi-Fi mapping/query methods against bounded IOKit stubs.

Only kernel services are replaced. Method bodies and the runtime structure are
extracted verbatim from GiuliaWiFi.cpp on every run; no device is accessed.
This proves host-visible ownership and ABI behavior, not physical DMA/cache
coherence, firmware compatibility, or successful radio bring-up.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PIN = "7bea48e5e850ab4cafbe68d3765cdaba13a86d6f"
BEAR = Path(os.environ.get("BEARSSL_SOURCE", ROOT / ".cache/wifi-sae-bearssl"))
# Exactly the production provider subset also exercised by ios16_wifi_sae.py.
# Compile it as C, then link those objects into the C++ runtime fixture.
assert subprocess.check_output(["git", "-C", str(BEAR), "rev-parse", "HEAD"], text=True).strip() == PIN
# Windows checkout CRLF conversion must not look like a provider modification
# when the offline runner uses WSL; all other tracked source changes fail.
subprocess.run(["git", "-C", str(BEAR), "diff", "--quiet", "--ignore-cr-at-eol",
                "--no-ext-diff", "--no-textconv", PIN, "--", "inc", "src"], check=True)
bear_sources = ["ec/ec_p256_m31.c", "hash/sha2small.c", "codec/ccopy.c", "codec/dec32be.c", "codec/enc32be.c"]
bear_sources += ["int/" + name + ".c" for name in (
    "i31_add", "i31_sub", "i31_bitlen", "i31_decmod", "i31_decode", "i31_decred",
    "i31_encode", "i31_ninv31", "i31_montmul", "i31_muladd", "i31_tmont",
    "i31_modpow", "i31_rshift", "i32_div32")]
source = (ROOT / "ios16/GiuliaWiFi.cpp").read_text()


def function(signature):
    begin = source.index(signature)
    start = source.index("{", begin)
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end] + "\n"


runtime_start = source.index("struct giulia_wifi_runtime {")
runtime_end = source.index("\n};", runtime_start) + 3
query_start = source.index("IOReturn IO80211Controller::query(")
query_end = source.rindex("\n#endif")
methods = source[query_start:query_end]
mapping = "".join(function(signature) for signature in [
    "static vm_offset_t gw_map_region(", "static void gw_unmap_regions(",
    "static vm_offset_t gw_register(", "static uint32_t gr32(",
    "static void gw32(", "static uint32_t gw_read_region(",
    "static void gw_write_region(", "static uint32_t gm_read(",
    "static void gm_write(", "static bool gw_allocate(",
    "static void gw_shutdown(", "void IO80211Controller::free(",
    "static void gw_unpublish(", "void IO80211Controller::stop(",
    "static void gn_detached(",
    "static errno_t gn_output(",
    "static errno_t gn_ioctl(",
    "static void gw_notice(",
])
# Host RAM-only barrier substitution; address validation and loads/stores stay
# unchanged. The real kernel keeps its AArch64 device barriers.
mapping = mapping.replace('__asm__ volatile("dmb oshld" ::: "memory")', '__sync_synchronize()')
mapping = mapping.replace('__asm__ volatile("dsb oshst" ::: "memory")', '__sync_synchronize()')

with tempfile.TemporaryDirectory(prefix="wifi-runtime-") as folder:
    path = Path(folder)
    (path / "wifi_runtime_types.inc").write_text(source[runtime_start:runtime_end])
    (path / "wifi_runtime_under_test.inc").write_text(methods)
    (path / "wifi_mapping_under_test.inc").write_text(mapping)
    network = (ROOT / "ios16/giulia_wifi_network_impl.h").read_text()
    network = network.replace('__asm__ volatile("dsb sy" ::: "memory")', '__sync_synchronize()')
    (path / "wifi_network_under_test.inc").write_text(network)
    objects = []
    crypto_sources = [ROOT / "ios16/giulia_wifi_sae_crypto.c", *[BEAR / "src" / name for name in bear_sources]]
    for index, crypto_source in enumerate(crypto_sources):
        obj = path / f"sae-{index}.o"
        subprocess.run([
            os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-misleading-indentation", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(BEAR / "inc"), "-I", str(BEAR / "src"), "-c", str(crypto_source), "-o", str(obj),
        ], check=True)
        objects.append(str(obj))
    subprocess.run([
        os.environ.get("CXX", "clang++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-Wno-misleading-indentation", "-Wno-unused-function", "-Wno-deprecated-declarations", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-pthread", "-I", str(ROOT / "ios16"), "-I", str(path),
        str(ROOT / "tests/ios16_wifi_runtime.cpp"), *objects, "-lcrypto", "-o", str(path / "runtime"),
    ], check=True)
    subprocess.run([str(path / "runtime")], check=True)
