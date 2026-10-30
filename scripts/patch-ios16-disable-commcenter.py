#!/usr/bin/env python3
"""Disable com.apple.CommCenter to eliminate the continuous crash loop on SM8650 bring-up.

Changes <key>KeepAlive</key> to <key>Disabled </key> and redirects ProgramArguments to /usr/bin/true.
"""
import argparse
from pathlib import Path

TARGET_KEEPALIVE = b"<key>KeepAlive</key>"
REPLACE_KEEPALIVE = b"<key>Disabled </key>"
KEEPALIVE_OFFSETS = [0xce23112d, 0xce23212d]

TARGET_BIN = b"<string>/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter</string>"
REPLACE_BIN = b"<string>/usr/bin/true                                                        </string>"
BIN_OFFSETS = [0xce231488, 0xce2324c5]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--image", type=Path, default=Path("/home/ronard/unix-giulia/out/ios16-root/giulia-ios16-userdata.img"))
    args = p.parse_args()

    if not args.image.is_file():
        print(f"Error: image not found at {args.image}")
        return 1

    with open(args.image, "r+b") as f:
        for off in KEEPALIVE_OFFSETS:
            f.seek(off)
            cur = f.read(len(TARGET_KEEPALIVE))
            if cur == TARGET_KEEPALIVE:
                f.seek(off)
                f.write(REPLACE_KEEPALIVE)
                f.flush()
                print(f"[SUCCESS] Disabled KeepAlive at 0x{off:x}")
            elif cur == REPLACE_KEEPALIVE:
                print(f"[OK] KeepAlive already disabled at 0x{off:x}")

        for off in BIN_OFFSETS:
            f.seek(off)
            cur = f.read(len(TARGET_BIN))
            if cur == TARGET_BIN:
                f.seek(off)
                f.write(REPLACE_BIN)
                f.flush()
                print(f"[SUCCESS] Replaced CommCenter binary at 0x{off:x} with /usr/bin/true")
            elif cur == REPLACE_BIN:
                print(f"[OK] CommCenter binary already replaced at 0x{off:x}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
