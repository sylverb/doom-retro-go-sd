// pico-SDK shim for the Game & Watch (STM32H7B0, single Cortex-M7).
// Engine sources include "pico.h" unconditionally under PICO_BUILD; this
// provides just the surface rp2040-doom actually uses, mapped to no-ops or
// M7 equivalents.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef unsigned int uint;

// "Not in flash" on the GNW means ITCM (64K, zero-wait): upstream already
// annotated its decode hot path (tiny_huff, image_decoder, emu8950) with
// these; main_gnw.c copies .itcram_hot from flash at boot.
#define __not_in_flash_func(x) __attribute__((section(".itcram_hot." #x))) x
#define __no_inline_not_in_flash_func(x) __attribute__((noinline, section(".itcram_hot." #x))) x

// Hot zero-init data -> DTCM (128K, zero-wait, CPU-only — never DMA buffers).
#define DOOMX_DTCM_BSS __attribute__((section(".dtcm_bss")))
#define __scratch_x(group)
#define __scratch_y(group)
#define __noinline __attribute__((noinline))
#define __force_inline inline __attribute__((always_inline))
#ifndef __aligned
#define __aligned(n) __attribute__((aligned(n)))
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef count_of
#define count_of(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define tight_loop_contents() do {} while (0)
#define __breakpoint() __asm__ volatile ("bkpt #0")

// Single core: there is no core 1. Constant 0 also steers pd_render's
// get_patch_decoder() onto the caching decoder path, which is what we want.
#define get_core_num() 0u

// RP2040 single-cycle multiply hints -> plain multiply (M7 has it natively).
#define __mul_instruction(a, b) ((a) * (b))
#define __fast_mul(a, b) ((a) * (b))

// OSPI memory-mapped window (p_saveg computes flash offsets against this).
#ifndef XIP_BASE
#define XIP_BASE 0x90000000u
#endif

#define panic(...) do { printf("PANIC: " __VA_ARGS__); __breakpoint(); for (;;); } while (0)
#define panic_unsupported() panic("not supported")
#define hard_assert assert

// kilograham's CU debug-pin instrumentation -> no-ops.
#define CU_REGISTER_DEBUG_PINS(...)
#define CU_SELECT_DEBUG_PINS(x)
#define DEBUG_PINS_SET(x, v) ((void)0)
#define DEBUG_PINS_CLR(x, v) ((void)0)
#define DEBUG_PINS_XOR(x, v) ((void)0)

// On the pico-sdk, spinlock/irq helpers arrive via pico.h's include chain.
#include "hardware/sync.h"
