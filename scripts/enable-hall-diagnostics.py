"""Carry the optional native Hall preparation result into a bounded boot argument.

gh bits: 0 = native ldob1 ACTIVE vote completed; 1 = all native I2C hub8
clock requests and readbacks completed. gh=3 permits further runtime preflight;
it does not claim that pins, I2C, sensor identity or positions were verified.
"""
from pathlib import Path
import argparse

HELPER = '''
STATIC VOID GiuliaAppendHallDiagnostic (CHAR8 *Line, UINTN Capacity)
{
  EFI_GUID Guid = {0x1c87b711,0xeaa6,0x42cb,{0xa3,0xd5,0x8c,0x21,0x19,0x65,0x21,0x31}};
  struct { UINT32 Stage, Detail; UINT64 Status; } Power, Clock;
  UINTN Size = sizeof(Power), Used, Length;
  UINT32 Ready = 0;
  CHAR8 Extra[12];
  if (!EFI_ERROR(gRT->GetVariable(L"GiuliaHallPowerStatus",&Guid,NULL,&Size,&Power)) &&
      Size == sizeof(Power) && Power.Stage == 10 && Power.Status == 0) { Ready |= 1; }
  Size = sizeof(Clock);
  if (!EFI_ERROR(gRT->GetVariable(L"GiuliaHallClockStatus",&Guid,NULL,&Size,&Clock)) &&
      Size == sizeof(Clock) && Clock.Stage == 10 && Clock.Status == 0) { Ready |= 2; }
  AsciiSPrint(Extra,sizeof(Extra)," gh=%u",Ready);
  Used=AsciiStrLen(Line);Length=AsciiStrLen(Extra);
  if (Used >= Capacity || Length >= Capacity-Used) { return; }
  CopyMem(Line+Used,Extra,Length+1);
  DEBUG((DEBUG_ERROR,"GIULIA HALL: native-preparation bits=%u; pins and sensor unverified\\n",Ready));
}

'''
CALL = '  GiuliaAppendHallDiagnostic (CommandLine, sizeof (CommandLine));\n'


def install(driver):
    source = driver / 'DarwinBoot.c'
    inf = driver / 'DarwinBoot.inf'
    text, spec = source.read_text(), inf.read_text()
    marker = '  CopyMem (Args->CommandLine, CommandLine, sizeof (CommandLine));'
    if text.count(marker) != 1:
        raise ValueError('DarwinBoot command-line anchor changed')
    if CALL in text:
        if text.count(CALL) != 1 or text.count('STATIC VOID GiuliaAppendHallDiagnostic') != 1:
            raise ValueError('Ambiguous existing Hall diagnostic integration')
        return
    if 'GiuliaAppendHallDiagnostic' in text:
        raise ValueError('Incomplete existing Hall diagnostic integration')
    include = '#include <Library/UefiRuntimeServicesTableLib.h>'
    if include not in text:
        anchor = '#include <Library/PrintLib.h>'
        if text.count(anchor) != 1:
            raise ValueError('DarwinBoot include anchor changed')
        text = text.replace(anchor, anchor + '\n' + include)
    lines = text.splitlines(True)
    at = max(i for i, line in enumerate(lines) if line.startswith('#include ')) + 1
    lines.insert(at, HELPER)
    text = ''.join(lines).replace(marker, CALL + marker)
    if '  UefiRuntimeServicesTableLib' not in spec:
        anchor = '  UefiBootServicesTableLib'
        if spec.count(anchor) != 1:
            raise ValueError('DarwinBoot library anchor changed')
        spec = spec.replace(anchor, anchor + '\n  UefiRuntimeServicesTableLib')
    source.write_text(text, encoding='utf-8', newline='\n')
    inf.write_text(spec, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('driver', type=Path)
    install(parser.parse_args().driver)
    print('Native Hall preparation diagnostics installed; gh=3 only permits runtime preflight')
