"""Qualify the pinned upstream patches for a build without the legacy display driver."""
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])/'patches/xnu/offline-component-load-probes.patch'
text = path.read_text()
old = '+\tint legacy_register = 0;\n'
end = '+\tint client_probe = 0;\n'
if '+#if defined(XNU_NATIVE_LEGACY_DISPLAY) && XNU_NATIVE_LEGACY_DISPLAY\n' + old not in text:
    if text.count(old) != 1 or text.count(end) != 1:
        raise SystemExit('Unexpected upstream legacy display probe patch')
    text = text.replace(old, '+#if defined(XNU_NATIVE_LEGACY_DISPLAY) && XNU_NATIVE_LEGACY_DISPLAY\n'+old)
    text = text.replace(end, '+#endif\n'+end)
    # Recount changed unified-diff hunks, including subsequent new-line offsets.
    lines = text.splitlines(keepends=True)
    delta = 0
    for i, line in enumerate(lines):
        if line.startswith(('diff --git ', '--- a/')):
            delta = 0
        match = re.match(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)',line)
        if not match:
            continue
        old_start, old_count, new_start, new_count, context = match.groups()
        previous_count = int(new_count or 1)
        body = []
        for item in lines[i+1:]:
            if item.startswith(('@@ ', 'diff --git ', '--- a/')):
                break
            if item.startswith((' ', '+', '-')):
                body.append(item)
        before = sum(item[0] in ' -' for item in body)
        after = sum(item[0] in ' +' for item in body)
        if before != int(old_count or 1):
            raise SystemExit('Unexpected old hunk size')
        lines[i] = f'@@ -{old_start},{before} +{int(new_start)+delta},{after} @@{context}\n'
        delta += after-previous_count
    path.write_text(''.join(lines))
print('Legacy display probe is conditional on its configured driver')
