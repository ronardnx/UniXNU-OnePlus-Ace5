#!/usr/bin/env python3
"""Check that the GPU terminal commands agree on both sides of the wire.

The kernel service accepts a set of command names, the PC tool refuses anything
that is not in its own allow-list before sending, and the operator reads the USB
terminal documentation. Those are three independent files, so a command can be
implemented and still be unusable because one layer never learned about it.

This check is read-only and does not build or touch a device.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KERNEL = ROOT / "ios16" / "gpu" / "giulia_gpu_terminal.h"
HOST = ROOT / "scripts" / "debug_terminal.py"
DOC = ROOT / "docs" / "USB_TERMINAL.md"
TEMPLATE = ROOT / "hardware" / "gpu-stack-session.example.json"


def kernel_commands() -> list[str]:
    text = KERNEL.read_text(encoding="utf-8")
    return re.findall(r'gg_terminal_equal\(command,"([a-z0-9.]+)"\)', text)


def host_commands() -> set[str]:
    text = HOST.read_text(encoding="utf-8")
    block = re.search(r"DEVICE_COMMANDS\s*=\s*\((.*?)\)\s*\n", text, re.S)
    if not block:
        raise SystemExit("debug_terminal.py: DEVICE_COMMANDS block not found")
    return set(re.findall(r"'([a-z0-9.]+)'", block.group(1)))


def main() -> int:
    for path in (KERNEL, HOST, DOC):
        if not path.is_file():
            print("missing input: %s" % path.relative_to(ROOT))
            return 2
    kernel = kernel_commands()
    host = host_commands()
    doc = DOC.read_text(encoding="utf-8")
    problems: list[str] = []
    if len(set(kernel)) != len(kernel):
        problems.append("duplicate command mapping in %s" % KERNEL.name)
    for name in kernel:
        if name not in host:
            problems.append("host tool rejects %r: add it to DEVICE_COMMANDS" % name)
        if name not in doc:
            problems.append("documentation does not mention %r" % name)
    gpu_host = sorted(c for c in host if c.startswith("gpu.") and c != "gpu.dev")
    for name in gpu_host:
        if name not in kernel:
            problems.append("host tool offers %r but the kernel does not map it" % name)
    if TEMPLATE.is_file():
        try:
            template = json.loads(TEMPLATE.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            problems.append("session template is not valid JSON: %s" % error)
            template = {}
        for entry in template.get("commands", []):
            name = entry.get("name", "")
            if name not in kernel:
                problems.append("session template records %r, which the kernel does not map" % name)
    if problems:
        for line in problems:
            print("FAIL: " + line)
        return 1
    print("GPU terminal surfaces agree: %d kernel commands, %d host commands"
          % (len(kernel), len(host)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
