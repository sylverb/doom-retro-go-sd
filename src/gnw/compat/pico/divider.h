#pragma once
#include "pico.h"

// M7 has single-cycle-ish hardware divide; the RP2040 SIO divider helpers
// become plain C division.
static inline uint32_t hw_divider_u32_quotient_inlined(uint32_t a, uint32_t b) {
    return a / b;
}
static inline int32_t hw_divider_s32_quotient_inlined(int32_t a, int32_t b) {
    return a / b;
}
static inline uint32_t hw_divider_u32_remainder_inlined(uint32_t a, uint32_t b) {
    return a % b;
}
static inline int32_t hw_divider_s32_remainder_inlined(int32_t a, int32_t b) {
    return a % b;
}
