"""Minimal upstream anchors for applying real optional-driver patch sets."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_modules import emit_optional_modules
from ios16_wifi_sae import BEARSSL_PIN


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8', newline='\n')


def apply_modules(folder, selected):
    write(folder / 'iokit/conf/files.arm64', '\n' * 13 +
          'iokit/Kernel/arm/SweetKeyStore.cpp                               optional iokitcpp\n'
          'iokit/Kernel/arm/GiuliaAMFI.cpp                                  optional iokitcpp\n'
          'iokit/Kernel/arm/SweetWatchdog.cpp                               optional iokitcpp\n')
    write(folder / 'iokit/KernelConfigTables.cpp', '\n' * 54 +
          '    "     \'IOMatchCategory\' = AppleMobileFileIntegrity;"\n'
          '    "   },"\n    "   {"\n    "     \'IOClass\' = ExistingResource;"\n')
    write(folder / 'libkern/conf/files', '\n' * 98 +
          'libkern/crypto/corecrypto_aes.c\t\t\toptional crypto\n'
          'libkern/crypto/sweet_aes.c\toptional crypto\n'
          'libkern/crypto/sweet_aes_ct.c\toptional crypto\n'
          'libkern/crypto/sweet_aes_ct_enc.c\toptional crypto\n')
    write(folder / 'libkern/conf/Makefile.template', '\n' * 25 +
          'sweet_aes.o_INCFLAGS = $(SWEET_AES_INC)\n'
          'sweet_aes_ct.o_INCFLAGS = $(SWEET_AES_INC)\n'
          'sweet_aes_ct_enc.o_INCFLAGS = $(SWEET_AES_INC)\n'
          'sweet_aes_ct_dec.o_INCFLAGS = $(SWEET_AES_INC)\n')
    write(folder / '0020-stage-bearssl.sh', BEARSSL_PIN + '\n' +
          "shutil.copy2(source / 'LICENSE.txt', target / 'LICENSE.txt')\n")
    registry = emit_optional_modules(folder, ROOT, selected)
    for patch in sorted(folder.glob('*.patch')):
        # IRQ/pmap kernel hooks are verified against the pinned full upstream tree.
        if patch.name.startswith(('0085-', '0086-')):
            continue
        for flags in (['--check'], []):
            subprocess.run(['git', 'apply', *flags, str(patch)], cwd=folder,
                           capture_output=True, text=True, check=True)
    return registry
