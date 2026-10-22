"""Attach bounded terminal queries to the pinned CDC and UFS owners."""
from pathlib import Path
import json
import re


def debug_trust_source(manifest):
    data=json.loads(Path(manifest).read_text())
    if data.get('schema')!=1 or data.get('purpose')!='giulia-debug-userland':
        raise ValueError('Expected stage-debug-userland manifest')
    files=data.get('files',{})
    if not {'/usr/local/giulia/giulia-debugd','/usr/local/giulia/bin/sh'} <= files.keys():
        raise ValueError('Debug manifest lacks daemon or shell')
    hashes=set()
    for name, record in files.items():
        if not re.fullmatch(r'/usr/local/giulia/(?:giulia-debugd|(?:bin|lib)/[A-Za-z0-9_.+-]+)',name):
            raise ValueError('Debug trust path outside staged bundle')
        if not re.fullmatch('[0-9a-f]{64}',record.get('sha256','')):
            raise ValueError('Missing binary digest')
        values=record.get('cdhashes',[])
        if not values or any(not re.fullmatch('[0-9a-f]{40}',h) for h in values):
            raise ValueError('Invalid CodeDirectory hash')
        hashes.update(values)
    if not 1<=len(hashes)<=256:raise ValueError('Debug trust list exceeds 256 hashes')
    rows=',\n'.join('{'+','.join('0x'+h[i:i+2] for i in range(0,40,2))+'}' for h in sorted(hashes))
    return ('#include <pexpert/pexpert.h>\n'
            'static bool giulia_debug_trusted(const void *hash, size_t length) {\n'
            '    int enabled=0; PE_parse_boot_argn("giulia_debug",&enabled,sizeof(enabled));\n'
            '    if(enabled!=1 || !hash || length!=20) return false;\n'
            '    static const unsigned char hashes[][20]={'+rows+'};\n'
            '    for(unsigned i=0;i<sizeof(hashes)/sizeof(hashes[0]);i++)\n'
            '        if(!memcmp(hash,hashes[i],20)) return true;\n'
            '    return false;\n}\n')


def terminal_sources(usb: Path, block: Path, edit, wifi=False, gui=False):
    if 'giulia_terminal_command' in usb.read_text():
        raise ValueError('Terminal integration already applied')
    root = Path(__file__).resolve().parents[1]
    abi = (root/'ios16/giulia_debug_abi.h').read_text().replace('#include <stdint.h>\n', '')
    shell = (root/'ios16/giulia_shell.inc').read_text().replace(
        '/* GIULIA_DEBUG_ABI is embedded here by the profile generator. */', abi)
    reboot = (root/'ios16/giulia_reboot.inc').read_text().replace(
        '/* GIULIA_REBOOT_CORE is embedded here by the profile generator. */',
        (root/'ios16/giulia_reboot.h').read_text().replace('#include <stdint.h>\n', ''))
    screen = (root/'ios16/giulia_screen.inc').read_text().replace(
        '/* GIULIA_SCREEN_CORE is embedded here by the profile generator. */',
        (root/'ios16/giulia_screen.h').read_text().replace('#include <stdint.h>\n','').replace('#include <stddef.h>\n',''))
    screen = screen.replace('/* GIULIA_SCREEN_PROVIDER is embedded here by the profile generator. */',
        'extern int giulia_screen_capture(uint8_t *,size_t,unsigned *,unsigned *);' if gui else
        'static int giulia_screen_capture(uint8_t *p,size_t n,unsigned *w,unsigned *h) '
        '{(void)p;(void)n;(void)w;(void)h;return 0;}')
    code = ('extern unsigned long gPhysBase, gPhysSize;\n' + shell + '\n' +
            (root/'ios16/giulia_mmio.inc').read_text() + '\n' +
            reboot + '\n' + screen + '\n' +
            (root / 'ios16/giulia_terminal.inc').read_text())
    if wifi:
        code='extern int giulia_wifi_terminal(const char *);\n'+code
        code=code.replace('    if(gs_command(cmd) || gm_command(cmd) || gr_command(cmd)) return 1;',
                          '    if(giulia_wifi_terminal(cmd) || gs_command(cmd) || gm_command(cmd) || gr_command(cmd)) return 1;')
    edit(usb, 'static void command_line(void)', code + '\nstatic void command_line(void)')
    # Keep ping/sample/status adjacent: the complete-boot-log patch uses them
    # as context when it extends the status response later in the GUI profile.
    edit(usb, '    else if (!strcmp(u.command,"ping"))',
         '    else if (giulia_terminal_command(u.command)) {}\n'
         '    else if (!strcmp(u.command,"ping"))')
    edit(usb, 'commands help status ping replay',
         'commands help status ping replay info uptime memory ps bootargs display usb ufs features shell shell.size reg.* reboot.fastboot reboot.status screen.start screen.stop screen.snapshot')
    edit(usb, '; queries only', '; shell/MMIO require development boot args')
    edit(usb, '(read only)', '(development protocol 2)')
    edit(usb, 'RAM log and read-only commands', 'RAM log and protocol 2 terminal')
    edit(usb, 'USB can read the RAM console ring and issue four bounded queries only.',
         'USB exposes log queries plus opt-in root PTY and explicit MMIO access.')
    edit(usb, '    sync_dma(RX,RX_SIZE);u.rx_bytes+=length;',
         '    sync_dma(RX,RX_SIZE);u.rx_bytes+=length;\n'
         '    if (gs_receive((const uint8_t *)(u.dma+RX),length)) { receive_arm(); return; }')
    edit(usb, '    unsigned n=vermeer_debug_copy(&u.cursor,(char *)(u.dma+TX),TX_SIZE);',
         '    unsigned screen_n=0;\n'
         '    if (gscreen_transmit((uint8_t *)(u.dma+TX),TX_SIZE-1,&screen_n)) {\n'
         '        if(screen_n) { transfer(3,TX,screen_n,T_NORMAL);u.tx_bytes+=screen_n; }\n'
         '        return;\n    }\n'
         '    unsigned shell_n=0;\n'
         '    if (gs_transmit((char *)(u.dma+TX),u.mps-1,&shell_n)) {\n'
         '        if(shell_n) { transfer(3,TX,shell_n,T_NORMAL);u.tx_bytes+=shell_n; }\n'
         '        return;\n    }\n'
         '    unsigned n=vermeer_debug_copy(&u.cursor,(char *)(u.dma+TX),TX_SIZE);')
    edit(usb, '    while (!u.failed) {', '    gs_register();\n    while (!u.failed) {')
    edit(usb, '        transmit();',
         '        if (!u.configured || !u.dtr) {gs_disconnect();gscreen_disconnect();}\n        transmit();')
    edit(usb, '    wr(DEVTEN,0);wr(DCTL,rd(DCTL)&~RUN);\n    thread_terminate(current_thread());',
         '    gs_disconnect();gscreen_disconnect();\n    wr(DEVTEN,0);wr(DCTL,rd(DCTL)&~RUN);\n    thread_terminate(current_thread());')
    code = (root / 'ios16/giulia_terminal_ufs.inc').read_text()
    edit(block, 'static uint64_t vb_device_bytes(void)', code + '\nstatic uint64_t vb_device_bytes(void)')
