"""Apply the Darwin 22 boot ABI to a separate DarwinBoot source checkout."""
import argparse
from pathlib import Path

def replace(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f'Loader source drift: {old}')
    return text.replace(old, new)

def main():
    p = argparse.ArgumentParser()
    p.add_argument('driver', type=Path)
    args = p.parse_args()
    header = args.driver / 'BootArgs.h'
    source = args.driver / 'DarwinBoot.c'
    h, c = header.read_text(), source.read_text()
    for old, new in [('Darwin 23', 'Darwin 22'), ('CommandLine[1024]', 'CommandLine[608]'),
                     ('== 0x478', '== 0x2d8'), ('== 0x480', '== 0x2e0')]:
        h = replace(h, old, new)
    c = replace(c, 'CommandLine[1024]', 'CommandLine[608]')
    c = replace(c, '#define BLOCK_SIZE   SIZE_2MB', '#define BLOCK_SIZE   SIZE_32MB')
    c = replace(c, '  if (!Executable || (Kernel->Entry & 3)',
                '  Kernel->Base &= ~((UINT64)SIZE_32MB - 1);\n  if (!Executable || (Kernel->Entry & 3)')
    c = replace(c, '(Kernel->Base & EFI_PAGE_MASK)', '(Kernel->Base & (SIZE_16KB - 1))')
    c = replace(c, 'TreeAddress = ArgsAddress + EFI_PAGE_SIZE;', 'TreeAddress = ArgsAddress + SIZE_16KB;')
    c = replace(c, 'ALIGN_VALUE (TreeAddress + TreeSize, EFI_PAGE_SIZE)',
                'ALIGN_VALUE (TreeAddress + TreeSize, SIZE_16KB)')
    c = replace(c, 'ALIGN_VALUE (KeepPacked ? Request->RamdiskSize : RamdiskExpandedSize, EFI_PAGE_SIZE)',
                'ALIGN_VALUE (KeepPacked ? Request->RamdiskSize : RamdiskExpandedSize, SIZE_16KB)')
    header.write_text(h)
    source.write_text(c)

if __name__ == '__main__':
    main()
