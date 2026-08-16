//
// GNW video: replaces src/pico/i_video.c.
//
// The RP2040 streamed RGB565 scanlines to VGA from core1; here the LTDC
// scans a 320x240 LUT8 framebuffer (firmware symbol _frame_buffer, uncached
// RAM_UC at 0x24000000) autonomously, so the whole display path collapses to
// one synchronous compose per rendered frame (I_DisplayFrameReady, called by
// pd_end_frame):
//
//   - latch display_* from next_*
//   - palette change -> 256-entry RGB888 CLUT via ltdc_set_clut()
//     (keeping upstream's WHX tint synthesis: WHX ships only PLAYPAL[0])
//   - per source scanline: video-type base layer (double/single/wipe),
//     vpatch overlay pass — all in 8-bit palette indices, the per-pixel
//     palette[] lookups of the RGB565 path simply drop out —
//   - 200->240 row replication into the LTDC framebuffer (Doom's 320x200 is
//     authored for 4:3; the stretch restores aspect on the 320x240 panel).
//
// Uncached framebuffer: no cache maintenance needed (same as doomgeneric).
//

#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <doom/r_data.h>
#include "doom/f_wipe.h"
#include "pico.h"

#include "config.h"
#include "d_loop.h"
#include "deh_str.h"
#include "doomtype.h"
#include "i_input.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_misc.h"
#include "tables.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"
#include "picodoom.h"
#include "trace_gnw.h"
#include "rg_data.h"   // systick_cnt + gnw_frame_buffer via the ABI table
#include "gw_lcd.h"

/* retro-go's lcd_set_clut caches only 32 entries (sized for pico8) + dark
 * twins — doom needs all 256, so push the full table straight into the LTDC
 * layer-1 CLUT (CLUTWR: index<<24 | RGB888) after letting the firmware cache
 * its 32 (keeps the overlay menu's dimming machinery coherent). Re-pushed
 * after the overlay menu returns (I_ReloadClut). */
static void ltdc_push_clut256(const uint32_t *rgb888)
{
    volatile uint32_t *L1CLUTWR = (volatile uint32_t *)0x500010C4;
    volatile uint32_t *L1CR     = (volatile uint32_t *)0x50001084;
    volatile uint32_t *SRCR     = (volatile uint32_t *)0x50001024;
    for (uint32_t i = 0; i < 256; i++)
        *L1CLUTWR = (i << 24) | (rgb888[i] & 0x00FFFFFFu);
    *L1CR |= (1u << 4);          /* CLUTEN */
    *SRCR  = (1u << 1);          /* reload at vertical blanking */
}
void I_UpdateSound(void);
void I_GetEvent(void);

#define LCD_WIDTH  320
#define LCD_HEIGHT 240

static const patch_t *stbar;

volatile uint8_t interp_in_use;   // referenced by pd_render's SafeUpdateSound (dead on single core)

static boolean initialized = false;

boolean screenvisible = true;
boolean screensaver_mode = false;
isb_int8_t usegamma = 0;
unsigned int joywait = 0;

pixel_t *I_VideoBuffer;

uint8_t __aligned(4) frame_buffer[2][SCREENWIDTH * MAIN_VIEWHEIGHT];
static uint32_t clut[256];                              // RGB888 for the LTDC

// Expose the live palette so the firmware overlay can pick legible colors.
const uint32_t *I_GetClut(void) { return clut; }
static uint8_t shared_pal[NUM_SHARED_PALETTES][16];     // doom palette indices (8bpp path)
static int8_t next_pal = -1;

// Legacy externs from i_system.h; unused in the synchronous model.
semaphore_t render_frame_ready, display_frame_freed;

void I_ShutdownGraphics(void)
{
    initialized = false;
}

void I_StartFrame(void)
{
    // Deferred savestate dump/restore (retrogo_persist.c): this is the
    // stack-shallow between-frames point where rewriting all of doom's RAM
    // (on load) is safe.
    extern void doom_persist_pump(void);
    doom_persist_pump();
}

void I_SetWindowTitle(const char *title)
{
}

//
// I_SetPalette
//
void I_SetPaletteNum(int doompalette)
{
    next_pal = doompalette;
}

void I_FinishUpdate(void)
{
}

uint8_t display_frame_index;
uint8_t display_overlay_index;
uint8_t display_video_type;

uint8_t *wipe_yoffsets;      // position of start of y in each column
int16_t *wipe_yoffsets_raw;
uint32_t *wipe_linelookup;   // absolute address of each line (PICO_ON_DEVICE semantics)
uint8_t next_video_type;
uint8_t next_frame_index;
uint8_t next_overlay_index;
#if !DEMO1_ONLY
uint8_t *next_video_scroll;
uint8_t *video_scroll;
#endif
volatile uint8_t wipe_min;

// ---------------------------------------------------------------------------
// Base-layer scanline builders (8-bit palette indices into line[])
// ---------------------------------------------------------------------------

static void scanline_func_none(uint8_t *dest, int scanline)
{
    memset(dest, 0, SCREENWIDTH);
}

static void scanline_func_double(uint8_t *dest, int scanline)
{
    if (scanline < MAIN_VIEWHEIGHT) {
        memcpy(dest, frame_buffer[display_frame_index] + scanline * SCREENWIDTH, SCREENWIDTH);
    } else {
        // fully overdrawn by the status-bar overlay vpatches
        memset(dest, 0, SCREENWIDTH);
    }
}

static void scanline_func_single(uint8_t *dest, int scanline)
{
    uint8_t *src;
    if (scanline < MAIN_VIEWHEIGHT) {
        src = frame_buffer[display_frame_index] + scanline * SCREENWIDTH;
    } else {
        src = frame_buffer[display_frame_index ^ 1] + (scanline - 32) * SCREENWIDTH;
    }
#if !DEMO1_ONLY
    if (video_scroll) {
        // incremental one-column scroll, written back into the framebuffer
        // (bunny finale) — upstream does exactly this
        for (int i = SCREENWIDTH - 1; i > 0; i--) {
            src[i] = src[i - 1];
        }
        src[0] = video_scroll[scanline];
    }
#endif
    memcpy(dest, src, SCREENWIDTH);
}

static void scanline_func_wipe(uint8_t *dest, int scanline)
{
    const uint8_t *src;
    if (scanline < MAIN_VIEWHEIGHT) {
        src = frame_buffer[display_frame_index];
    } else {
        src = frame_buffer[display_frame_index ^ 1] - 32 * SCREENWIDTH;
    }
    assert(wipe_yoffsets && wipe_linelookup);
    src += scanline * SCREENWIDTH;
    for (int i = 0; i < SCREENWIDTH; i++) {
        int rel = scanline - wipe_yoffsets[i];
        if (rel < 0) {
            dest[i] = src[i];
        } else {
            const uint8_t *flip = (const uint8_t *)wipe_linelookup[rel];
            if (flip >= &frame_buffer[0][0] && flip < &frame_buffer[0][0] + 2 * SCREENWIDTH * MAIN_VIEWHEIGHT) {
                dest[i] = flip[i];
            }
        }
    }
}

typedef void (*scanline_func)(uint8_t *dest, int scanline);
static const scanline_func scanline_funcs[] = {
        scanline_func_none,     // VIDEO_TYPE_NONE
        scanline_func_none,     // VIDEO_TYPE_TEXT (text mode dropped)
        scanline_func_single,   // VIDEO_TYPE_SAVING
        scanline_func_double,   // VIDEO_TYPE_DOUBLE
        scanline_func_single,   // VIDEO_TYPE_SINGLE
        scanline_func_wipe,     // VIDEO_TYPE_WIPE
};

// ---------------------------------------------------------------------------
// vpatch overlay drawing — 8bpp port of pico/i_video.c draw_vpatch (the
// palette[] RGB lookups drop; values written are Doom palette indices).
// The RP2040 stbar XIP-DMA hack is replaced by the plain path.
// ---------------------------------------------------------------------------

static inline uint draw_vpatch(uint8_t *dest, const patch_t *patch, vpatchlist_t *vp, uint off)
{
    int repeat = vp->entry.repeat;
    dest += vp->entry.x;
    int w = vpatch_width(patch);
    const uint8_t *data0 = vpatch_data(patch);
    const uint8_t *data = data0 + off;
    if (!vpatch_has_shared_palette(patch)) {
        const uint8_t *pal = vpatch_palette(patch);
        switch (vpatch_type(patch)) {
            case vp4_runs: {
                uint8_t *p = dest;
                uint8_t *pend = dest + w;
                uint8_t gap;
                while (0xff != (gap = *data++)) {
                    p += gap;
                    int len = *data++;
                    for (int i = 1; i < len; i += 2) {
                        uint v = *data++;
                        *p++ = pal[v & 0xf];
                        *p++ = pal[v >> 4];
                    }
                    if (len & 1) {
                        *p++ = pal[(*data++) & 0xf];
                    }
                    assert(p <= pend);
                    if (p == pend) break;
                }
                break;
            }
            case vp4_alpha: {
                uint8_t *p = dest;
                for (int i = 0; i < w / 2; i++) {
                    uint v = *data++;
                    if (v & 0xf) p[0] = pal[v & 0xf];
                    if (v >> 4) p[1] = pal[v >> 4];
                    p += 2;
                }
                if (w & 1) {
                    uint v = *data++;
                    if (v & 0xf) p[0] = pal[v & 0xf];
                }
                break;
            }
            case vp4_solid: {
                uint8_t *p = dest;
                for (int i = 0; i < w / 2; i++) {
                    uint v = *data++;
                    p[0] = pal[v & 0xf];
                    p[1] = pal[v >> 4];
                    p += 2;
                }
                if (w & 1) {
                    uint v = *data++;
                    p[0] = pal[v & 0xf];
                }
                break;
            }
            case vp6_runs: {
                uint8_t *p = dest;
                uint8_t *pend = dest + w;
                uint8_t gap;
                while (0xff != (gap = *data++)) {
                    p += gap;
                    int len = *data++;
                    for (int i = 3; i < len; i += 4) {
                        uint v = *data++;
                        v |= (*data++) << 8;
                        v |= (*data++) << 16;
                        *p++ = pal[v & 0x3f];
                        *p++ = pal[(v >> 6) & 0x3f];
                        *p++ = pal[(v >> 12) & 0x3f];
                        *p++ = pal[(v >> 18) & 0x3f];
                    }
                    len &= 3;
                    if (len--) {
                        uint v = *data++;
                        *p++ = pal[v & 0x3f];
                        if (len--) {
                            v >>= 6;
                            v |= (*data++) << 2;
                            *p++ = pal[v & 0x3f];
                            if (len--) {
                                v >>= 6;
                                v |= (*data++) << 4;
                                *p++ = pal[v & 0x3f];
                                assert(!len);
                            }
                        }
                    }
                    assert(p <= pend);
                    if (p == pend) break;
                }
                break;
            }
            case vp8_runs: {
                uint8_t *p = dest;
                uint8_t *pend = dest + w;
                uint8_t gap;
                while (0xff != (gap = *data++)) {
                    p += gap;
                    int len = *data++;
                    for (int i = 0; i < len; i++) {
                        *p++ = pal[*data++];
                    }
                    assert(p <= pend);
                    if (p == pend) break;
                }
                break;
            }
            case vp_border: {
                dest[0] = *data++;
                uint8_t col = *data++;
                for (int i = 1; i < w - 1; i++) dest[i] = col;
                dest[w - 1] = *data++;
                break;
            }
            default:
                assert(false);
                break;
        }
    } else {
        uint sp = vpatch_shared_palette(patch);
        assert(sp < NUM_SHARED_PALETTES);
        const uint8_t *pal8 = shared_pal[sp];
        switch (vpatch_type(patch)) {
            case vp4_solid: {
                uint8_t *p = dest;
                for (int i = 0; i < w / 2; i++) {
                    uint v = *data++;
                    p[0] = pal8[v & 0xf];
                    p[1] = pal8[v >> 4];
                    p += 2;
                }
                if (w & 1) {
                    uint v = *data++;
                    p[0] = pal8[v & 0xf];
                }
                break;
            }
            case vp4_alpha: {
                uint8_t *p = dest;
                for (int i = 0; i < w / 2; i++) {
                    uint v = *data++;
                    if (v & 0xf) p[0] = pal8[v & 0xf];
                    if (v >> 4) p[1] = pal8[v >> 4];
                    p += 2;
                }
                if (w & 1) {
                    uint v = *data++;
                    if (v & 0xf) p[0] = pal8[v & 0xf];
                }
                break;
            }
            default:
                assert(false);
        }
    }
    if (repeat) {
        if (vp->entry.patch_handle == VPATCH_M_THERMM) w--; // hackity hack (upstream)
        for (int i = 0; i < repeat * w; i++) {
            dest[w + i] = dest[i];
        }
    }
    return data - data0;
}

// ---------------------------------------------------------------------------
// Per-present frame init: overlay lists, palette -> CLUT, wipe progression.
// Direct port of pico/i_video.c new_frame_init_overlays_palette_and_wipe.
// ---------------------------------------------------------------------------

static void new_frame_init_overlays_palette_and_wipe(void)
{
    if (display_video_type >= FIRST_VIDEO_TYPE_WITH_OVERLAYS) {
        memset(vpatchlists->vpatch_next, 0, sizeof(vpatchlists->vpatch_next));
        memset(vpatchlists->vpatch_starters, 0, sizeof(vpatchlists->vpatch_starters));
        memset(vpatchlists->vpatch_doff, 0, sizeof(vpatchlists->vpatch_doff));
        vpatchlist_t *overlays = vpatchlists->overlays[display_overlay_index];
        // do it in reverse so our linked lists are in ascending order
        for (int i = overlays->header.size - 1; i > 0; i--) {
            assert(overlays[i].entry.y < count_of(vpatchlists->vpatch_starters));
            vpatchlists->vpatch_next[i] = vpatchlists->vpatch_starters[overlays[i].entry.y];
            vpatchlists->vpatch_starters[overlays[i].entry.y] = i;
        }
        if (next_pal != -1) {
            static const uint8_t *playpal;
            static bool calculate_palettes;
            if (!playpal) {
                lumpindex_t l = W_GetNumForName("PLAYPAL");
                playpal = W_CacheLumpNum(l, PU_STATIC);
                // WHX ships only PLAYPAL[0]; pain/bonus/radsuit tints are
                // synthesized procedurally below (mandatory for WHX).
                calculate_palettes = W_LumpLength(l) == 768;
            }
            if (!calculate_palettes || !next_pal) {
                const uint8_t *doompalette = playpal + next_pal * 768;
                for (int i = 0; i < 256; i++) {
                    int r = *doompalette++;
                    int g = *doompalette++;
                    int b = *doompalette++;
                    if (usegamma) {
                        r = gammatable[usegamma - 1][r];
                        g = gammatable[usegamma - 1][g];
                        b = gammatable[usegamma - 1][b];
                    }
                    clut[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                }
            } else {
                int mul, r0, g0, b0;
                if (next_pal < 9) {
                    mul = next_pal * 65536 / 9;
                    r0 = 255; g0 = b0 = 0;
                } else if (next_pal < 13) {
                    mul = (next_pal - 8) * 65536 / 8;
                    r0 = 215; g0 = 186; b0 = 69;
                } else {
                    mul = 65536 / 8;
                    r0 = b0 = 0; g0 = 256;
                }
                const uint8_t *doompalette = playpal;
                for (int i = 0; i < 256; i++) {
                    int r = *doompalette++;
                    int g = *doompalette++;
                    int b = *doompalette++;
                    r += ((r0 - r) * mul) >> 16;
                    g += ((g0 - g) * mul) >> 16;
                    b += ((b0 - b) * mul) >> 16;
                    clut[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                }
            }
            next_pal = -1;
            lcd_set_clut(clut, 256);
            ltdc_push_clut256(clut);
            // 8bpp path: shared palettes hold Doom palette indices, which do
            // not change with the CLUT — refresh is still cheap, keep it.
            for (int i = 0; i < NUM_SHARED_PALETTES; i++) {
                const patch_t *patch = resolve_vpatch_handle(vpatch_for_shared_palette[i]);
                assert(vpatch_colorcount(patch) <= 16);
                assert(vpatch_has_shared_palette(patch));
                for (int j = 0; j < 16; j++) {
                    shared_pal[i][j] = vpatch_palette(patch)[j];
                }
            }
        }
        if (display_video_type == VIDEO_TYPE_WIPE) {
            if (wipe_min <= 200) {
                bool regular = display_overlay_index; // just happens to toggle every frame
                int new_wipe_min = 200;
                for (int i = 0; i < SCREENWIDTH; i++) {
                    int v;
                    if (wipe_yoffsets_raw[i] < 0) {
                        if (regular) {
                            wipe_yoffsets_raw[i]++;
                        }
                        v = 0;
                    } else {
                        int dy = (wipe_yoffsets_raw[i] < 16) ? (1 + wipe_yoffsets_raw[i] + regular) / 2 : 4;
                        if (wipe_yoffsets_raw[i] + dy > 200) {
                            v = 200;
                        } else {
                            wipe_yoffsets_raw[i] += dy;
                            v = wipe_yoffsets_raw[i];
                        }
                    }
                    wipe_yoffsets[i] = v;
                    if (v < new_wipe_min) new_wipe_min = v;
                }
                assert(new_wipe_min >= wipe_min);
                wipe_min = new_wipe_min;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Compose: base layer + overlays per source line, 200->240 row replication
// into the LTDC framebuffer.
// ---------------------------------------------------------------------------

// For each source row: first output row and replication count (1 or 2).
static uint8_t out_row_first[SCREENHEIGHT];
static uint8_t out_row_count[SCREENHEIGHT];

static void compose_frame(void)
{
    uint8_t *out = lcd_get_active_buffer();
    uint8_t line[SCREENWIDTH];
#if DOOMX_TRACE
    uint32_t cyc_base = 0, cyc_overlay = 0, cyc_out = 0, t0;
#define CMP_T0() (t0 = *(volatile uint32_t *)0xE0001004)
#define CMP_ACC(var) (var += *(volatile uint32_t *)0xE0001004 - t0)
#else
#define CMP_T0() ((void)0)
#define CMP_ACC(var) ((void)0)
#endif

    for (int scanline = 0; scanline < SCREENHEIGHT; scanline++) {
        CMP_T0();
        scanline_funcs[display_video_type](line, scanline);
        CMP_ACC(cyc_base);

        CMP_T0();
        if (display_video_type >= FIRST_VIDEO_TYPE_WITH_OVERLAYS) {
            // ordered-insert of overlay vpatches starting on this scanline,
            // then draw all active ones (port of fill_scanlines)
            assert(scanline < count_of(vpatchlists->vpatch_starters));
            int prev = 0;
            for (int vp = vpatchlists->vpatch_starters[scanline]; vp;) {
                int next = vpatchlists->vpatch_next[vp];
                while (vpatchlists->vpatch_next[prev] && vpatchlists->vpatch_next[prev] < vp) {
                    prev = vpatchlists->vpatch_next[prev];
                }
                assert(prev != vp);
                assert(vpatchlists->vpatch_next[prev] != vp);
                vpatchlists->vpatch_next[vp] = vpatchlists->vpatch_next[prev];
                vpatchlists->vpatch_next[prev] = vp;
                prev = vp;
                vp = next;
            }
            vpatchlist_t *overlays = vpatchlists->overlays[display_overlay_index];
            prev = 0;
            for (int vp = vpatchlists->vpatch_next[prev]; vp; vp = vpatchlists->vpatch_next[prev]) {
                const patch_t *patch = resolve_vpatch_handle(overlays[vp].entry.patch_handle);
                int yoff = scanline - overlays[vp].entry.y;
                if (yoff < vpatch_height(patch)) {
                    vpatchlists->vpatch_doff[vp] = draw_vpatch(line, patch, &overlays[vp],
                                                               vpatchlists->vpatch_doff[vp]);
                    prev = vp;
                } else {
                    vpatchlists->vpatch_next[prev] = vpatchlists->vpatch_next[vp];
                }
            }
        }

        CMP_ACC(cyc_overlay);

        CMP_T0();
        uint8_t *dst = out + out_row_first[scanline] * LCD_WIDTH;
        memcpy(dst, line, SCREENWIDTH);
        if (out_row_count[scanline] > 1)
            memcpy(dst + LCD_WIDTH, line, SCREENWIDTH);
        CMP_ACC(cyc_out);
    }
#if DOOMX_TRACE
    TRACE_EVT(TEV_CMP_BASE, cyc_base / 280);
    TRACE_EVT(TEV_CMP_OVERLAY, cyc_overlay / 280);
    TRACE_EVT(TEV_CMP_OUT, cyc_out / 280);
#endif
#undef CMP_T0
#undef CMP_ACC
}

extern boolean uncapped_fps;   // r_main.c -- master switch for uncapped presents

// Called by pd_end_frame in place of sem_release(&render_frame_ready):
// latch and present synchronously.
void I_DisplayFrameReady(void)
{
    display_video_type = next_video_type;
    display_frame_index = next_frame_index;
    display_overlay_index = next_overlay_index;
#if !DEMO1_ONLY
    video_scroll = next_video_scroll;
#endif
    if (display_video_type != VIDEO_TYPE_SAVING) {
        new_frame_init_overlays_palette_and_wipe();
    }
    TRACE_EVT(TEV_COMPOSE_BEG, display_video_type);
    compose_frame();
    TRACE_EVT(TEV_COMPOSE_END, 0);
    if (display_video_type == VIDEO_TYPE_WIPE)
        TRACE_EVT(TEV_WIPE_MARK, wipe_min);
    I_UpdateSound();
    lcd_swap();           // present the just-composed buffer (deferred to vblank
                          // by the firmware's _NoReload swap; pacing is in
                          // I_DisplayFrameFreedWait below)
}

// Called by pd_end_frame where the display_frame_freed wait was. Upstream's
// 60 Hz scanout throttled presents; without a throttle the loop re-renders
// duplicate frames between 35 Hz tics (measured 71 fps, half the CPU wasted).
// Pace presents to doom's 35 Hz tic rate: 28.571 ms fractional (28 + 4/7),
// doomgeneric-style. Idle time feeds the SAI buffer and the perf overlay CPU%.
void I_DisplayFrameFreedWait(void)
{
    void perf_note_idle(int ms);
    static uint32_t next_ms;
    static unsigned frac;

    uint32_t now = (uint32_t)systick_cnt;
    if (next_ms == 0 || (int32_t)(now - next_ms) > 250) {
        next_ms = now;   // first frame / long stall (level load): resync
    }
    uint32_t idle_start = now;
    TRACE_EVT(TEV_IDLE_BEG, 0);
    if (uncapped_fps) {
        // [gnw] Cap the present rate to the panel: block until the previous
        // present's vblank reload has actually landed. lcd_swap() requested it
        // with SRCR = VBR (deferred, via the firmware's _NoReload swap); the
        // hardware clears VBR when the reload happens at the next vertical
        // blank. Waiting for that clear gates the render loop to one present
        // per refresh (~59.64 Hz) and guarantees every rendered frame is
        // actually displayed.
        //
        // This is the RATE CAP, and it is a different job from the tear fix
        // (which is the _NoReload swap in the firmware). It is NOT the
        // front-porch wait that failed earlier -- it does not try to hit the
        // ~0.5 ms blanking window; it just waits for a reload that the hardware
        // completes on its own schedule. Without it the loop free-runs to
        // ~160 fps, burning render work on frames the 60 Hz panel never shows.
        //
        // Bounded so a stopped/reconfigured LTDC (e.g. mid mode-switch) can
        // never wedge the frame loop; ~2 refreshes' worth of polling.
        volatile uint32_t *const LTDC_SRCR = (volatile uint32_t *)0x50001024;
        for (uint32_t guard = 4000000u;
             guard && (*LTDC_SRCR & 0x2u);   // SRCR.VBR: reload still pending
             guard--) {
            I_UpdateSound();           // never let the SAI buffer starve
        }
    } else {
        while ((int32_t)(next_ms - (uint32_t)systick_cnt) > 0) {
            I_UpdateSound();
        }
    }
    TRACE_EVT(TEV_IDLE_END, 0);
    perf_note_idle((int)((uint32_t)systick_cnt - idle_start));

    if (wipestate) {
        // Wipe frames present at ~60 Hz like the RP2040's scanout did —
        // at 35 fps the melt ran 1.7x slower than upstream and read as a
        // hitch. Wipe frames are cheap (no 3D render behind them).
        next_ms += 17;
    } else {
        next_ms += 28;
        frac += 4;
        if (frac >= 7) {
            frac -= 7;
            next_ms += 1;   // 28 + 4/7 ms = 1000/35
        }
    }
    I_UpdateSound();
}

void I_InitGraphics(void)
{
    lcd_setup_framebuffers(LCD_MODE_LUT8);   // retro-go double-buffer (2x LUT8)
    stbar = resolve_vpatch_handle(VPATCH_STBAR);
    (void)stbar;
    pd_init();

    for (int oy = 0; oy < LCD_HEIGHT; oy++) {
        int sy = (oy * SCREENHEIGHT) / LCD_HEIGHT;
        if (!out_row_count[sy])
            out_row_first[sy] = oy;
        out_row_count[sy]++;
    }

    I_VideoBuffer = frame_buffer[0];
    initialized = true;
}

void I_BindVideoVariables(void)
{
}

//
// I_StartTic
//
void I_StartTic(void)
{
    if (!initialized)
    {
        return;
    }
    I_GetEvent();
}

void I_UpdateNoBlit(void)
{
}

int I_GetPaletteIndex(int r, int g, int b)
{
    return 0;
}

void I_Endoom(byte *endoom_data)
{
    // text mode dropped; quit goes straight to power-off
}

void I_GraphicsCheckCommandLine(void)
{
}

void I_CheckIsScreensaver(void)
{
}

void I_DisplayFPSDots(boolean dots_on)
{
}

void I_ReloadClut(void)
{
    lcd_set_clut(I_GetClut(), 256);
    ltdc_push_clut256(I_GetClut());
}

/* Repaint callback for the firmware's blocking menus (open_pause_menu et al.
 * redraw the game beneath the dialog through this). compose_frame's overlay
 * pass CONSUMES the per-frame vpatch lists (vpatch_next / vpatch_doff are
 * mutated in place), so recomposing without re-running the per-frame init
 * walks stale lists and overruns the decoder — rebuild them first, exactly
 * like I_DisplayFrameReady does. */
void I_RepaintFrame(void)
{
    if (display_video_type != VIDEO_TYPE_SAVING)
        new_frame_init_overlays_palette_and_wipe();
    compose_frame();
    /* Blocking firmware menus (pause menu, savestate slot picker) call this
     * each redraw but never let doom's mixer run — keep the DMA ring silent
     * so it doesn't loop the last mixed buffers. */
    extern void audio_clear_active_buffer(void);
    extern void audio_clear_inactive_buffer(void);
    audio_clear_active_buffer();
    audio_clear_inactive_buffer();
}
