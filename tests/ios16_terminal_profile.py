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
    assert 'if(gaudio_command(cmd))return 1;' in source
    assert 'static int gaudio_command(const char *cmd)' in source
    assert 'audio.status audio.map' in source
    assert 'if(!strcmp(cmd,"keys.status")) {giulia_keys_debug();return 1;}' in source
    if (profile/'0083-giulia-physical-keys.patch').is_file():
        assert '#define GIULIA_KEYS_INCLUDED 1' in source
        assert 'extern void giulia_keys_debug(void);' in source
        keys=(profile/'0083-giulia-physical-keys.patch').read_text()
        assert 'b/bsd/dev/giulia_keys_terminal.inc' in keys
    else:
        assert '#define GIULIA_KEYS_INCLUDED 0' in source
        assert 'GIULIA KEYS: compiled=0 service=unavailable' in source
    assert 'speakers=SIA9177-left+right interface=secondary-MI2S' in source
    assert 'GIULIA SHELL: /dev/giulia-debug ready;' in source
    assert 'GIULIA REG: mapped slot=' in source
    assert 'gr_command(cmd)' in source
    assert 'reboot_kernel(RB_AUTOBOOT,NULL)' in source
    assert 'int giulia_reboot_finish(void)' in source
    assert '0x84000009u' in source
    assert (profile/'0082-giulia-fastboot-reboot.patch').is_file()
    if (profile/'0073-giulia-complete-boot-log.patch').exists():
        assert (profile/'0070-giulia-service-images.patch').is_file()
        assert 'ring_bytes=33554432' in source
print('USB terminal and complete-log patch chain applied successfully')
