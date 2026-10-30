#!/usr/bin/env python3
"""Apply SpringBoard display scale fix to dyld_shared_cache_arm64.09.

Patches FrontBoardServices at 0x19b921d38 (tbnz w0, #0 -> NOP) so that
[CADisplay scale] is executed unconditionally for all displays, avoiding
scale=0.0 and infinity x infinity EXC_BREAKPOINT in FBSDisplayMode.
"""
import argparse
import sys
from pathlib import Path

TARGET_INSTR = bytes.fromhex("80000037")  # tbnz w0, #0
NOP_INSTR    = bytes.fromhex("1f2003d5")  # nop

CACHE_OFFSET = 0x1c9d38
IMG_OFFSET   = 0x2596bcd38

def patch_file(path: Path, offset: int, desc: str):
    if not path.is_file():
        print(f"Skipping {desc}: {path} not found")
        return False
    with open(path, "r+b") as f:
        f.seek(offset)
        cur = f.read(4)
        if cur == NOP_INSTR:
            print(f"[OK] {desc} already patched at 0x{offset:x}")
            return True
        if cur != TARGET_INSTR:
            print(f"[ERR] {desc} expected {TARGET_INSTR.hex()} at 0x{offset:x}, got {cur.hex()}", file=sys.stderr)
            return False
        f.seek(offset)
        f.write(NOP_INSTR)
        f.flush()
        print(f"[SUCCESS] {desc} patched at 0x{offset:x} with NOP")
        return True

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cache", type=Path, default=Path("/mnt/media/hfsfile/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.09"))
    p.add_argument("--image", type=Path, default=Path("/home/ronard/unix-giulia/out/ios16-root/giulia-ios16-userdata.img"))
    args = p.parse_args()

    success = False
    if args.image.exists():
        success |= patch_file(args.image, IMG_OFFSET, "Rootfs Image (giulia-ios16-userdata.img)")
    if args.cache.exists():
        try:
            success |= patch_file(args.cache, CACHE_OFFSET, "dyld_shared_cache_arm64.09")
        except OSError as e:
            print(f"Note: cache file {args.cache} ({e})")
    return 0 if success else 1

if __name__ == "__main__":
    raise SystemExit(main())
