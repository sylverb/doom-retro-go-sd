#pragma once
#include "pico/time.h"

static inline void busy_wait_us(uint64_t us) {
    // ms-granularity busy wait off SysTick; engine only uses this in
    // non-hot paths (flash save pacing).
    uint32_t start = (uint32_t)systick_cnt;
    uint32_t ms = (uint32_t)(us / 1000u) + 1u;
    while ((uint32_t)systick_cnt - start < ms) {}
}
static inline void busy_wait_ms(uint32_t ms) {
    busy_wait_us((uint64_t)ms * 1000u);
}
