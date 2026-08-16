//
// GNW sound module: rp2040-doom's ADPCM SFX engine (from pico/i_picosound.c)
// mixed into the firmware's SAI1 circular DMA buffer (the hardware-validated
// scheme from doomgeneric/i_sound_stm32.c).
//
// Firmware seam (resolved via -Wl,--just-symbols=firmware.out):
//   audio_start/audio_stop/audio_pos/audio_clean_range  (src/audio_sai.c)
//
// HARD CONSTRAINT: the final mix is clamped to +/-12000. The speaker amp
// browns out the board under sustained higher output (rail sags, LCD whites
// out, reset). Raise loudness via the balance gains, never this ceiling.
//

#include "config.h"

#include <string.h>
#include <assert.h>
#include <doom/sounds.h>
#include <z_zone.h>

#include "deh_str.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"

#include "doomtype.h"
#include "i_picosound.h"
#include "trace_gnw.h"

#define ADPCM_BLOCK_SIZE 128
#define ADPCM_SAMPLES_PER_BLOCK_SIZE 249
#define MIX_MAX_VOLUME 128
typedef struct channel_s channel_t;

// retro-go ping-pong audio: the firmware owns the DMA buffer (two halves); we
// refill the half it isn't playing, once per flip. AUDIO_LEN is ONE half.
//
// HARD LIMIT: the firmware's audiobuffer_dma is a FIXED int16[AUDIO_BUFFER_LENGTH*2]
// = int16[2154] array (gw_audio.h, AUDIO_BUFFER_LENGTH=1077). audio_start_playing(L)
// makes the firmware ping-pong over 2*L samples and hand back halves at
// &audiobuffer_dma[L]; we then write L samples there. So 2*L must be <= 2154, i.e.
// L <= 1077 -- otherwise our refill overruns the array into the firmware state
// that lives right after it (the savestate slot table _emu_states_buf sits at
// audiobuffer_dma+0x10d4), which silently zeroed savestate slot selection so every
// save landed on slot 0. 1024 (power of two, under 1077) also halves the per-refill
// mix cost vs the old 2048, smoothing the audio CPU spike. ~21 ms/half @48 kHz.
#define AUDIO_LEN   1024
#define MIX_SAMPLES (AUDIO_LEN / 2)
// Mixer scratch in zero-wait DTCM (CPU-only; the DMA never reads these — only the
// firmware's pool-slack ping-pong buffer, which out[] writes to). Hot per-sample.
static int16_t musicbuf[MIX_SAMPLES] __attribute__((section(".dtcm_bss")));  // music gen output
static int32_t accbuf[MIX_SAMPLES]   __attribute__((section(".dtcm_bss")));  // SFX accumulator
static int16_t *last_active;           // last half filled (detects the DMA flip)

// Diagnostics readable over SWD (same names the doomgeneric build used).
uint32_t snd_max_gap, snd_near_underruns;

extern void audio_start_playing(uint16_t length);
extern void audio_clear_active_buffer(void);
extern void audio_clear_inactive_buffer(void);
extern int16_t *audio_get_active_buffer(void);
extern uint8_t common_emu_sound_get_volume(void);

static volatile enum {
    FS_NONE,
    FS_FADE_OUT,
    FS_FADE_IN,
    FS_SILENT,
} fade_state;
#define FADE_STEP 8 // must be power of 2
uint16_t fade_level;

struct channel_s
{
    const uint8_t *data;
    const uint8_t *data_end;
    uint32_t offset;
    uint32_t step;
    uint8_t left, right; // 0-255
    uint32_t decompressed_size;
#if SOUND_LOW_PASS
    uint8_t alpha256;
#endif
    const int8_t *decompressed;
};

// ====== FROM ADPCM-LIB =====
#define CLIP(data, min, max) \
if ((data) > (max)) data = max; \
else if ((data) < (min)) data = min;

/* step table */
static const uint16_t step_table[89] = {
        7, 8, 9, 10, 11, 12, 13, 14,
        16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66,
        73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658,
        724, 796, 876, 963, 1060, 1166, 1282, 1411,
        1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
        3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
        7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767
};

/* step index tables */
static const int index_table[] = {
        /* adpcm data size is 4 */
        -1, -1, -1, -1, 2, 4, 6, 8
};
// =============================

static void (*music_generator)(audio_buffer_t *buffer);

static boolean sound_initialized = false;
static channel_t channels[NUM_SOUND_CHANNELS];

static boolean use_sfx_prefix;

static inline bool is_channel_playing(int channel) {
    return channels[channel].decompressed_size != 0;
}

static inline void stop_channel(int channel) {
    channels[channel].decompressed_size = 0;
}

static bool check_and_init_channel(int channel) {
    return sound_initialized && ((uint)channel) < NUM_SOUND_CHANNELS;
}

int adpcm_decode_block_s8(int8_t *outbuf, const uint8_t *inbuf, int inbufsize)
{
    int samples = 1, chunks;

    if (inbufsize < 4)
        return 0;

    int32_t pcmdata = (int16_t) (inbuf [0] | (inbuf [1] << 8));
    *outbuf++ = pcmdata>>8u;
    int index = inbuf[2];

    if (index < 0 || index > 88 || inbuf [3])     // sanitize the input a little...
        return 0;

    inbufsize -= 4;
    inbuf += 4;

    chunks = inbufsize / 4;
    samples += chunks * 8;

    while (chunks--) {
        for (int i = 0; i < 4; ++i) {
            int step = step_table[index], delta = step >> 3;

            if (*inbuf & 1) delta += (step >> 2);
            if (*inbuf & 2) delta += (step >> 1);
            if (*inbuf & 4) delta += step;
            if (*inbuf & 8) delta = -delta;

            pcmdata += delta;
            index += index_table [*inbuf & 0x7];
            CLIP(index, 0, 88);
            CLIP(pcmdata, -32768, 32767);
            outbuf [i * 2] = pcmdata>>8u;

            step = step_table[index], delta = step >> 3;

            if (*inbuf & 0x10) delta += (step >> 2);
            if (*inbuf & 0x20) delta += (step >> 1);
            if (*inbuf & 0x40) delta += step;
            if (*inbuf & 0x80) delta = -delta;

            pcmdata += delta;
            index += index_table[(*inbuf >> 4) & 0x7];
            CLIP(index, 0, 88);
            CLIP(pcmdata, -32768, 32767);
            outbuf [i * 2 + 1] = pcmdata>>8u;
            inbuf++;
        }

        outbuf += 8;
    }

    return samples;
}

static void decompress_buffer(channel_t *channel) {
    if (channel->data == channel->data_end) {
        channel->decompressed_size = 0;
    } else {
        int block_size = MIN(ADPCM_BLOCK_SIZE, channel->data_end - channel->data);
        channel->decompressed_size = adpcm_decode_block_s8(channel->decompressed, channel->data, block_size);
        assert(channel->decompressed_size && channel->decompressed_size <= sizeof(channel->decompressed));
        channel->data += block_size;
    }
}

static boolean init_channel_for_sfx(channel_t *ch, const sfxinfo_t *sfxinfo, int pitch)
{
    int lumpnum = sfx_mut(sfxinfo)->lumpnum;
    int lumplen = W_LumpLength(lumpnum);

    const uint8_t *data = W_CacheLumpNum(lumpnum, PU_STATIC); // we don't track because we assume in ROWAD anyway

    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x81) // note 0x81 is our uncompressed signed 8-bit format
    {
        return false;
    }

    int length = lumplen - 8;

    ch->data = data + 8;
    ch->data_end = ch->data + length;

    uint32_t sample_freq = (data[3] << 8) | data[2];
    if (pitch == NORM_PITCH)
        ch->step = sample_freq * 65536 / PICO_SOUND_SAMPLE_FREQ;
    else
        ch->step = (uint32_t)((sample_freq * pitch) * 65536ull / (PICO_SOUND_SAMPLE_FREQ * pitch));

    ch->decompressed = (const int8_t *)ch->data;
    ch->decompressed_size = length;
    ch->offset = 0;

#if SOUND_LOW_PASS
    ch->alpha256 = 256u * 201u * sample_freq / (201u * sample_freq + 64u * (uint)PICO_SOUND_SAMPLE_FREQ);
#endif
    return true;
}

static void GetSfxLumpName(const sfxinfo_t *sfx, char *buf, size_t buf_len)
{
    // Linked sfx lumps? Get the lump number for the sound linked to.
    if (sfx->link != NULL)
    {
        sfx = sfx->link;
    }

    if (use_sfx_prefix)
    {
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    }
    else
    {
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
    }
}

static void I_Pico_PrecacheSounds(should_be_const sfxinfo_t *sounds, int num_sounds)
{
    // no-op: sfx decode straight out of memory-mapped flash
}

static int I_Pico_GetSfxLumpNum(should_be_const sfxinfo_t *sfx)
{
    char namebuf[9];
    GetSfxLumpName(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

static void I_Pico_UpdateSoundParams(int handle, int vol, int sep)
{
    int left, right;

    if (!sound_initialized || handle < 0 || handle >= NUM_SOUND_CHANNELS)
    {
        return;
    }

    left = ((254 - sep) * vol) / 127;
    right = ((sep) * vol) / 127;

    if (left < 0) left = 0;
    else if ( left > 255) left = 255;
    if (right < 0) right = 0;
    else if (right > 255) right = 255;

    channels[handle].left = left;
    channels[handle].right = right;
}

static int I_Pico_StartSound(should_be_const sfxinfo_t *sfxinfo, int channel, int vol, int sep, int pitch)
{
    if (!check_and_init_channel(channel)) return -1;

    stop_channel(channel);
    channel_t *ch = &channels[channel];
    if (!init_channel_for_sfx(ch, sfxinfo, pitch)) {
        assert(!is_channel_playing(channel)); // don't expect to have to mark it sotpped
    }
    I_Pico_UpdateSoundParams(channel, vol, sep);
    return channel;
}

static void I_Pico_StopSound(int channel)
{
    if (check_and_init_channel(channel)) {

    }
}

// Re-sync after a blocking firmware menu: silence the ring and forget the
// last refilled half so mixing resumes immediately on the next poll.
void I_SoundResync(void)
{
    if (!sound_initialized) return;
    audio_clear_active_buffer();
    audio_clear_inactive_buffer();
    last_active = 0;
}

static boolean I_Pico_SoundIsPlaying(int channel)
{
    if (!check_and_init_channel(channel)) return false;
    return is_channel_playing(channel);
}

// Generate the samples the DMA consumed since last call: music block once
// (advances the MIDI timeline), SFX channels accumulated, then the balance
// and the non-negotiable +/-12000 clamp.
static void I_Pico_UpdateSound(void)
{
    if (!sound_initialized) return;

    // retro-go ping-pong: refill the half the DMA is NOT playing, once per flip.
    // Polling I_UpdateSound re-checks the active half; until the DMA crosses into
    // it we've already filled it, so bail until the flip.
    int16_t *out = audio_get_active_buffer();
    if (out == last_active)
        return;
    last_active = out;

    const uint32_t count = MIX_SAMPLES;

    // Sampled once per buffer fill, not per sample: the level only changes from
    // the overlay (which is blocking), so a mid-buffer change is impossible.
    const uint8_t g_out_volume = common_emu_sound_get_volume();

    TRACE_EVT(TEV_MIX_BEG, count);

    audio_buffer_t buffer = { musicbuf, count, 0 };
    bool have_music = false;
    if (music_generator) {
        music_generator(&buffer);
        have_music = buffer.sample_count != 0;
    }

    memset(accbuf, 0, count * sizeof(accbuf[0]));
    for (int chn = 0; chn < NUM_SOUND_CHANNELS; chn++) {
        if (!is_channel_playing(chn))
            continue;
        channel_t *channel = &channels[chn];
        assert(channel->decompressed_size);
        // Mono fold: left+right ~= 2*vol, so /2 lands at doomgeneric's
        // per-channel gain. (First cut used /4 — "extremely quiet" sfx.)
        int vol = (channel->left + channel->right) / 2;
        uint offset_end = channel->decompressed_size * 65536;
        assert(channel->offset < offset_end);
#if SOUND_LOW_PASS
        int alpha256 = channel->alpha256;
        int beta256 = 256 - alpha256;
        int sample = channel->decompressed[channel->offset >> 16];
#endif
        for (uint32_t s = 0; s < count; s++) {
#if !SOUND_LOW_PASS
            int sample = channel->decompressed[channel->offset >> 16];
#else
            sample = (beta256 * sample + alpha256 * channel->decompressed[channel->offset >> 16]) / 256;
#endif
            accbuf[s] += sample * vol;
            channel->offset += channel->step;
            if (channel->offset >= offset_end) {
                stop_channel(chn);
                break;
            }
        }
    }

    for (uint32_t k = 0; k < count; k++) {
        // SFX: s8(+/-127) * vol(<=127) -> ~+/-16k per channel; >>2 puts one
        // full-volume sound at ~4k. Music *2 lands OPL peaks in the same band.
        int32_t acc = accbuf[k] >> 2;
        if (have_music)
            acc += musicbuf[k] * 2;
        if (fade_state == FS_SILENT) {
            acc = 0;
        } else if (fade_state != FS_NONE) {
            acc = (acc * (int)fade_level) >> 16;
            int fade_step = fade_state == FS_FADE_IN ? FADE_STEP : -FADE_STEP;
            fade_level += fade_step;
            if (!fade_level)
                fade_state = (fade_state == FS_FADE_OUT) ? FS_SILENT : FS_NONE;
        }
        // Global output volume (retro-go's overlay slider). The firmware never
        // touches the samples; it just publishes a 0..255 scale factor and the
        // app applies it — so this multiply is what makes the slider audible.
        // Applied BEFORE the clamp so attenuation can never push us past it.
        acc = (acc * (int32_t)g_out_volume) >> 8;

        // Brown-out guard (see header comment): never raise.
        if (acc > 12000)       acc = 12000;
        else if (acc < -12000) acc = -12000;
        int16_t out_val = (int16_t) acc;
        out[k * 2] = out_val;
        out[k * 2 + 1] = out_val;
    }

    TRACE_EVT(TEV_MIX_END, count);
}

static void I_Pico_ShutdownSound(void)
{
    if (!sound_initialized)
    {
        return;
    }
    // No audio_stop in retro-go's ABI; the firmware silences the DMA on standby.
    sound_initialized = false;
}

static boolean I_Pico_InitSound(boolean _use_sfx_prefix)
{
    use_sfx_prefix = _use_sfx_prefix;

    last_active = NULL;
    audio_start_playing(AUDIO_LEN);     // firmware ping-pong DMA over 2x AUDIO_LEN
    audio_clear_active_buffer();        // silence whatever the launcher left in
    audio_clear_inactive_buffer();      // the ring (the boot "crunch" otherwise)

    sound_initialized = true;
    return true;
}

static snddevice_t sound_pico_devices[] =
{
    SNDDEVICE_SB,
};

sound_module_t sound_pico_module =
{
    sound_pico_devices,
    arrlen(sound_pico_devices),
    I_Pico_InitSound,
    I_Pico_ShutdownSound,
    I_Pico_GetSfxLumpNum,
    I_Pico_UpdateSound,
    I_Pico_UpdateSoundParams,
    I_Pico_StartSound,
    I_Pico_StopSound,
    I_Pico_SoundIsPlaying,
    I_Pico_PrecacheSounds,
};

bool I_PicoSoundIsInitialized(void) {
    return sound_initialized;
}

void I_PicoSoundSetMusicGenerator(void (*generator)(audio_buffer_t *buffer)) {
    music_generator = generator;
}

void I_PicoSoundFade(bool in) {
    fade_state = in ? FS_FADE_IN : FS_FADE_OUT;
    fade_level = in ? FADE_STEP : 0x10000 - FADE_STEP;
}

bool I_PicoSoundFading(void) {
    return fade_state == FS_FADE_IN || fade_state == FS_FADE_OUT;
}
