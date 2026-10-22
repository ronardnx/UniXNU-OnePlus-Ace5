"""Apply the generated USB patch chain, including the later GUI log patch."""
from pathlib import Path
import subprocess,sys,tempfile
profile=Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as folder:
    root=Path(folder)
    subprocess.run(['git','init','-q',str(root)],check=True)
    for patch in sorted(profile.glob('*.patch')):
        subprocess.run(['git','apply','--whitespace=nowarn',
            '--include=bsd/dev/vermeer_usb_debug.c',str(patch)],cwd=root,check=True)
    source=(root/'bsd/dev/vermeer_usb_debug.c').read_text()
    assert 'giulia_terminal_command(u.command)' in source
    assert 'GIULIA SHELL: /dev/giulia-debug ready;' in source
    assert 'GIULIA REG: mapped slot=' in source
    assert 'gr_command(cmd)' in source
    assert 'reboot_kernel(RB_AUTOBOOT,NULL)' in source
    assert 'int giulia_reboot_finish(void)' in source
    assert '0x84000009u' in source
    assert (profile/'0082-giulia-fastboot-reboot.patch').is_file()
    if (profile/'0073-giulia-complete-boot-log.patch').exists():
        assert 'ring_bytes=33554432' in source
print('USB terminal and complete-log patch chain applied successfully')
