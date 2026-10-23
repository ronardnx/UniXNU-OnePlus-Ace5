#!/usr/bin/env python3
"""Keep the stack header, its description table and the design document in step.

The framework is only useful if the enum, the prerequisite table and the
documented gate list cannot drift apart silently. This check parses the three
places directly, so adding or reordering a stage without updating the document
fails the runner.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "ios16/gpu/giulia_gpu_stack.h"
DOC = ROOT / "docs" / "GPU_STACK_FRAMEWORK.md"

CLASS_TOKENS = {"RO": "只读", "HOST": "宿主", "DEVICE": "设备"}
STATUS_TOKENS = ("部分", "未通", "通")  # longest first: 部分/未通 both contain 通


def fail(message):
    print("STACK CONTRACT: " + message)
    raise SystemExit(1)


def enum_stages(text):
    block = re.search(r"enum ggsk_stage \{(.*?)\n\};", text, re.S)
    if not block:
        fail("enum ggsk_stage not found")
    body = re.sub(r"/\*.*?\*/", "", block.group(1), flags=re.S)
    names = re.findall(r"GGSK_([A-Z0-9_]+)", body)
    if not names or names[-1] != "STAGES":
        fail("enum ggsk_stage must end with the GGSK_STAGES sentinel")
    return names[:-1]


def described_stages(text):
    table = re.search(r"static const struct ggsk_desc table\[GGSK_STAGES\]=\{(.*?)\n    \};", text, re.S)
    if not table:
        fail("stage description table not found")
    rows = re.findall(r'\{"([a-z0-9-]+)",(.+?),GGSK_CLASS_([A-Z]+)\}', table.group(1))
    return rows


def documented_stages(text):
    rows = []
    for line in text.splitlines():
        if not line.startswith("| `GGSK_"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) != 8:
            fail("stage table row does not have 8 columns: " + line)
        rows.append([cell.strip("`") if index < 2 else cell for index, cell in enumerate(cells)])
    return rows


def main():
    header = HEADER.read_text(encoding="utf-8")
    doc = DOC.read_text(encoding="utf-8")
    stages = enum_stages(header)
    described = described_stages(header)
    documented = documented_stages(doc)

    if len(stages) != 15:
        fail("expected 15 stages, found %d" % len(stages))
    if len(described) != len(stages):
        fail("description table has %d rows for %d stages" % (len(described), len(stages)))
    if len(documented) != len(stages):
        fail("document has %d rows for %d stages" % (len(documented), len(stages)))

    status_count = {"通": 0, "部分": 0, "未通": 0}
    for index, stage in enumerate(stages):
        name, prereq, klass = described[index]
        enum_cell, doc_name, doc_class, doc_prereq, doc_status = documented[index][:5]
        if enum_cell != "GGSK_" + stage:
            fail("row %d documents %s but the enum says GGSK_%s" % (index, enum_cell, stage))
        if doc_name != name:
            fail("row %d name %s does not match header name %s" % (index, doc_name, name))
        expected = CLASS_TOKENS.get(klass)
        if expected is None or expected != doc_class:
            fail("row %d class %s does not match header class %s" % (index, doc_class, klass))
        for reference in re.findall(r"GGSK_BIT\(GGSK_([A-Z0-9_]+)\)", prereq):
            if reference not in stages:
                fail("row %d prerequisite %s is not a stage" % (index, reference))
            if stages.index(reference) >= index:
                fail("row %d prerequisite %s is not earlier" % (index, reference))
        status = next((token for token in STATUS_TOKENS if token in doc_status), None)
        if status is None:
            fail("row %d status %r is not one of 通/部分/未通" % (index, doc_status))
        status_count[status] += 1

    if not status_count["通"] or not status_count["未通"]:
        fail("status summary lost its contrast: %r" % status_count)

    print("STACK CONTRACT: %d stages match (通=%d 部分=%d 未通=%d)"
          % (len(stages), status_count["通"], status_count["部分"], status_count["未通"]))


if __name__ == "__main__":
    main()
