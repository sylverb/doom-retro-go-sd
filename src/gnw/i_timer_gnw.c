//
// GNW timer: replaces src/pico/i_timer.c.
//
// Pacing model (verified in d_loop.c): TryRunTics blocks on I_GetTimeMS via
// GetAdjustedTime, with an I_Sleep(1) idle loop — so a correct millisecond
// clock here IS the 35 fps pacer, and I_Sleep is the idle window that keeps
// the SAI buffer fed.
//

#include "i_timer.h"
#include "doomtype.h"
#include "rg_data.h"   // systick_cnt -> firmware counter via the ABI (volatile ptr)

void I_UpdateSound(void);

//
// returns time in 1/35th second tics. Upstream pico computed TICRATE*ms
// (a bug: 35000 tics/sec — harmless there because only GetAdjustedTime's
// ms path is used, wrong for anything else). 2^32*35/1000 = 150323855.
//
int I_GetTime(void)
{
    return (int)((150323855ull * (uint32_t)systick_cnt) >> 32);
}

int I_GetTimeMS(void)
{
    return (int)systick_cnt;
}

void perf_note_idle(int ms);

void I_Sleep(int ms)
{
    uint32_t start = (uint32_t)systick_cnt;
    while ((uint32_t)systick_cnt - start < (uint32_t)ms) {
        I_UpdateSound();   // idle window: keep the SAI DMA buffer topped up
    }
    perf_note_idle(ms);    // CPU% proxy for the perf overlay
}

void I_WaitVBL(int count)
{
    I_Sleep((count * 1000) / 70);
}

void I_InitTimer(void)
{
}
