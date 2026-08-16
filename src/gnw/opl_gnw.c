//
// GNW OPL driver: replaces opl/opl_pico.c behind rp2040-doom's opl.h API.
//
// emu8950 runs at its native 49716 Hz (EMU8950_NO_RATECONV; clk/72) and is
// resampled to the 48 kHz SAI rate with 16.16 linear interpolation — the
// hardware-validated scheme from doomgeneric/opl_stm32.c (nearest-neighbour
// resampling produced an audible ~1.7 kHz buzz on held notes).
//
// MIDI event timing: callbacks are queued in microseconds and fired from the
// render path (refill_chunk splits render blocks at callback deadlines), the
// same single-threaded timeline as both upstream backends.
//
// Exports `opl_pico_driver` so opl_api.c's driver table links unchanged, and
// registers OPL_GNW_Mix_callback as the i_picosound music generator.
//

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "rg_data.h"   // systick_cnt via the ABI table

#include "i_picosound.h"
#include "trace_gnw.h"

#include "emu8950.h"
#include "opl.h"
#include "opl_internal.h"
#include "opl_queue.h"

#define OPL_CLK   3579552u            // standard AdLib OPL clock
#define OPL_RATE  (OPL_CLK / 72u)     // 49716 Hz, rate converter disabled

static OPL *emu8950_opl;
static uint64_t current_time;   // microseconds since playback start
static uint64_t pause_offset;
static int opl_gnw_paused;
static int register_num;
static bool audio_was_initialized;

// ---------------------------------------------------------------------------
// Callback queue: upstream used pico/util/pheap; 10 entries makes a sorted
// array insert cheaper than any heap.
// ---------------------------------------------------------------------------
#define MAX_OPL_QUEUE 10

typedef struct {
    uint64_t time;
    opl_callback_t callback;
    void *data;
} queue_entry_t;

struct opl_callback_queue_s {
    queue_entry_t entries[MAX_OPL_QUEUE];
    int count;
};

static struct opl_callback_queue_s the_queue;

opl_callback_queue_t *OPL_Queue_Create(void)
{
    the_queue.count = 0;
    return &the_queue;
}

int OPL_Queue_IsEmpty(opl_callback_queue_t *queue)
{
    return queue->count == 0;
}

void OPL_Queue_Clear(opl_callback_queue_t *queue)
{
    queue->count = 0;
}

void OPL_Queue_Destroy(opl_callback_queue_t *queue)
{
    queue->count = 0;
}

void OPL_Queue_Push(opl_callback_queue_t *queue,
                    opl_callback_t callback, void *data,
                    uint64_t time)
{
    if (queue->count == MAX_OPL_QUEUE) {
        // never expected (upstream sized it for at most a few in flight)
        return;
    }
    int i = queue->count++;
    while (i > 0 && queue->entries[i - 1].time > time) {
        queue->entries[i] = queue->entries[i - 1];
        i--;
    }
    queue->entries[i].time = time;
    queue->entries[i].callback = callback;
    queue->entries[i].data = data;
}

int OPL_Queue_Pop(opl_callback_queue_t *queue,
                  opl_callback_t *callback, void **data)
{
    if (!queue->count)
        return 0;
    *callback = queue->entries[0].callback;
    *data = queue->entries[0].data;
    queue->count--;
    memmove(&queue->entries[0], &queue->entries[1],
            queue->count * sizeof(queue_entry_t));
    return 1;
}

uint64_t OPL_Queue_Peek(opl_callback_queue_t *queue)
{
    return queue->count ? queue->entries[0].time : 0;
}

void OPL_Queue_AdjustCallbacks(opl_callback_queue_t *queue, uint64_t time,
                               unsigned int old_tempo, unsigned int new_tempo)
{
    for (int i = 0; i < queue->count; i++) {
        uint64_t offset = queue->entries[i].time - time;
        queue->entries[i].time = time + offset * old_tempo / new_tempo;
    }
}

static opl_callback_queue_t *callback_queue;

// ---------------------------------------------------------------------------
// Timeline + block renderer
// ---------------------------------------------------------------------------

#if DOOM_TINY
extern uint8_t restart_song_state;
extern void RestartSong(void *unused);
#endif

// Fire every due callback, then render only up to the next deadline.
#define MUSIC_CHUNK 256                  // OPL samples per block (~5 ms)
static DOOMX_DTCM_BSS int32_t chunk[MUSIC_CHUNK];
static uint32_t chunk_len, chunk_pos;

static void __not_in_flash_func(refill_chunk)(void)
{
    const uint64_t dt = OPL_SECOND / OPL_RATE;   // microseconds per OPL sample
    uint32_t n = MUSIC_CHUNK;

    opl_callback_t cb;
    void *data;
    while (!OPL_Queue_IsEmpty(callback_queue)
        && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset)
    {
        if (!OPL_Queue_Pop(callback_queue, &cb, &data))
            break;
        cb(data);
    }
    if (!OPL_Queue_IsEmpty(callback_queue))
    {
        uint64_t next = OPL_Queue_Peek(callback_queue) + pause_offset;
        uint64_t until = (next - current_time) / dt + 1;
        if (until < n)
            n = (uint32_t) until;
    }

    if (opl_gnw_paused)
    {
        memset(chunk, 0, n * sizeof(chunk[0]));
        pause_offset += n * dt;
    }
    else
    {
        TRACE_EVT(TEV_OPL_BEG, n);
        OPL_calc_buffer_linear(emu8950_opl, chunk, n);
        TRACE_EVT(TEV_OPL_END, n);
    }
    current_time += n * dt;
    chunk_len = n;
    chunk_pos = 0;
}

// Music generator: fills the (mono, 48 kHz) buffer span. Raw emu8950 level is
// kept (no upstream <<3); the mixer applies the music*2 balance gain.
static void __not_in_flash_func(OPL_GNW_Mix_callback)(audio_buffer_t *audio_buffer)
{
    // 16.16 step from the 48 kHz output clock to the 49716 Hz OPL clock.
    static const uint32_t step = (uint32_t)(((uint64_t) OPL_RATE << 16) / PICO_SOUND_SAMPLE_FREQ);
    static uint32_t frac;
    static int16_t s0, s1;

#if DOOM_TINY
    if (restart_song_state == 2) {
        RestartSong(0);
    }
#endif

    int16_t *out = audio_buffer->samples;
    for (uint32_t i = 0; i < audio_buffer->max_sample_count; i++)
    {
        frac += step;
        while (frac >= (1u << 16))
        {
            frac -= (1u << 16);
            s0 = s1;
            if (chunk_pos >= chunk_len)
                refill_chunk();
            int32_t s = chunk[chunk_pos++];   // sum of up to 9 voices
            if (s > 32767)       s = 32767;
            else if (s < -32768) s = -32768;
            s1 = (int16_t) s;
        }
        out[i] = (int16_t)(s0 + (((s1 - s0) * (int32_t)(frac >> 8)) >> 8));
    }
    audio_buffer->sample_count = audio_buffer->max_sample_count;
}

// ---------------------------------------------------------------------------
// opl_driver_t implementation
// ---------------------------------------------------------------------------

static void OPL_Pico_Shutdown(void)
{
    if (audio_was_initialized)
    {
        I_PicoSoundSetMusicGenerator(NULL);
        OPL_Queue_Destroy(callback_queue);
        audio_was_initialized = 0;
    }
}

static int OPL_Pico_Init(unsigned int port_base)
{
    if (I_PicoSoundIsInitialized()) {
        opl_gnw_paused = 0;
        pause_offset = 0;

        callback_queue = OPL_Queue_Create();
        current_time = 0;
        chunk_len = chunk_pos = 0;

        emu8950_opl = OPL_new(OPL_CLK, OPL_RATE);

        I_PicoSoundSetMusicGenerator(OPL_GNW_Mix_callback);
        audio_was_initialized = 1;
    } else {
        audio_was_initialized = 0;
    }
    return 1;
}

static unsigned int OPL_Pico_PortRead(opl_port_t port)
{
    // Matches opl_pico.c: with EMU8950_NO_TIMER there are no timer bits.
    // If a chocolate-style OPL_Detect handshake ever runs against this and
    // misdetects OPL3, the known fix is idle value 0x06 (doomgeneric
    // opl_stm32.c:64 — phantom second-bank voices corrupted the real ones).
    if (port == OPL_REGISTER_PORT_OPL3)
    {
        return 0xff;
    }
    return 0;
}

static void WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_NEW:
        default:
            OPL_writeReg(emu8950_opl, reg_num, value);
            break;
    }
}

static void OPL_Pico_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)
    {
        register_num = value;
    }
    else if (port == OPL_REGISTER_PORT_OPL3)
    {
        register_num = value | 0x100;
    }
    else if (port == OPL_DATA_PORT)
    {
        WriteRegister(register_num, value);
    }
}

static void OPL_Pico_SetCallback(uint64_t us, opl_callback_t callback,
                                void *data)
{
    OPL_Queue_Push(callback_queue, callback, data,
                   current_time - pause_offset + us);
}

static void OPL_Pico_ClearCallbacks(void)
{
    OPL_Queue_Clear(callback_queue);
}

static void OPL_Pico_Lock(void)
{
}

static void OPL_Pico_Unlock(void)
{
}

static void OPL_Pico_SetPaused(int paused)
{
    opl_gnw_paused = paused;
}

static void OPL_Pico_AdjustCallbacks(unsigned int old_tempo, unsigned int new_tempo)
{
    OPL_Queue_AdjustCallbacks(callback_queue, current_time, old_tempo, new_tempo);
}

const opl_driver_t opl_pico_driver =
{
    "GNW",
    OPL_Pico_Init,
    OPL_Pico_Shutdown,
    OPL_Pico_PortRead,
    OPL_Pico_PortWrite,
    OPL_Pico_SetCallback,
    OPL_Pico_ClearCallbacks,
    OPL_Pico_Lock,
    OPL_Pico_Unlock,
    OPL_Pico_SetPaused,
    OPL_Pico_AdjustCallbacks,
};

void OPL_Delay(uint64_t us)
{
    unsigned long start = systick_cnt;
    unsigned long ms = (unsigned long)(us / 1000u) + 1u;
    extern void I_UpdateSound(void);
    while (systick_cnt - start < ms) {
        I_UpdateSound();   // keep the SAI fed; OPL_Delay only runs at init
    }
}
