/* Exercise the same metadata command body linked into the USB translation unit. */
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char audio_output[32768];
static size_t audio_output_bytes;
static int audio_test_printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int bytes=vsnprintf(audio_output+audio_output_bytes,
                       sizeof(audio_output)-audio_output_bytes,format,arguments);
    va_end(arguments);
    assert(bytes>=0 && (size_t)bytes<sizeof(audio_output)-audio_output_bytes);
    audio_output_bytes+=(size_t)bytes;
    return bytes;
}

/* XNU supplies integer typedefs but does not guarantee libc stdint macros. */
#ifdef GAUDIO_KERNEL_EMBEDDED
#undef UINT32_C
#undef UINT64_C
#undef UINT32_MAX
#undef UINT64_MAX
#undef UINTPTR_MAX
#undef SIZE_MAX
#endif
#define printf audio_test_printf
#ifdef GAUDIO_KERNEL_EMBEDDED
#include "audio-generated.h"
#else
static int giulia_audio_runtime_command(const char *cmd) {(void)cmd;return 0;}
#include "../ios16/audio/giulia_audio_mapping.h"
#include "../ios16/audio/giulia_audio_terminal.inc"
#endif
#undef printf

static void clear_output(void)
{
    audio_output_bytes=0;
    audio_output[0]=0;
}

int main(void)
{
    const char *invalid[]={"", "audio", "audio.start", "audio.play", "audio.record",
                           "audio.status ", "audio.map now", "audio.map;ping",
                           "audio.status\nping", "AUDIO.map", NULL};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        assert(!gaudio_command(invalid[i]));
        assert(audio_output_bytes==0);
    }
    assert(gaudio_command("audio.status"));
    assert(strstr(audio_output,"GIULIA AUDIO:"));
    assert(strstr(audio_output,"stage=board-mapping-only"));
    assert(strstr(audio_output,"hardware_ready=0"));
    assert(strstr(audio_output,"playback=0"));
    assert(strstr(audio_output,"recording=0"));
    assert(!strstr(audio_output,"resource="));
    clear_output();
    assert(gaudio_command("audio.map"));
    assert(strstr(audio_output,"GIULIA AUDIO:"));
    assert(strstr(audio_output,"dts="));
    assert(strstr(audio_output,"source="));
    assert(strstr(audio_output,"kind=reserved-dsp-ram"));
    assert(strstr(audio_output,"android_enabled=0"));
    assert(strstr(audio_output,"audio_sid="));
    assert(strstr(audio_output,"xnu_dma_owned=0"));
    assert(strstr(audio_output,"speakers=SIA9177-left+right interface=secondary-MI2S"));
    assert(strstr(audio_output,"codec=WCD939x wsa884x=disabled"));
    assert(strstr(audio_output,"reset_gpio=unspecified"));
    assert(!strstr(audio_output,"reset_gpio=0\n"));
    assert(audio_output_bytes>0 && audio_output_bytes<sizeof(audio_output));
    puts("Audio metadata terminal checks passed");
    return 0;
}
