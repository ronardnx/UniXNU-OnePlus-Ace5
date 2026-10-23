"""Add the measured Ace 5 physical keys to every GUI profile."""
import difflib


def keys_patch(profile, repository):
    touch = (profile / '0069-giulia-touch.patch').is_file()
    touch_file = ' bsd/dev/giulia_touch.c\t\t\tstandard\n' if touch else ''
    touch_start = (' \textern void giulia_touch_start(void);\n \tgiulia_touch_start();\n') if touch else ''
    patch = ('--- a/bsd/conf/files\n+++ b/bsd/conf/files\n'
             f'@@ -109,{4 + touch} +109,{5 + touch} @@\n'
             ' bsd/dev/vermeer_usb_debug.c\t\tstandard\n'
             ' bsd/dev/sweet_sample.c\t\t\tstandard\n'
             + touch_file +
             '+bsd/dev/giulia_keys.c\t\t\tstandard\n'
             ' bsd/dev/memdev.c\t\t\tstandard\n bsd/dev/mem.c\t\t\t\tstandard\n'
             '--- a/bsd/kern/bsd_init.c\n+++ b/bsd/kern/bsd_init.c\n'
             f'@@ -1106,{5 + 2 * touch} +1106,{7 + 2 * touch} @@\n'
             ' \tvermeer_usb_debug_start();\n \textern void sweet_sample_start(void);\n'
             ' \tsweet_sample_start();\n' + touch_start +
             '+\textern void giulia_keys_start(void);\n+\tgiulia_keys_start();\n'
             ' #endif\n \n')
    for name in ('giulia_keys.c', 'giulia_keys.h', 'giulia_keys_terminal.inc'):
        data = (repository / 'ios16' / name).read_text().replace('#include <stdint.h>\n', '')
        patch += ''.join(difflib.unified_diff([], data.splitlines(True),
                        fromfile='/dev/null', tofile='b/bsd/dev/' + name))
    (profile / '0083-giulia-physical-keys.patch').write_text(patch)
