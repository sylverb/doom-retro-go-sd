#pragma once
#include "pico.h"
// RP2040 SIO interpolator. pd_render compiles with USE_INTERP=0 on this
// platform (the complete C fallback path); these types only keep stray
// references compiling.
typedef struct {
    volatile uint32_t accum[2];
    volatile uint32_t base[3];
    volatile uint32_t ctrl[2];
    volatile uint32_t peek[3];
    volatile uint32_t add_raw[2];
} interp_hw_t;
