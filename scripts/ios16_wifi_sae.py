"""Source-only integration for SAE; this module never invokes a build tool."""
from pathlib import Path
import difflib

BEARSSL_PIN = "7bea48e5e850ab4cafbe68d3765cdaba13a86d6f"
BEARSSL_SAE_SOURCES = (
    "ec/ec_p256_m31.c", "hash/sha2small.c", "codec/ccopy.c",
    *["int/" + name + ".c" for name in (
        "i31_add", "i31_sub", "i31_bitlen", "i31_decmod", "i31_decode", "i31_decred",
        "i31_encode", "i31_ninv31", "i31_montmul", "i31_muladd", "i31_tmont",
        "i31_modpow", "i31_rshift", "i32_div32")],
)


def wifi_sae_patch(out: Path, root: Path) -> str:
    stage = out / "0020-stage-bearssl.sh"
    original = stage.read_text()
    anchor = "shutil.copy2(source / 'LICENSE.txt', target / 'LICENSE.txt')"
    if BEARSSL_PIN not in original or original.count(anchor) != 1:
        raise ValueError("SAE pinned BearSSL staging source drift")
    addition = ("# SAE group 19: fixed-time P-256/i31 and SHA-256, same pinned provider.\n"
                f"for name in {BEARSSL_SAE_SOURCES!r}:\n"
                "    (target / 'src' / name).parent.mkdir(parents=True, exist_ok=True)\n"
                "    shutil.copy2(source / 'src' / name, target / 'src' / name)\n")
    stage.write_text(original.replace(anchor, addition + anchor), encoding="utf-8", newline="\n")
    units = ["giulia_wifi_sae_crypto"] + ["giulia_sae_" + Path(name).stem for name in BEARSSL_SAE_SOURCES]
    patch = ("--- a/libkern/conf/files\n+++ b/libkern/conf/files\n"
             f"@@ -99,1 +99,{len(units)+1} @@\n"
             " libkern/crypto/corecrypto_aes.c\t\t\toptional crypto\n")
    patch += "".join(f"+libkern/crypto/{name}.c\toptional crypto\n" for name in units)
    patch += ("--- a/libkern/conf/Makefile.template\n+++ b/libkern/conf/Makefile.template\n"
              f"@@ -26,1 +26,{1+2*len(units)} @@\n"
              " sweet_aes.o_INCFLAGS = $(SWEET_AES_INC)\n")
    patch += "".join(f"+{name}.o_INCFLAGS = $(SWEET_AES_INC)\n"
                     f"+{name}.o_CFLAGS_ADD += -mgeneral-regs-only\n" for name in units)
    contents = {"giulia_wifi_sae_crypto.c": (root / "ios16/giulia_wifi_sae_crypto.c").read_text(),
                "giulia_wifi_sae_crypto.h": (root / "ios16/giulia_wifi_sae_crypto.h").read_text()}
    contents.update({"giulia_sae_" + Path(name).name:
                     '/* Pinned upstream BearSSL source retains its MIT license. */\n'
                     '#if defined(ARM64_BOARD_CONFIG_SWEET)\n'
                     f'#include "sweet_bearssl/src/{name}"\n#endif\n'
                     for name in BEARSSL_SAE_SOURCES})
    for name, content in contents.items():
        patch += "".join(difflib.unified_diff([], content.splitlines(True),
                         fromfile="/dev/null", tofile="b/libkern/crypto/" + name))
    return patch
