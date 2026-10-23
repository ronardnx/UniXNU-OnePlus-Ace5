"""Strict offline regression for SAE's generated libkern configuration hunks.

Supply pristine xnu-8796.141.3 sources (only the two conf files are needed)
and a freshly generated Ace 5 profile. All preceding profile patches that
change either file are applied in CI order before checking the full 0076
patch. No downloads, kernel builds, or changes to the supplied trees occur.
Optionally provide a rejected older 0076 patch to reproduce its failure.
"""
from pathlib import Path
import argparse
import hashlib
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from ios16_wifi_sae import BEARSSL_SAE_SOURCES

BASE = {
    "libkern/conf/files": "f04589bac3837c2fb7fc69b229f2fb79628608cd49ba6d9d2b3df1912c63661f",
    "libkern/conf/Makefile.template": "649b6b63504fe4ffb3ebb1e4ddc0bb4ab51f6818e5c108544a7a56f125d6ea30",
}
TARGET = "0076-giulia-wifi-runtime.patch"


def apply(tree, patch, check=False):
    command = ["git", "apply", *["--include=" + name for name in BASE]]
    if check:
        command.append("--check")
    return subprocess.run([*command, str(patch)], cwd=tree, capture_output=True,
                          text=True, encoding="utf-8", errors="replace")


def require_success(result):
    assert not result.returncode, result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xnu", type=Path, required=True, help="Pristine xnu-8796.141.3 tree")
    parser.add_argument("--profile", type=Path, required=True, help="Generated profile with the complete patch chain")
    parser.add_argument("--rejected-patch", type=Path, help="Old 0076 patch expected to fail on both conf targets")
    args = parser.parse_args()
    profile = args.profile.resolve()
    target = profile / TARGET
    assert target.is_file(), target
    with tempfile.TemporaryDirectory(prefix="wifi-sae-patch-") as directory:
        tree = Path(directory)
        for name, digest in BASE.items():
            data = (args.xnu / name).read_bytes().replace(b"\r\n", b"\n")
            assert hashlib.sha256(data).hexdigest() == digest, "not pristine xnu-8796.141.3: " + name
            destination = tree / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        preceding = []
        for patch in sorted(profile.glob("*.patch")):
            if patch.name >= TARGET:
                break
            data = patch.read_bytes()
            if any(("+++ b/" + name).encode() in data for name in BASE):
                require_success(apply(tree, patch, check=True))
                require_success(apply(tree, patch))
                preceding.append(patch.name)
        assert preceding, "profile has no prior libkern configuration patches"
        if args.rejected_patch:
            result = apply(tree, args.rejected_patch.resolve(), check=True)
            assert result.returncode and all(name in result.stderr for name in BASE), result.stderr
        require_success(apply(tree, target, check=True))
        require_success(apply(tree, target))
        files = (tree / "libkern/conf/files").read_text(encoding="utf-8")
        makefile = (tree / "libkern/conf/Makefile.template").read_text(encoding="utf-8")
        block = makefile.index("ifeq ($(CURRENT_MACHINE_CONFIG),SWEET)")
        end = makefile.index("\nendif", block)
        units = ["giulia_wifi_sae_crypto", *["giulia_sae_" + Path(name).stem for name in BEARSSL_SAE_SOURCES]]
        for unit in units:
            assert files.count("libkern/crypto/" + unit + ".c\toptional crypto") == 1, unit
            for setting in (unit + ".o_INCFLAGS = $(SWEET_AES_INC)",
                            unit + ".o_CFLAGS_ADD += -mgeneral-regs-only"):
                assert makefile.count(setting) == 1 and block < makefile.index(setting) < end, setting
    print("SAE strict patch chain passed: pinned XNU conf files -> " + ", ".join(preceding) +
          f" -> full 0076 conf hunks; {len(units)} unique build units inside SWEET")


if __name__ == "__main__":
    main()
