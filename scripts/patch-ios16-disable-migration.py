#!/usr/bin/env python3
"""Patch com.apple.datamigrator and com.apple.migrationpluginwrapper entrypoints with immediate return 0.

Eliminates the migration hang (AppleKeyStore/DataProtection key wrapping stall)
which holds the progress bar and Apple logo indefinitely.
"""
import argparse
from pathlib import Path

# Direct Mach-O entrypoint patch (mov w0, #0; ret)
MACHO_ENTRY_PATCH = bytes.fromhex("00008052c0035fd6")

TARGETS = [
    (0x17fa243dc, "com.apple.datamigrator", bytes.fromhex("fd7bbfa9fd030091")),
    (0x17fa56bdc, "com.apple.migrationpluginwrapper", bytes.fromhex("fd7bbfa9fd030091")),
]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--image", type=Path, default=Path("/home/ronard/unix-giulia/out/ios16-root/giulia-ios16-userdata.img"))
    args = p.parse_args()

    if not args.image.is_file():
        print(f"Error: image not found at {args.image}")
        return 1

    with open(args.image, "r+b") as f:
        for off, name, orig in TARGETS:
            f.seek(off)
            cur = f.read(len(MACHO_ENTRY_PATCH))
            if cur[:8] == orig:
                f.seek(off)
                f.write(MACHO_ENTRY_PATCH)
                f.flush()
                print(f"[SUCCESS] Patched {name} entrypoint at 0x{off:x} with mov w0, #0; ret")
            elif cur == MACHO_ENTRY_PATCH:
                print(f"[OK] {name} entrypoint already patched at 0x{off:x}")
            else:
                print(f"[WARN] Unexpected bytes at 0x{off:x} for {name}: {cur.hex()}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
