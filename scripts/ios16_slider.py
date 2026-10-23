"""Add the default-disabled Hall diagnostic worker to GUI kernels."""
import difflib


def slider_patch(profile, repository):
    patch = ('--- a/bsd/conf/files\n+++ b/bsd/conf/files\n@@ -111,3 +111,4 @@\n'
             ' bsd/dev/giulia_keys.c\t\t\tstandard\n'
             '+bsd/dev/giulia_slider.c\t\t\tstandard\n'
             ' bsd/dev/memdev.c\t\t\tstandard\n bsd/dev/mem.c\t\t\t\tstandard\n'
             '--- a/bsd/kern/bsd_init.c\n+++ b/bsd/kern/bsd_init.c\n@@ -1110,4 +1110,6 @@\n'
             ' \textern void giulia_keys_start(void);\n'
             ' \tgiulia_keys_start();\n'
             '+\textern void giulia_slider_start(void);\n+\tgiulia_slider_start();\n'
             ' #endif\n \n')
    for name in ('giulia_slider.c', 'giulia_slider.h', 'giulia_slider_pins.h'):
        data = (repository/'ios16'/name).read_text().replace('#include <stdint.h>\n', '')
        data = data.replace('#include <stddef.h>\n', '')
        patch += ''.join(difflib.unified_diff([], data.splitlines(True),
                        fromfile='/dev/null', tofile='b/bsd/dev/'+name))
    (profile/'0084-giulia-alert-slider.patch').write_text(patch)
