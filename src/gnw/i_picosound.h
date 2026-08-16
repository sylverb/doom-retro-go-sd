//
// GNW replacement for src/pico/i_picosound.h (same include name so engine
// files compile untouched). The pico_audio buffer-pool types collapse to a
// simple mono span into our SAI mix pipeline (i_sound_gnw.c).
//
#ifndef __I_PICO_SOUND__
#define __I_PICO_SOUND__

#include "pico.h"

// Mono 48 kHz span; the music generator FILLS samples[0..max_sample_count),
// SFX channels then add on top (i_sound_gnw.c).
typedef struct audio_buffer {
    int16_t *samples;
    uint32_t max_sample_count;
    uint32_t sample_count;
} audio_buffer_t;

// SFX channel stepping and low-pass alpha resample straight to the SAI rate.
// The OPL emulator still runs at its native 49716 Hz; opl_gnw.c owns that
// resample (16.16 linear, the hardware-validated opl_stm32.c scheme).
#define PICO_SOUND_SAMPLE_FREQ 48000

#ifndef NUM_SOUND_CHANNELS
#define NUM_SOUND_CHANNELS 8
#endif

void I_PicoSoundSetMusicGenerator(void (*generator)(audio_buffer_t *buffer));
bool I_PicoSoundIsInitialized(void);
void I_PicoSoundFade(bool in);
bool I_PicoSoundFading(void);
#endif
