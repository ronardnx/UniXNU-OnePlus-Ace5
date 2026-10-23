"""Add the measured Ace 5 physical keys to every GUI profile."""
import difflib


def keys_patch(profile, repository):
    patch = ('--- a/bsd/conf/files\n+++ b/bsd/conf/files\n@@ -1 +1,2 @@\n'
             ' bsd/dev/sweet_sample.c\t\t\tstandard\n'
             '+bsd/dev/giulia_keys.c\t\t\tstandard\n'
             '--- a/bsd/kern/bsd_init.c\n+++ b/bsd/kern/bsd_init.c\n@@ -1 +1,3 @@\n'
             ' \tsweet_sample_start();\n'
             '+\textern void giulia_keys_start(void);\n+\tgiulia_keys_start();\n')
    for name in ('giulia_keys.c', 'giulia_keys.h', 'giulia_keys_terminal.inc'):
        data = (repository / 'ios16' / name).read_text().replace('#include <stdint.h>\n', '')
        patch += ''.join(difflib.unified_diff([], data.splitlines(True),
                        fromfile='/dev/null', tofile='b/bsd/dev/' + name))
    (profile / '0083-giulia-physical-keys.patch').write_text(patch)
