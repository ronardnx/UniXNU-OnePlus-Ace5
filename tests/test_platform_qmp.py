"""The platform handoff gate links and rejects uncertainty without a GPU driver."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_sources import include_flags


@unittest.skipUnless(shutil.which('clang++'), 'clang++ is required')
class PlatformQmp(unittest.TestCase):
    def test_handoff_without_gpu_service(self):
        source = (ROOT / 'ios16/platform/GiuliaPlatformQmp.cpp').read_text()
        source = re.sub(r'^#include <[^\n]+>\n', '', source, flags=re.M)
        shim = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#define ARM64_BOARD_CONFIG_SWEET 1
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
static const char *gIODTPlane="device-tree";
struct OSObject {virtual ~OSObject()=default;};
struct OSData : OSObject {
    const void *bytes;unsigned length;
    const void *getBytesNoCopy() {return bytes;}
    unsigned getLength() {return length;}
};
static bool present=true;
static unsigned releases;
static OSObject *property;
struct IORegistryEntry {
    static IORegistryEntry *fromPath(const char *,const char *) {
        static IORegistryEntry node;return present?&node:nullptr;
    }
    OSObject *getProperty(const char *) {return property;}
    void release() {++releases;}
};
'''
        scenario = r'''
int main() {
    present=false;assert(!giulia_platform_qmp_available());assert(!releases);
    present=true;assert(giulia_platform_qmp_available());assert(releases==1);
    OSObject invalid;property=&invalid;assert(!giulia_platform_qmp_available());
    OSData data;data.bytes=nullptr;data.length=0;property=&data;
    // An empty property is not the same as an absent handoff property.
    assert(!giulia_platform_qmp_available());
    gpp_report report={};data.bytes=&report;data.length=sizeof(report);
    assert(!giulia_platform_qmp_available());
    gpp_report_init(&report);report.code=GPP_ERR_PROTOCOL;
    report.phase=GPP_PHASE_RESOLVE;
    gpp_report_seal(&report);gpp_report decoded;
    assert(ggph_decode(&report,sizeof(report),&decoded)==GGPH_RECORDED);
    assert(giulia_platform_qmp_available());
    report.flags=GPP_QMP_UNCERTAIN;gpp_report_seal(&report);assert(!giulia_platform_qmp_available());
    report.flags=GPP_PROTOCOL_UNCERTAIN;gpp_report_seal(&report);assert(!giulia_platform_qmp_available());
    report.flags=0;data.length--;assert(!giulia_platform_qmp_available());
    assert(releases==8);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            unit, binary = path / 'qmp.cpp', path / 'qmp'
            unit.write_text(shim + source + scenario)
            result = subprocess.run(['clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                                     '-fsanitize=address,undefined', *include_flags(ROOT),
                                     str(unit), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run([str(binary)], check=True)
