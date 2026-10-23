#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "../ios16/display/giulia_display.h"

struct IOLock { bool held; unsigned locks, unlocks; };
static void IOLockLock(IOLock *lock)
{ assert(lock && !lock->held); lock->held = true; lock->locks++; }
static void IOLockUnlock(IOLock *lock)
{ assert(lock && lock->held); lock->held = false; lock->unlocks++; }
struct AppleCLCD {
    uintptr_t fPixels;
    uint32_t fWidth, fHeight, fRowBytes;
    IOLock *fCaptureLock;
    unsigned references;
    void retain() { references++; }
    void release() { assert(references); references--; }
};
#include "../ios16/display/giulia_screen_framebuffer.inc"

int main()
{
    const size_t frame_bytes = (size_t)(GD_WIDTH / 4) * (GD_HEIGHT / 4) * 2;
    std::vector<uint8_t> frame(frame_bytes + 32, 0xa5);
    unsigned width = 99, height = 99;
    assert(!giulia_screen_capture(frame.data(), frame_bytes, &width, &height));
    assert(width == 0 && height == 0);
    IOLock lock = {};
    std::vector<uint32_t> panel((size_t)GD_WIDTH * GD_HEIGHT);
    for (unsigned y = 0; y < GD_HEIGHT; y++)
        for (unsigned x = 0; x < GD_WIDTH; x++)
            panel[(size_t)y * GD_WIDTH + x] = 0xff000000u |
                ((x & 255u) << 16) | ((y & 255u) << 8) | ((x + y) & 255u);
    AppleCLCD fb = {(uintptr_t)panel.data(), GD_WIDTH, GD_HEIGHT, GD_STRIDE, &lock, 1};
    fb.fWidth--;
    giulia_screen_register(&fb);
    assert(fb.references == 1 && !giulia_screen_framebuffer);
    fb.fWidth++;
    giulia_screen_register(&fb);
    assert(fb.references == 2 && giulia_screen_framebuffer == &fb);
    AppleCLCD second = fb; second.references = 1;
    giulia_screen_register(&second);
    assert(second.references == 1 && giulia_screen_framebuffer == &fb);
    assert(!giulia_screen_capture(frame.data(), frame_bytes - 1, &width, &height));
    assert(!lock.locks && frame.front() == 0xa5 && frame.back() == 0xa5);
    assert(!giulia_screen_capture(frame.data(), frame_bytes, NULL, &height));
    assert(!giulia_screen_capture(NULL, frame_bytes, &width, &height));
    assert(!giulia_screen_capture(frame.data(), frame_bytes, &width, NULL));
    assert(giulia_screen_capture(frame.data(), frame_bytes, &width, &height));
    assert(width == 316 && height == 695 && lock.locks == 1 && lock.unlocks == 1 && !lock.held);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            unsigned sx = x * 4, sy = y * 4;
            uint16_t expected = (uint16_t)(((sx & 255u) >> 3) << 11 |
                ((sy & 255u) >> 2) << 5 | (((sx + sy) & 255u) >> 3));
            size_t at = ((size_t)y * width + x) * 2;
            assert(frame[at] == (uint8_t)expected && frame[at + 1] == (uint8_t)(expected >> 8));
        }
    for (size_t i = frame_bytes; i < frame.size(); i++) assert(frame[i] == 0xa5);
    assert(!giulia_screen_capture_size(frame.data(), frame_bytes, &width, &height, 159));
    assert(!giulia_screen_capture_size(frame.data(), frame_bytes, &width, &height, 2781));
    const size_t native_bytes = (size_t)GD_WIDTH * GD_HEIGHT * 2;
    std::vector<uint8_t> native(native_bytes + 32, 0xa5);
    assert(giulia_screen_capture_size(native.data(), native_bytes, &width, &height, 2780));
    assert(width == GD_WIDTH && height == GD_HEIGHT);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            uint16_t expected = (uint16_t)(((x & 255u) >> 3) << 11 |
                ((y & 255u) >> 2) << 5 | (((x + y) & 255u) >> 3));
            size_t at = ((size_t)y * width + x) * 2;
            assert(native[at] == (uint8_t)expected && native[at + 1] == (uint8_t)(expected >> 8));
        }
    for (size_t i = native_bytes; i < native.size(); i++) assert(native[i] == 0xa5);
    assert(giulia_screen_capture_size(frame.data(), frame_bytes, &width, &height, 160));
    assert(width == 72 && height == 160);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            unsigned sx = x * GD_WIDTH / width, sy = y * GD_HEIGHT / height;
            uint16_t expected = (uint16_t)(((sx & 255u) >> 3) << 11 |
                ((sy & 255u) >> 2) << 5 | (((sx + sy) & 255u) >> 3));
            size_t at = ((size_t)y * width + x) * 2;
            assert(frame[at] == (uint8_t)expected && frame[at + 1] == (uint8_t)(expected >> 8));
        }
    fb.fRowBytes -= 4;
    assert(!giulia_screen_capture(frame.data(), frame_bytes, &width, &height));
    assert(width == 0 && height == 0 && lock.locks == 3 && lock.unlocks == 3 && !lock.held);
    puts("Screen framebuffer: native/downsampled real BGRA samples, RGB565 LE, exact bounds, shared lock and provider lifetime passed");
    return 0;
}
