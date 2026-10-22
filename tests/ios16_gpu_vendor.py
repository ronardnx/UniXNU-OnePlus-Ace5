"""Verify unchanged vendored SMMU code against the recorded upstream blobs."""
import hashlib
import json
from pathlib import Path
root=Path(__file__).resolve().parents[1]/'ios16/vendor/ArmSmmu4K'
manifest=json.loads((root/'SOURCE.json').read_text())
assert manifest['commit']=='4d0d6598b1a36aeec3de01091893fa6660e49388'
for item in manifest['files']:
    assert hashlib.sha256((root/item['file']).read_bytes()).hexdigest()==item['sha256'],item['file']
print('Pinned ArmSmmu4K sources, license and tests verified')
