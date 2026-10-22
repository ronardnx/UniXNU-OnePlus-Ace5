"""Generate the native-panel presentation patch against the applied GUI series."""
import difflib
from pathlib import Path
import subprocess
import tempfile


def native_display_patch(profile: Path, repository: Path):
    targets = ('iokit/Kernel/arm/SweetFramebuffer.cpp',
               'iokit/Kernel/arm/hid/SweetHID.cpp')
    with tempfile.TemporaryDirectory(prefix='giulia-display-') as temp:
        work = Path(temp)
        subprocess.run(['git', '-C', temp, 'init', '-q'], check=True)
        for patch in sorted(profile.glob('*.patch')):
            if int(patch.name[:4]) >= 75:
                continue
            subprocess.run(['git', '-C', temp, 'apply', '--whitespace=nowarn',
                            *['--include=' + p for p in targets], str(patch.resolve())],
                           check=True, capture_output=True)
        old = [(work / p).read_text(errors='surrogateescape') for p in targets]
    fb, hid = old

    def replace(text, before, after):
        if text.count(before) != 1:
            raise ValueError('Display source drift: ' + repr(before[:100]))
        return text.replace(before, after)

    fb = replace(fb, '#include "SweetSurface.h"',
                 '#include "SweetSurface.h"\n#include "giulia_display.h"')
    fb = replace(fb, '\tint32_t       fOffX, fOffY; /* logical origin relative to the panel */\n', '')
    fb = replace(fb, '\tIOLock       *fLock;            /* swaps and client list */\n',
                 '\tIOLock       *fLock;            /* swaps and client list */\n'
                 '\tIOLock       *fCaptureLock;     /* panel writes and USB snapshots */\n')
    fb = replace(fb, 'OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);\n',
                 'OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);\n\n'
                 '#include "giulia_screen_framebuffer.inc"\n')
    fb = replace(fb, '\t/* Known UIKit layout, centered 1:1 inside the guarded Ace 5 panel. */\n'
                 '\tfOffX = (VERMEER_LOGICAL_W - (int32_t)fWidth) / 2;\n'
                 '\tfOffY = (VERMEER_LOGICAL_H - (int32_t)fHeight) / 2;\n',
                 '\tsetProperty("GiuliaNativeWidth", GD_WIDTH, 32);\n'
                 '\tsetProperty("GiuliaNativeHeight", GD_HEIGHT, 32);\n'
                 '\tsetProperty("GiuliaPresentation", "full-panel");\n')
    fb = replace(fb, 'logical canvas 1125x2436 centered in 1264x2780',
                 'native panel 1264x2780 full coverage; UIKit canvas 1125x2436')
    fb = replace(fb, '/* Clear the border once; all writes stay inside the guarded reservation. */',
                 '/* Clear stale console pixels before the first full-panel frame. */')
    fb = replace(fb, '\tfVsync->setTimeoutUS(SWEET_FB_VSYNC_US);\n',
                 '\tfCaptureLock = IOLockAlloc();\n'
                 '\tif (fCaptureLock == NULL) {\n'
                 '\t\tIOLog("GIULIA SCREEN: panel capture lock unavailable\\n");\n'
                 '\t\treturn false;\n\t}\n'
                 '\tfVsync->setTimeoutUS(SWEET_FB_VSYNC_US);\n')
    fb = replace(fb, '\tregisterService();\n',
                 '\tgiulia_screen_register(this);\n\tregisterService();\n')
    fb = replace(fb, 'AppleCLCD::present(const uint8_t *rec)\n{\n',
                 'AppleCLCD::present(const uint8_t *rec)\n{\n'
                 '\tif (fCaptureLock == NULL || fPixels == 0) return;\n'
                 '\tIOLockLock(fCaptureLock);\n')
    begin = fb.index('\t\tuintptr_t src = s->kernelAddress();')
    end = fb.index('\t\ts->release();', begin)
    fb = fb[:begin] + '''\t\tuintptr_t address = s->kernelAddress();
\t\tuint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;
\t\tint32_t sw = (int32_t)get32(a, 8), sh = (int32_t)get32(a, 12);
\t\tif (sw <= 0 || sh <= 0) { sw = (int32_t)s->width; sh = (int32_t)s->height; }
\t\tif (w <= 0 || h <= 0) { w = sw; h = sh; }
\t\tif (address && bpe == 4 && s->elementWidth == 1 && s->elementHeight == 1 &&
\t\t    s->pixelFormat == SWEET_FB_FORMAT_BGRA && s->memory) {
\t\t\tstruct gd_rect source = { sx, sy, sw, sh }, destination = { dx, dy, w, h };
\t\t\t(void)gd_present((void *)fPixels, (size_t)fRowBytes * fHeight,
\t\t\t    (const void *)address, s->memory->getLength(), s->offset,
\t\t\t    s->width, s->height, s->bytesPerRow, source, destination);
\t\t}
''' + fb[end:]
    fb = replace(fb, '\n}\n\nbool\nIOMobileFramebufferUserClient::initWithTask',
                 '\n\tIOLockUnlock(fCaptureLock);\n}\n\nbool\n'
                 'IOMobileFramebufferUserClient::initWithTask')
    fb = fb.replace('/* Copy each enabled layer\'s surface into the panel memory (no scaling). */',
                    '/* Present enabled layers over the native panel using the shared bounded transform. */')
    hid = replace(hid, '\t/* Translate physical panel coordinates into the centered UIKit canvas. */\n'
                  '\tint32_t lx = x - (1264 - 1125) / 2, ly = y - (2780 - 2436) / 2;\n'
                  '\tlx = lx < 0 ? 0 : (lx > 1124 ? 1124 : lx);\n'
                  '\tly = ly < 0 ? 0 : (ly > 2435 ? 2435 : ly);\n'
                  '\tIOFixed nx = (IOFixed)(((int64_t)lx << 16) / 1125);\n'
                  '\tIOFixed ny = (IOFixed)(((int64_t)ly << 16) / 2436);',
                  '\t/* Same full-panel transform as SweetFramebuffer; no centered offset. */\n'
                  '\tIOFixed nx = (IOFixed)gd_touch(x, GD_WIDTH);\n'
                  '\tIOFixed ny = (IOFixed)gd_touch(y, GD_HEIGHT);')
    # The type definitions used by the portable helper have already been
    # included by the existing HID source at this point.
    marker = 'class SweetHIDTouch'
    pos = hid.index(marker)
    hid = hid[:pos] + '#include "../giulia_display.h"\n\n' + hid[pos:]
    output = ''
    for name, before, after in zip(targets, old, (fb, hid)):
        output += ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                             fromfile='a/' + name, tofile='b/' + name))
    header = (repository / 'ios16/giulia_display.h').read_text()
    header = header.replace('#include <stdint.h>\n', '').replace('#include <stddef.h>\n', '')
    output += ''.join(difflib.unified_diff([], header.splitlines(True), fromfile='/dev/null',
                                         tofile='b/iokit/Kernel/arm/giulia_display.h'))
    capture = (repository / 'ios16/giulia_screen_framebuffer.inc').read_text()
    output += ''.join(difflib.unified_diff([], capture.splitlines(True), fromfile='/dev/null',
                                         tofile='b/iokit/Kernel/arm/giulia_screen_framebuffer.inc'))
    (profile / '0075-giulia-native-panel.patch').write_text(output, errors='surrogateescape')
