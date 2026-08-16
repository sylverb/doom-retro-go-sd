#pragma once
#include "pico.h"
#include "rg_data.h"   // systick_cnt -> firmware counter via the ABI (volatile ptr)

// Engine uses time_us_32 only for perf instrumentation; ms*1000 granularity
// is fine for that.
static inline uint32_t time_us_32(void) {
    return (uint32_t)systick_cnt * 1000u;
}
