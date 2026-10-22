/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Include after AppleCLCD's definition. The first successfully started panel
 * retains its provider until reboot, so USB captures never use a stale mapping.
 * Capture and present share a separate lock; USB transmission happens later.
 */
static AppleCLCD *giulia_screen_framebuffer;

static int giulia_screen_geometry(const AppleCLCD *fb)
{
    return fb && fb->fPixels && fb->fCaptureLock &&
        !(fb->fPixels & 3) && fb->fWidth == GD_WIDTH &&
        fb->fHeight == GD_HEIGHT && fb->fRowBytes == GD_STRIDE;
}

static void giulia_screen_register(AppleCLCD *fb)
{
    if (!giulia_screen_geometry(fb)) return;
    fb->retain();
    AppleCLCD *expected = NULL;
    if (!__atomic_compare_exchange_n(&giulia_screen_framebuffer, &expected, fb,
            0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) fb->release();
}

extern "C" int giulia_screen_capture(uint8_t *rgb565, size_t capacity,
                                    unsigned *width, unsigned *height)
{
    const unsigned step = 4, out_width = GD_WIDTH / step,
        out_height = GD_HEIGHT / step;
    const size_t bytes = (size_t)out_width * out_height * 2;
    if (width) *width = 0;
    if (height) *height = 0;
    if (!rgb565 || !width || !height || capacity < bytes) return 0;
    AppleCLCD *fb = __atomic_load_n(&giulia_screen_framebuffer, __ATOMIC_ACQUIRE);
    if (!giulia_screen_geometry(fb)) return 0;
    IOLockLock(fb->fCaptureLock);
    const volatile uint32_t *pixels = (const volatile uint32_t *)fb->fPixels;
    for (unsigned y = 0; y < out_height; y++) {
        const volatile uint32_t *row = pixels + (size_t)y * step * (GD_STRIDE / 4);
        for (unsigned x = 0; x < out_width; x++) {
            uint32_t bgra = row[x * step];
            uint16_t color = (uint16_t)(((bgra >> 19) & 31u) << 11 |
                ((bgra >> 10) & 63u) << 5 | ((bgra >> 3) & 31u));
            size_t offset = ((size_t)y * out_width + x) * 2;
            rgb565[offset] = (uint8_t)color;
            rgb565[offset + 1] = (uint8_t)(color >> 8);
        }
    }
    IOLockUnlock(fb->fCaptureLock);
    *width = out_width;
    *height = out_height;
    return 1;
}
