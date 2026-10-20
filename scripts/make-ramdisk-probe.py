"""Build a read-only FAT transport probe: menu title only, no OS or flash tools."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

output = Path(sys.argv[1]).resolve()
if output.exists():
    raise SystemExit('Refusing to overwrite existing probe')
output.parent.mkdir(parents=True,exist_ok=True)
config = '''version = 1
[boot]
timeout = -1
fallback_timeout = 600
default = "restart"
[ui]
title = "Ace 5 - RAM payload mounted"
[kits]
autoload = false
[[entry]]
id = "restart"
title = "RAM payload OK - Restart"
type = "action"
action = "reset"
confirm = true
'''
with tempfile.TemporaryDirectory() as temporary:
    stage = Path(temporary)
    destination = stage/'EFI/UniXNU/unixnu.toml'
    destination.parent.mkdir(parents=True)
    destination.write_text(config)
    subprocess.run(['mformat','-C','-F','-T',str(64*2048),'-v','GIULIAESP',
                    '-i',str(output),'::'],check=True)
    subprocess.run(['mcopy','-i',str(output),'-s',str(stage/'EFI'),'::/'],check=True)
with output.open('rb') as f:
    digest = hashlib.file_digest(f,'sha256').hexdigest()
report = {'payload_bytes':output.stat().st_size,'payload_sha256':digest,
          'purpose':'Read-only FAT transport probe; no XNU or system volume'}
output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
