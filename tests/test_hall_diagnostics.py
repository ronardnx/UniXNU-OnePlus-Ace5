import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
import shutil

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('hall_diagnostics', ROOT / 'scripts/enable-hall-diagnostics.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class HallDiagnostics(unittest.TestCase):
    def test_install_is_idempotent_and_before_commandline_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            driver = Path(directory)
            source = driver / 'DarwinBoot.c'
            source.write_text('#include <Library/PrintLib.h>\nvoid boot(void) {\n'
                              '  CopyMem (Args->CommandLine, CommandLine, sizeof (CommandLine));\n}\n')
            (driver / 'DarwinBoot.inf').write_text('[LibraryClasses]\n  UefiBootServicesTableLib\n')
            module.install(driver)
            first = source.read_bytes()
            module.install(driver)
            self.assertEqual(first, source.read_bytes())
            text = first.decode()
            self.assertEqual(text.count(module.CALL), 1)
            self.assertLess(text.index(module.CALL), text.index('  CopyMem (Args->CommandLine'))
            source.write_text(text.replace(' gh=%u', ' gh-old=%u'))
            module.install(driver)
            self.assertEqual(first, source.read_bytes())

    def test_bad_anchor_preserves_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            driver = Path(directory)
            for name in ('DarwinBoot.c', 'DarwinBoot.inf'):
                (driver / name).write_text('unchanged\n')
            with self.assertRaises(ValueError):
                module.install(driver)
            self.assertEqual((driver / 'DarwinBoot.c').read_text(), 'unchanged\n')
            self.assertEqual((driver / 'DarwinBoot.inf').read_text(), 'unchanged\n')

    @unittest.skipUnless(shutil.which('clang'), 'requires host clang')
    def test_actual_c_readiness_and_commandline_bounds(self):
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef char CHAR8;
typedef void VOID;
typedef struct {unsigned a,b,c,d[8];} EFI_GUID;
#define STATIC static
#define EFI_ERROR(status) ((status) != 0)
#define AsciiSPrint snprintf
#define AsciiStrLen strlen
#define CopyMem memcpy
#define DEBUG(arguments) ((void)0)
static unsigned good[2], wrong_size[2];
static uint64_t status[2];
static int get(const wchar_t *name, EFI_GUID *guid, void *attr, size_t *size, void *out) {
    (void)guid;(void)attr;
    unsigned which = !wcscmp(name,L"GiuliaHallClockStatus");
    struct {UINT32 stage,detail;UINT64 status;} value={good[which]?10:9,0,status[which]};
    assert(*size==sizeof(value));
    memcpy(out,&value,sizeof(value));
    *size=sizeof(value)-wrong_size[which];
    return 0;
}
static struct {int (*GetVariable)(const wchar_t *,EFI_GUID *,void *,size_t *,void *);} runtime={get}, *gRT=&runtime;
#include "hall_diagnostic.h"
int main(void) {
    char line[32];
    for(unsigned bits=0;bits<4;bits++) {
        good[0]=bits&1;good[1]=bits&2;
        strcpy(line,"-v");GiuliaAppendHallDiagnostic(line,sizeof(line));
        static const unsigned encoded[4]={612,617,678,3};
        char expect[32];snprintf(expect,sizeof(expect),"-v gh=%u",encoded[bits]);
        assert(!strcmp(line,expect));
    }
    good[0]=good[1]=1;status[0]=1;
    strcpy(line,"-v");GiuliaAppendHallDiagnostic(line,sizeof(line));assert(!strcmp(line,"-v gh=1706"));
    status[0]=0;wrong_size[1]=1;
    strcpy(line,"-v");GiuliaAppendHallDiagnostic(line,sizeof(line));assert(!strcmp(line,"-v gh=262185"));
    wrong_size[1]=0;
    memset(line,0x7e,sizeof(line));strcpy(line,"-v");
    GiuliaAppendHallDiagnostic(line,7);assert(!strcmp(line,"-v") && line[7]==0x7e);
    GiuliaAppendHallDiagnostic(line,8);assert(!strcmp(line,"-v gh=3") && line[8]==0x7e);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / 'hall_diagnostic.h').write_text(module.HELPER)
            (build / 'test.c').write_text(fixture)
            executable = build / 'test'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(build / 'test.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
