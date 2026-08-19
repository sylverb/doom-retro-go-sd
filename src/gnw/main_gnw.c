//
// GNW payload stage 2: zero NOLOAD regions, map WHD, D_DoomMain.
//
// Stage-1 snapshotted ACTIVE_FILE into a durable .core_romfile and unpacked
// ITCM + AXI only — AHB (firmware malloc heap) is left alone so
// odroid_overlay_cache_file_in_flash / FatFs still work.
//
#include <stdio.h>
#include <stdint.h>

#include "rg_abi.h"
#include "gw_malloc.h"
#include "pico/i_picosound.h"
#include "odroid_overlay.h"

/* Dedicated app id for saves/settings (not APPID_HOMEBREW). */
#define GNW_APPID_DOOM 30

extern unsigned long _doom_bss_vma_start, _doom_bss_vma_end,
    _dtcm_bss_start, _dtcm_bss_end;

extern const uint8_t *whd_map_base;

extern void D_DoomMain(void);
extern void I_Init(void);

static void doom_sram_init(void)
{
    unsigned long *dst = &_doom_bss_vma_start;
    while (dst < &_doom_bss_vma_end)
        *dst++ = 0;
}

/* .dtcm_bss is linked at DTCM ORIGIN (NOLOAD). Firmware already dtc_init()s
 * before jumping here; re-init + dtc_malloc the span so later firmware
 * dtc_malloc (pause overlay, etc.) does not overlap, then zero. */
static int doom_dtcm_reserve(void)
{
    size_t n = (size_t)((uintptr_t)&_dtcm_bss_end - (uintptr_t)&_dtcm_bss_start);
    void *want = (void *)&_dtcm_bss_start;
    void *got;

    dtc_init();
    got = dtc_malloc(n);
    if (got != want) {
        printf("gnw-doom: DTCM reserve failed (got %p want %p, %lu bytes)\r\n",
               got, want, (unsigned long)n);
        return -1;
    }

    {
        unsigned long *dst = &_dtcm_bss_start;
        while (dst < &_dtcm_bss_end)
            *dst++ = 0;
    }
    return 0;
}

void doom_start(uint8_t load_state, uint8_t start_paused, int8_t save_slot,
                const char *whd_path)
{
    doom_sram_init();

    if (!gnw_abi_ok()) {
        printf("gnw-doom: incompatible firmware ABI (need v2)\r\n");
        return;
    }

    if (doom_dtcm_reserve() != 0)
        return;

    printf("gnw-doom: core multi-WHD\r\n");

    if (!whd_path || !whd_path[0]) {
        printf("gnw-doom: no ACTIVE_FILE WHD path\r\n");
        return;
    }

    uint32_t whd_size = 0;
    whd_map_base = odroid_overlay_cache_file_in_flash(whd_path, &whd_size, 0);
    if (!whd_map_base || whd_size < 8) {
        printf("gnw-doom: missing %s\r\n", whd_path);
        return;
    }
    printf("gnw-doom: WHD %s (%lu bytes)\r\n", whd_path, (unsigned long)whd_size);

    gnw_abi()->odroid_system_init(GNW_APPID_DOOM, PICO_SOUND_SAMPLE_FREQ);

    (void)start_paused;
    extern void doom_persist_boot_args(uint8_t load_state, int8_t save_slot);
    doom_persist_boot_args(load_state, save_slot);

    I_Init();
    D_DoomMain();
}
