#!/usr/bin/env python3
"""Disable com.apple.CommCenter to eliminate the continuous crash loop on SM8650 bring-up.

Changes <key>KeepAlive</key> to <key>Disabled </key> in giulia-ios16-userdata.img.
"""
import argparse
from pathlib import Path

TARGET = b"<key>KeepAlive</key>"
REPLACEMENT = b"<key>Disabled </key>"
OFFSETS = [0xce23112d, 0xce23212d]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--image", type=Path, default=Path("/home/ronard/unix-giulia/out/ios16-root/giulia-ios16-userdata.img"))
    args = p.parse_args()

    if not args.image.is_file():
        print(f"Error: image not found at {args.image}")
        return 1

    with open(args.image, "r+b") as f:
        for off in OFFSETS:
            f.seek(off)
            cur = f.read(len(TARGET))
            if cur == REPLACEMENT:
                print(f"[OK] Already disabled at 0x{off:x}")
            elif cur == TARGET:
                f.seek(off)
                f.write(REPLACEMENT)
                f.flush()
                print(f"[SUCCESS] Disabled CommCenter at 0x{off:x}")
            else:
                print(f"[WARN] Unexpected bytes at 0x{off:x}: {cur}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
