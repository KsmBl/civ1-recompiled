/* Sound hardware emulation: PC speaker and AdLib (OPL2). */
#pragma once
#include <stdint.h>

void audio_init(int rate);
void audio_render(int16_t *out, int frames);   /* called from audio thread */
void speaker_set(uint8_t port61, uint16_t divisor);
uint8_t adlib_status(void);
void adlib_write_index(uint8_t v);
void adlib_write_data(uint8_t v);
