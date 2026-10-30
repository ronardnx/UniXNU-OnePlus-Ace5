#!/usr/bin/env python3
"""Inject early hardware EPSS clock boost into kernel.macho at entry point."""

import hashlib
import json
from pathlib import Path

# Instructions injected at 0x4a2488 (0xfffffe00074a6488):
#   movz x16, #0x1320
#   movk x16, #0x17d9, lsl #16
#   mov  w17, #0x11
#   str  w17, [x16]              ; domain 0 (boot cluster) -> 2.26 GHz
#   add  x16, x16, #0x1000
#   str  w17, [x16]              ; domain 1 (gold cluster 1) -> 2.26 GHz
#   add  x16, x16, #0x1000
#   str  w17, [x16]              ; domain 2 (gold cluster 2) -> 2.26 GHz
#   dsb  sy
#   isb
#   b    0xfffffe00074a805c        ; continue original start_first_cpu
PATCH_BYTES = bytes.fromhex(
    "106482d230fba2f231028052110200b910064091110200b910064091110200b99f3f03d5df3f03d5eb060014"
)
OFFSET = 0x4A2488


def patch_kernel(kernel_path: Path, json_path: Path):
    with open(kernel_path, "r+b") as f:
        f.seek(OFFSET)
        f.write(PATCH_BYTES)
        f.flush()

    digest = hashlib.sha256(kernel_path.read_bytes()).hexdigest()
    if json_path.exists():
        data = json.loads(json_path.read_text())
        data["output_sha256"] = digest
        json_path.write_text(json.dumps(data, indent=2) + "\n")
    print(f"Patched kernel {kernel_path} (offset {OFFSET:#x}), new sha256: {digest}")


def main():
    root = Path(__file__).resolve().parents[1]
    kernel_path = root / "kernel-artifact" / "profile" / "kernel.macho"
    json_path = root / "kernel-artifact" / "profile" / "kernel.json"
    if not kernel_path.exists():
        raise SystemExit(f"Kernel macho not found: {kernel_path}")
    patch_kernel(kernel_path, json_path)


if __name__ == "__main__":
    main()
