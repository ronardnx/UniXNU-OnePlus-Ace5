"""Run the shared QMP fault model against the actual UEFI transport header."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
native=ROOT/'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/GpuQmpTransport.h'
source=(ROOT/'tests/ios16_wifi_qmp.c').read_text()
original=(ROOT/'ios16/giulia_wifi_qmp.h').read_text()
algorithm=original[original.index('#define QPT_MSGRAM_PA'):]
actual=native.read_text()
assert actual[actual.index('#define QPT_MSGRAM_PA'):]==algorithm
prelude=('''#include <stdint.h>
#include <stddef.h>
typedef uint32_t UINT32;typedef uint64_t UINT64;typedef uintptr_t UINTN;
#undef UINT32_C
#undef UINT64_C
#undef UINT32_MAX
#define MAX_UINT32 0xffffffffu
''')
with tempfile.TemporaryDirectory() as temporary:
    folder=Path(temporary)
    code=folder/'qmp.c'
    code.write_text(prelude+source.replace('"../ios16/giulia_wifi_qmp.h"','"'+native.as_posix()+'"'))
    binary=folder/'qmp'
    subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',str(code),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('UEFI GPU QMP algorithm matches shared source; actual firmware header passed fault model')
