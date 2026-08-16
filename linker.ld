/* gnw-doom RAM-overlay linker script (CORE packaging).
 *
 * The firmware copies the payload to __RAM_EMU_START__ (0x2404B000) and jumps
 * to offset 0 (app_main_doom). Stage 1 (src/gnw/gwhb_entry.c) runs in place
 * and copies ITCM + AXI segments from image LMA to VMA, then enters doom_start.
 * The WHD is a separate /roms/doom/*.whd selected via ACTIVE_FILE.
 *
 * Firmware memory model (SD Retro-Go, post-AHB-heap move):
 *   DTCM  — stack (~24 KiB + redzone at top) + ~104 KiB dtcm_malloc bump.
 *           Doom's former "DTCM scratch" (.dtcm_bss) lives here as NOLOAD;
 *           doom_start reserves the span via dtcm_malloc so the bump stays coherent.
 *   AHB   — firmware .persistent / .data / .bss / newlib malloc heap / .audio.
 *           Cores must NOT link fixed segments here (ahb_malloc → malloc only).
 *   ITCM  — app hot code (pack_core ITCM segment / stage-1 copy).
 *   AXI   — framebuffers, pcache, RAM_EMU image + .bss/zone, cold .text_axis.
 *
 * AXISRAM map (1024K @ 0x24000000):
 *   0x24000000  160K  two 75K LUT8 framebuffers (MPU-uncached)
 *   0x24028000  140K  .pcache       patch_cache_buf (PATCH_CACHE_BYTES=0x23000)
 *   0x2404B000        .gwhb         image load address (stage-1)
 *   0x2404B000..7FF   .core_entry   stage-1 + durable ACTIVE_FILE snapshot
 *   0x2404B800        .data + .bss  (LMA overlaps BY DESIGN; zero .bss after copy)
 *   (bss end)         zone heap     _zone_start.._zone_end
 *   TEXT_AXIS_ORIGIN  .text_axis    warm+cold code + all rodata
 */

ENTRY(app_main_doom)

/* pack_core.py / run_dynamic_core symbols (must match firmware gnw_ram_emu.ld). */
__RAM_EMU_START__ = 0x2404B000;

TEXT_AXIS_ORIGIN   = 0x240C1800;   /* was 0xC2000; -2K for gcc 15 .text_axis growth */
ZONE_MIN           = 182K;         /* was 184K; follows TEXT_AXIS_ORIGIN -2K */

/* Match firmware SDCARD.ld: 128K DTCM − 24K stack − 256B redzone ≈ 103.75K. */
DTCM_CORE_LENGTH   = 103K;

MEMORY {
    /* +0x100: tolerate the firmware's optional null-pointer-guard MPU region
     * (__NULLPTR_LENGTH__ up to 256B) at the bottom of ITCM. */
    ITCMRAM (rwx) : ORIGIN = 0x00000100, LENGTH = 64K - 0x100
    /* Real DTCM bump window (firmware dtcm_malloc). NOLOAD only — never packed. */
    DTCMAPP (rw)  : ORIGIN = 0x20000000, LENGTH = DTCM_CORE_LENGTH
    /* 160K: two 75K LUT8 framebuffers (retro-go double-buffer), MPU-uncached. */
    RAM_UC  (rwx) : ORIGIN = 0x24000000, LENGTH = 160K
    PCACHE  (rw)  : ORIGIN = 0x24028000, LENGTH = 0x23000
    /* Image load region: VMA of .gwhb AND the LMA chain of every copied
     * section. Length caps the image so stage-1 copy destinations
     * (TEXT_AXIS_ORIGIN and up) can never overlap the source image. */
    BLOB    (rwx) : ORIGIN = 0x2404B000, LENGTH = 0x240C1800 - 0x2404B000
    /* .data + .bss; image LMAs overlap (see header). +0x800 leaves room for
     * .core_entry (stage-1 + durable ACTIVE_FILE). */
    AXIBSS  (rw)  : ORIGIN = 0x2404B800, LENGTH = 0x240C1800 - 0x2404B800
    AXITEXT (rwx) : ORIGIN = 0x240C1800, LENGTH = 0x24100000 - 0x240C1800
}

SECTIONS {
    /* Image offset 0: CORE entry (stage-1 unpacker). VMA == LMA (no AT>). */
    .core_entry : {
        KEEP(*gwhb_entry.o(.gwhb_entry))
        *gwhb_entry.o(.text*)
        *gwhb_entry.o(.rodata*)
        KEEP(*(.core_abi_meta))
        KEEP(*(.core_romfile))
        . = ALIGN(4);
    } > BLOB

    /* ============== ITCM: zero-wait instruction RAM (64K) ==============
     * Hot-renderer functions by name (upstream list) + whole hot objects.
     * TUNABLE: object list driven by TRACE profiling. */
    .itcram_hot : {
        . = ALIGN(4);
        __itcram_hot_start__ = .;
        *(.itcram_hot)
        *(.itcram_hot.*)
        *(.text.R_RenderBSPNode)
        *(.text.R_RenderSegLoop)
        *(.text.R_StoreWallRange)
        *(.text.R_ClipSolidWallSegment)
        *(.text.R_ClipPassWallSegment)
        *(.text.R_AddLine)
        *(.text.R_CheckBBox)
        *(.text.R_Subsector)
        *(.text.R_PointOnSide)
        *(.text.R_ScaleFromGlobalAngle)
        *(.text.R_DrawMaskedColumn)
        *(.text.R_ProjectSprite)
        *(.text.R_AddSprites)
        /* whole-object hot set (text only; their rodata goes to .text_axis) */
        *pd_render.o(.text*)
        *p_map.o(.text*)
        *p_enemy.o(.text*)
        *p_sight.o(.text*)
        *p_maputl.o(.text*)
        . = ALIGN(4);
        __itcram_hot_end__ = .;
    } > ITCMRAM AT > BLOB
    __itcram_hot_lma = LOADADDR(.itcram_hot);
    ASSERT(__itcram_hot_end__ <= ORIGIN(ITCMRAM) + LENGTH(ITCMRAM),
           "ITCM overflow: shrink the .itcram_hot object list")

    /* ============== DTCM: render/audio scratch (dtcm_malloc pool) ==============
     * NOLOAD — not in the packed image. doom_start must dtcm_malloc() this
     * span so the firmware bump does not reuse it. */
    .dtcm_bss (NOLOAD) : {
        . = ALIGN(4);
        _dtcm_bss_start = .;
        *(.dtcm_bss)
        . = ALIGN(4);
        _dtcm_bss_end = .;
    } > DTCMAPP
    ASSERT(_dtcm_bss_end <= ORIGIN(DTCMAPP) + LENGTH(DTCMAPP),
           "DTCM .dtcm_bss overflow: shrink DOOMX_DTCM_BSS scratch")

    /* ============== AXISRAM ============== */

    /* patch_cache_buf (pd_render.cpp, DOOMX_PCACHE_SECTION) exactly fills the
     * gap between the framebuffers and the image load address. */
    .pcache (NOLOAD) : {
        _pcache_start = .;
        *(.pcache)
        _pcache_end = .;
    } > PCACHE

    /* Pipeline trace slot pool (TRACE=1 only; zero-size otherwise). */
    .trace_buf (NOLOAD) : {
        . = ALIGN(8);
        *(.trace_buf)
        . = ALIGN(8);
    } > PCACHE
    ASSERT(SIZEOF(.pcache) + SIZEOF(.trace_buf) <= LENGTH(PCACHE),
           "patch cache + trace pool exceed the PCACHE window: lower PATCH_CACHE_BYTES or TRACE_NUM_SLOTS")

    /* Initialized data in RAM_EMU (was wrongly linked into AHB). */
    .data : {
        . = ALIGN(4);
        _doom_data_vma_start = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _doom_data_vma_end = .;
    } > AXIBSS AT > BLOB
    _doom_data_lma = LOADADDR(.data);

    /* Warm + cold code + ALL rodata at top of AXISRAM (stage-1 copy).
     * Former .text_dtcm objects are listed first (AXI-cached, not AHB). */
    .text_axis : {
        . = ALIGN(4);
        __text_axis_start__ = .;
        /* Warm tier (AXI-cached). Keep p_tick/p_user here — ITCM placement
         * hardfaulted in P_RunThinkers (PC in ITCM, LR=0x10000001). */
        *r_main.o(.text*)
        *r_segs.o(.text*)
        *r_things.o(.text*)
        *p_mobj.o(.text*)
        *p_inter.o(.text*)
        *p_pspr.o(.text*)
        *p_tick.o(.text*)
        *p_user.o(.text*)
        *s_sound.o(.text*)
        *r_plane.o(.text*)
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
        __text_axis_end__ = .;
    } > AXITEXT AT > BLOB
    __text_axis_lma = LOADADDR(.text_axis);
    ASSERT(__text_axis_end__ <= 0x24100000, "AXITEXT overflow: lower TEXT_AXIS_ORIGIN")

    /* End of file-backed payload for pack_core.py (bss_size = 0; doom zeroes
     * its overlapping .bss itself after stage-1). */
    .__core_image_end : {
        . = ALIGN(4);
        __CORE_CODE_END__ = .;
    } > BLOB
    __CORE_BSS_END__ = __CORE_CODE_END__;
    __CORE_INIT_ARRAY_START__ = __CORE_CODE_END__;
    __CORE_INIT_ARRAY_END__ = __CORE_CODE_END__;

    .bss (NOLOAD) : {
        . = ALIGN(4);
        _doom_bss_vma_start = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _doom_bss_vma_end = .;
    } > AXIBSS

    /* Single zone region for rp2040-doom's z_zone (I_ZoneBase). */
    _zone_start = _doom_bss_vma_end;
    _zone_end = TEXT_AXIS_ORIGIN;
    ASSERT(_zone_end - _zone_start >= ZONE_MIN,
           "zone heap below ZONE_MIN: shrink PATCH_CACHE_BYTES / trace ring, or raise TEXT_AXIS_ORIGIN")

    /DISCARD/ : {
        *(.ARM.exidx*) *(.ARM.extab*)
    }
}
