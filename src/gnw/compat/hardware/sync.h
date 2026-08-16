#pragma once
#include "pico.h"

// Real interrupt masking on the M7 (PRIMASK), real where it matters.
static inline uint32_t save_and_disable_interrupts(void) {
    uint32_t primask;
    __asm__ volatile ("mrs %0, primask" : "=r" (primask));
    __asm__ volatile ("cpsid i" ::: "memory");
    return primask;
}
static inline void restore_interrupts(uint32_t primask) {
    __asm__ volatile ("msr primask, %0" :: "r" (primask) : "memory");
}

// Single core: spin locks degenerate to nothing.
typedef volatile uint32_t spin_lock_t;
static inline spin_lock_t *spin_lock_instance(uint n) { (void)n; static spin_lock_t dummy; return &dummy; }
static inline uint32_t spin_lock_blocking(spin_lock_t *l) { (void)l; return save_and_disable_interrupts(); }
static inline void spin_unlock(spin_lock_t *l, uint32_t saved) { (void)l; restore_interrupts(saved); }
static inline void spin_lock_unsafe_blocking(spin_lock_t *l) { (void)l; }
static inline void spin_unlock_unsafe(spin_lock_t *l) { (void)l; }
#define PICO_SPINLOCK_ID_OS2 15
