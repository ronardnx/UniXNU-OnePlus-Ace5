#!/usr/bin/env python3
"""Check that the 15-stage table in the framework header and the doc agree.

The stage table drives ordering, prerequisites and the class of every stage; the
documentation table is what the next hardware session reads. They are edited by
hand in different files, so a rename or a class change can silently leave the
doc describing a chain that no longer exists.

Read-only. No build, no device.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "ios16" / "gpu" / "giulia_gpu_stack.h"
DOC = ROOT / "docs" / "GPU_STACK_FRAMEWORK.md"

CLASS_TOKENS = {"RO": "只读", "HOST": "宿主", "LEASE": "租约", "DEVICE": "设备"}


def stage_enum() -> list[str]:
    text = HEADER.read_text(encoding="utf-8")
    block = re.search(r"enum ggsk_stage\s*\{(.*?)\};", text, re.S)
    if not block:
        raise SystemExit("giulia_gpu_stack.h: ggsk_stage enum not found")
    names = re.findall(r"GGSK_([A-Z_]+)", block.group(1))
    names = [n for n in names if n not in ("STAGES",)]
    expected = re.search(r"GGSK_STAGES\s*=\s*(\d+)", block.group(1))
    if expected and int(expected.group(1)) != len(names):
        raise SystemExit("GGSK_STAGES=%s but the enum lists %d stages"
                         % (expected.group(1), len(names)))
    return names


def header_table() -> list[tuple[str, str, str]]:
    text = HEADER.read_text(encoding="utf-8")
    block = re.search(r"struct ggsk_desc table\[GGSK_STAGES\]\s*=\s*\{(.*?)\n\s*\};", text, re.S)
    if not block:
        raise SystemExit("giulia_gpu_stack.h: ggsk_desc table not found")
    return re.findall(r'\{"([a-z0-9-]+)",([^,]+),GGSK_CLASS_([A-Z]+)\}', block.group(1))


def doc_table() -> list[tuple[str, str, str]]:
    rows: list[tuple[str, str, str]] = []
    for line in DOC.read_text(encoding="utf-8").splitlines():
        m = re.match(r"\| `GGSK_([A-Z_]+)` \| ([a-z0-9-]+) \| ([^|]+?) \|", line)
        if m:
            rows.append((m.group(1), m.group(2), m.group(3).strip()))
    return rows


def provider_coverage(enum: list[str]) -> list[str]:
    """Every stage must have ops assigned and be registered exactly once."""
    problems: list[str] = []
    impl = (ROOT / "ios16" / "gpu" / "giulia_gpu_stack_impl.h").read_text(encoding="utf-8")
    registered = re.search(r"registered=(\d+)", impl)
    if not registered:
        problems.append("stack_impl: no 'registered=N' summary log found")
    elif int(registered.group(1)) != len(enum):
        problems.append("stack_impl: log says registered=%s, enum has %d stages"
                        % (registered.group(1), len(enum)))
    for name in enum:
        assigned = impl.count("stackOps[GGSK_%s]=ops;" % name)
        if assigned != 1:
            problems.append("stage %s: stackOps assigned %d times, expected 1" % (name, assigned))
        registered = impl.count("ggsk_register(&stackState,GGSK_%s," % name)
        if registered != 1:
            problems.append("stage %s: registered %d times, expected 1" % (name, registered))
    return problems


def service_definitions() -> list[str]:
    """Each out-of-class definition must be unique and declared in the class."""
    problems: list[str] = []
    service = (ROOT / "ios16" / "gpu" / "GiuliaGPU.cpp").read_text(encoding="utf-8")
    class_block = re.search(r"class GiuliaGPU\b.*?\n\};", service, re.S)
    if not class_block:
        return ["GiuliaGPU.cpp: class block not found"]
    seen: dict[str, list[str]] = {}
    for path in sorted((ROOT / "ios16" / "gpu").glob("*.h")) + [ROOT / "ios16" / "gpu" / "GiuliaGPU.cpp"]:
        if path.name.startswith("giulia_gpu_stack.h"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(r"^(?:int|void|bool|uint32_t|uint8_t \*) GiuliaGPU::([A-Za-z_]\w*)\(",
                                 text, re.M):
            seen.setdefault(match.group(1), []).append(path.name)
    for name, files in sorted(seen.items()):
        if len(files) > 1:
            problems.append("GiuliaGPU::%s defined more than once: %s" % (name, ", ".join(files)))
        if re.search(r"\b%s\s*\(" % re.escape(name), class_block.group(0)) is None:
            problems.append("GiuliaGPU::%s is defined but not declared in the class" % name)
    return problems


def main() -> int:
    for path in (HEADER, DOC):
        if not path.is_file():
            print("missing input: %s" % path.relative_to(ROOT))
            return 2
    enum = stage_enum()
    desc = header_table()
    rows = doc_table()
    problems: list[str] = []
    if len(desc) != len(enum):
        problems.append("header table lists %d stages, enum declares %d" % (len(desc), len(enum)))
    if len(rows) != len(desc):
        problems.append("doc lists %d stages, header declares %d" % (len(rows), len(desc)))
    index_of = {name: i for i, name in enumerate(enum)}
    for index, (name, prereq, klass) in enumerate(desc):
        for dep in re.findall(r"GGSK_BIT\(GGSK_([A-Z_]+)\)", prereq):
            if dep not in index_of:
                problems.append("stage %d (%s): unknown prerequisite %s" % (index, name, dep))
            elif index_of[dep] >= index:
                problems.append("stage %d (%s): prerequisite %s is not earlier"
                                % (index, name, dep))
        if index >= len(rows):
            continue
        doc_name, doc_symbol, doc_class = rows[index][1], rows[index][0], rows[index][2]
        expected_symbol = enum[index] if index < len(enum) else "?"
        if doc_symbol != expected_symbol:
            problems.append("stage %d: doc symbol GGSK_%s, enum GGSK_%s"
                            % (index, doc_symbol, expected_symbol))
        if doc_name != name:
            problems.append("stage %d: doc name %r, header %r" % (index, doc_name, name))
        if doc_class != CLASS_TOKENS.get(klass, klass):
            problems.append("stage %d (%s): doc class %r, header %r"
                            % (index, name, doc_class, CLASS_TOKENS.get(klass, klass)))
    problems.extend(provider_coverage(enum))
    problems.extend(service_definitions())
    if problems:
        for line in problems:
            print("FAIL: " + line)
        return 1
    print("GPU stack contract agrees: %d stages (enum, table, prereqs, doc, class tokens)"
          % len(desc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
