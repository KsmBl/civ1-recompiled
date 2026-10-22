/* PC speaker square wave generator; AdLib registers are accepted but the
   OPL synthesis lives in opl.c when built with it. */
#include <stdint.h>
#include "audio.h"

static int out_rate = 44100;
static volatile int spk_on;
static volatile uint32_t spk_step;      /* 16.16 phase increment */
static uint32_t spk_phase;
static uint8_t adlib_idx;
static uint8_t adlib_timer_ctl;

void audio_init(int rate)
{
    out_rate = rate;
}

void speaker_set(uint8_t port61, uint16_t divisor)
{
    uint32_t d = divisor ? divisor : 65536;
    double freq = 1193182.0 / d;
    spk_on = (port61 & 3) == 3 && freq > 20 && freq < 20000;
    spk_step = (uint32_t)(freq * 65536.0 * 2 / out_rate);
}

void audio_render(int16_t *out, int frames)
{
    for (int i = 0; i < frames; i++) {
        int16_t s = 0;
        if (spk_on) {
            spk_phase += spk_step;
            s = (spk_phase & 0x10000) ? 3000 : -3000;
        }
        out[i] = s;
    }
}

uint8_t adlib_status(void)
{
    /* timer flags so that AdLib detection succeeds when timers are run */
    if (adlib_timer_ctl & 0x01)
        return 0xC0;
    return 0x00;
}

void adlib_write_index(uint8_t v)
{
    adlib_idx = v;
}

void adlib_write_data(uint8_t v)
{
    if (adlib_idx == 4) {
        if (v & 0x80)
            adlib_timer_ctl = 0;
        else
            adlib_timer_ctl = v;
    }
}
