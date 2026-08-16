//
// rg_data.h — route firmware DATA symbols through the ABI table.
//
// Functions resolve via abi_stubs.c, but data (the firmware's free-running counter
// and its LUT8 framebuffer) can't be wrapped — so expose them as macros that read
// the pointers the firmware published in g_firmware_abi. Include this (and drop the
// old `extern ... systick_cnt;` decl) wherever the firmware counter is read.
//
#ifndef RG_DATA_H
#define RG_DATA_H

#include <stdint.h>

#include "rg_abi.h"

// Firmware's 1 kHz millisecond clock (retro-go's get_elapsed_time), plus the
// savestate time bias: a restored snapshot's timing globals (TryRunTics
// lasttime, OPL pacing, perf windows) reference the clock epoch of the session
// that SAVED it — after a reboot the raw clock restarts and the game would
// stall waiting to "catch up". retrogo_persist.c rebases g_doom_time_bias on
// every state load so this clock continues from the snapshot's timestamp.
// Used as a counter value throughout (never address-of), so the expression is
// transparent.
extern uint32_t g_doom_time_bias;
#define systick_cnt (gnw_abi()->HAL_GetTick() + g_doom_time_bias)

#endif // RG_DATA_H
