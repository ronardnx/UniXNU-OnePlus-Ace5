"""Compile the real generated bridge, including profiles without GPU symbols."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from ios16_terminal import gpu_terminal_source
from debug_terminal import Commands, Capture, wire_command

for command in ('gpu.status','gpu.observe'):
    assert wire_command(command)==(command+'\n').encode()
for command in ('gpu.start','gpu.observe now','gpu.status\nping','gpu.observe;ping'):
    try:wire_command(command)
    except ValueError:pass
    else:raise AssertionError(command)

with tempfile.TemporaryDirectory() as folder:
    directory=Path(folder)
    for gpu in (False,True):
        source=directory/f'bridge-{int(gpu)}.c'
        code='#include <stdio.h>\n#include <string.h>\n#include <assert.h>\n'+gpu_terminal_source(gpu)
        if gpu:
            code+='int giulia_gpu_terminal(const char *s) { return !strcmp(s,"gpu.status") || !strcmp(s,"gpu.observe"); }\n'
        code+='''int main(void) {
  assert(giulia_gpu_terminal("gpu.status")==1);
  assert(giulia_gpu_terminal("gpu.observe")==1);
  assert(giulia_gpu_terminal("gpu.status extra")==0);
  assert(giulia_gpu_terminal("gpu.observe;ping")==0);
  return 0;
}
'''
        source.write_text(code)
        binary=directory/f'bridge-{int(gpu)}'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',str(source),'-o',str(binary)],check=True)
        output=subprocess.check_output([str(binary)],text=True)
        if not gpu:
            assert output.count('service=unavailable stage=not-compiled')==2
            assert 'extern int giulia_gpu_terminal' not in code
        else:assert 'extern int giulia_gpu_terminal' in code
    capture=Capture(directory/'capture')
    try:
        commands=Commands(capture,'fixture')
        commands.accept('watch gpu.status 1')
        assert commands.watch==('gpu.status',1)
        try:commands.accept('watch gpu.observe 1')
        except ValueError:pass
        else:raise AssertionError('Observation must remain an explicit request')
        commands.accept('all')
        assert 'gpu.status' in commands.queue and 'gpu.observe' not in commands.queue
    finally:capture.close()
print('GPU command bridge links with and without GPU; collection is status-only and observation stays explicit')
