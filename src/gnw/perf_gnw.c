//
// Performance overlay for gnw-doom.
//
// GAME + TIME cycles: memory detail -> basic -> hidden. Boots to whatever
// DOOM_PERF_HUD selects (off unless the build asks for it).
// The same modes are selectable from DOOM's OPTIONS menu ("PERF HUD",
// OFF / ON / FULL) via perf_overlay_mode() / perf_overlay_set_mode().
// Stats recomputed once per second:
//   - FPS:  perf_frame calls (presents) per wall-clock second.
//   - CPU:  100% - idle. TryRunTics blocks in I_Sleep(1) when there is
//           spare time, so accumulated sleep is the idle headroom proxy.
//   - MEM:  used / total of the single Z_Zone span.
// Detail adds the zone's largest still-allocatable run ("L", the
// fragmentation early-warning) and the firmware malloc heap's
// used/total/largest (emu8950 + engine I_Realloc live there).
//
// Text goes through M_WriteText (hu_font vpatches) into the CURRENT patch
// list; perf_frame is called from pd_end_frame right after ST_FpsDrawer,
// where the overlay list is active — so the text rides the normal overlay
// compose path. V_DrawPatch saturates if the list fills (worst case: lines
// drop in busy scenes, never corruption).
//

#include "doomtype.h"
#include "z_zone.h"
#include "m_misc.h"        // M_snprintf
#include "rg_data.h"       // systick_cnt + gnw_abi() via the ABI table

void M_WriteText(int x, int y, const char *string);
int  M_StringWidth(const char *string);

#define SCR_W 320

#define NUM_LINES 6

static uint32_t window_start = 0;   // ms at start of the current 1s window
static int      frames = 0;         // frames presented in the current window
static int      idle_ms = 0;        // idle ms accumulated in the current window

static char lines[NUM_LINES][28];
static int  num_lines = 2;

// 0 = basic (FPS/CPU/MEM), 1 = + memory detail, 2 = hidden.
//
// The boot mode is the Makefile knob DOOM_PERF_HUD, spelled in the menu's
// order so builds and the OPTIONS entry agree: 0/OFF (default), 1/ON,
// 2/FULL. Unset means OFF -- the overlay costs frametime, so a plain
// `make` never ships it on; GAME+TIME (or OPTIONS) cycles it on at runtime.
#ifndef DOOM_PERF_HUD
#define DOOM_PERF_HUD 0
#endif

#if   DOOM_PERF_HUD == 0
#define PERF_BOOT_MODE 2   // hidden
#elif DOOM_PERF_HUD == 1
#define PERF_BOOT_MODE 0   // basic
#elif DOOM_PERF_HUD == 2
#define PERF_BOOT_MODE 1   // + memory detail
#else
#error "DOOM_PERF_HUD must be 0 (off), 1 (on) or 2 (full)"
#endif

static int overlay_mode = PERF_BOOT_MODE;

void perf_note_idle(int ms)
{
    if (ms > 0)
        idle_ms += ms;
}

// --- overlay mode API -------------------------------------------------
// Used by the GAME+TIME combo below and by DOOM's OPTIONS menu entry
// (m_menu.c declares these extern, matching how perf_frame /
// perf_note_idle are wired up).

int perf_overlay_mode(void)
{
    return overlay_mode;
}

void perf_overlay_set_mode(int mode)
{
    if (mode < 0 || mode > 2)
        mode = 2;
    overlay_mode = mode;
    window_start = 0;   // restart the stats window on mode change
    frames = 0;
    idle_ms = 0;
    num_lines = 2;
    lines[0][0] = lines[1][0] = 0;
    if (mode != 2) {
        M_snprintf(lines[0], sizeof(lines[0]), "FPS --");
        M_snprintf(lines[1], sizeof(lines[1]), "CPU --");
    }
}

// basic -> detail -> hidden -> basic
void perf_overlay_cycle(void)
{
    perf_overlay_set_mode((overlay_mode + 1) % 3);
}

// GAME+TIME edge is handled in i_input_gnw.c (calls perf_overlay_cycle) so it
// cannot race the START↔VOLUME swap / firmware pause-menu deferral.

// Firmware malloc-heap stats now come through the ABI (gnw_abi()->rg_heap_stats),
// so the perf overlay no longer reaches into mm.c's block list / heap linker symbols.

static void format_stats(uint32_t elapsed)
{
    int n;

    int fps = (int)((frames * 1000U) / elapsed);

    int cpu = 100 - (int)((idle_ms * 100U) / elapsed);
    if (cpu < 0)   cpu = 0;
    if (cpu > 100) cpu = 100;

    int total_k = (int)Z_ZoneSize() / 1024;
    int used_k  = ((int)Z_ZoneSize() - Z_FreeMemory()) / 1024;

    // Busy time per frame in tenths of ms (frametime: what the frame COST,
    // not the paced 28.6 ms it was given).
    int ft10 = frames ? (int)(((elapsed - idle_ms) * 10U) / frames) : 0;

    M_snprintf(lines[0], sizeof(lines[0]), "FPS %d", fps);
    // The firmware's minimal vsnprintf has no "%%" -> append the literal
    // percent sign by hand.
    n = M_snprintf(lines[1], sizeof(lines[1]), "CPU %d", cpu);
    if (n > 0 && n < (int) sizeof(lines[1]) - 1)
    {
        lines[1][n]     = '%';
        lines[1][n + 1] = '\0';
    }
    num_lines = 2;

    if (overlay_mode != 1)
        return;

    M_snprintf(lines[num_lines++], sizeof(lines[0]), "FT %d.%dMS",
               ft10 / 10, ft10 % 10);
    M_snprintf(lines[num_lines++], sizeof(lines[0]), "MEM %d/%dK", used_k, total_k);
    M_snprintf(lines[num_lines++], sizeof(lines[0]), "ZONE L%dK",
               Z_LargestFree() / 1024);
}

void perf_frame(void)
{
    int i;

    if (overlay_mode == 2)
        return;

    uint32_t now = (uint32_t)systick_cnt;

    if (window_start == 0)
        window_start = now;

    frames++;

    uint32_t elapsed = now - window_start;
    if (elapsed >= 1000)
    {
        format_stats(elapsed);
        frames = 0;
        idle_ms = 0;
        window_start = now;
    }

    // hu_font glyphs are ~8px tall; stack the lines in the top-right,
    // right-aligned with a 2px margin.
    for (i = 0; i < num_lines; i++)
        M_WriteText(SCR_W - M_StringWidth(lines[i]) - 2, 2 + 9 * i, lines[i]);
}
