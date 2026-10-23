#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char output[2048];
static unsigned used, ready;
static int capture_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(output + used, sizeof(output) - used, format, args);
    va_end(args);
    assert(n >= 0 && (unsigned)n < sizeof(output) - used);
    used += (unsigned)n;
    return n;
}
static unsigned giulia_remote_ready(void) { return ready; }
#define printf capture_printf
#include "../ios16/keys/giulia_keys_terminal.inc"
#undef printf

int main(void)
{
    giulia_keys_debug();
    assert(strstr(output, "compiled=1 state=0 giulia_keys=1 buttons_service=0"));
    assert(strstr(output, "pon_apid=1024 gpio_apid=1024"));
    assert(strstr(output, "samples=0"));
    used = 0; output[0] = 0;
    ready = 7;
    GK_STATUS_SET(state, 2); GK_STATUS_SET(version, 0x70010000);
    GK_STATUS_SET(pon_apid, 533); GK_STATUS_SET(gpio_apid, 559);
    GK_STATUS_SET(status, 1); GK_STATUS_SET(pon, 0x50); GK_STATUS_SET(gpio, 1);
    GK_STATUS_SET(raw, 2); GK_STATUS_SET(stable, 2); GK_STATUS_SET(delivered, 2);
    GK_STATUS_INC(samples); GK_STATUS_INC(events); GK_STATUS_INC(retries);
    giulia_keys_debug();
    assert(strstr(output, "buttons_service=1 remote_touch=1 remote_keyboard=1 remote_buttons=1"));
    assert(strstr(output, "version=0x70010000 pon_apid=533 gpio_apid=559 status=0x1"));
    assert(strstr(output, "samples=1 pon=0x50 gpio=0x1 raw=0x2 stable=0x2 delivered=0x2 events=1 retries=1"));
    used = 0; output[0] = 0;
    ready = 2;
    GK_STATUS_SET(state, 5); GK_STATUS_SET(error, 5); GK_STATUS_INC(errors);
    GK_STATUS_SET(delivered, 0); GK_STATUS_INC(events);
    giulia_keys_debug();
    assert(strstr(output, "state=5 giulia_keys=1 buttons_service=0 remote_touch=0 remote_keyboard=1 remote_buttons=0"));
    assert(strstr(output, "error=5 thread_error=0 errors=1"));
    assert(strstr(output, "delivered=0 events=2 retries=1"));
    used = 0; output[0] = 0;
    GK_STATUS_SET(enabled, -1); GK_STATUS_SET(state, 3);
    giulia_keys_debug();
    assert(strstr(output, "state=3 giulia_keys=-1"));
    assert(strstr(output, "cache=approximate"));
    /* This real query links without MMIO, SPMI, mapping or button functions. */
    puts("Physical-key status: startup, cached samples/counters, provider readiness, fault and disabled queries passed without hardware access");
    return 0;
}
