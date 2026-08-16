#pragma once
#include "pico.h"

// RP2040 GPIO calls in engine code are debug-pin leftovers; no-ops here.
// Real Game & Watch button GPIO lives in stm32/i_input_gnw.c.
static inline void gpio_init(uint pin) { (void)pin; }
static inline void gpio_put(uint pin, bool value) { (void)pin; (void)value; }
static inline bool gpio_get(uint pin) { (void)pin; return false; }
static inline void gpio_set_dir(uint pin, bool out) { (void)pin; (void)out; }
#define GPIO_OUT true
#define GPIO_IN false
