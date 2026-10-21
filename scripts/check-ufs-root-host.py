"""Run upstream production root parser/read/publication tests on Linux or macOS."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--upstream',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--root-image',type=Path,required=True)
p.add_argument('--openssl-prefix',type=Path,required=True)
a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
copy=a.output/'NativeUfs'
shutil.copytree(a.upstream/'drivers/NativeUfs',copy,dirs_exist_ok=True)
# uint64_t is unsigned long on Linux; Apple's typedef is unsigned long long.
# Change only the fixture's initializer spelling, never the production parser.
test=copy/'tests/RootWindowRead.cpp'
text=test.read_text()
if text.count('{0ULL,UINT64_MAX}') != 1:
    raise SystemExit('Unexpected upstream fixture')
test.write_text(text.replace('{0ULL,UINT64_MAX}','{uint64_t(0),UINT64_MAX}'))
subprocess.run([sys.executable,str(copy/'tests/check_root_path_host.py'),
                '--output',str(a.output/'results'),
                '--root-image',str(a.root_image.resolve()),
                '--openssl-prefix',str(a.openssl_prefix)],check=True)
