//
// i_saveg_gnw.c — Game & Watch savegame storage backend: in-RAM "SRAM".
//
// The portable engine (p_saveg.c / g_game.c / m_menu.c) does the archive/
// compress and calls the flash-slot seam declared in p_saveg.h. Here the
// slots live in a fixed static RAM region: doom's in-game saves are NOT
// persisted by themselves — the retro-go savestate (retrogo_persist.c) raw-
// snapshots all of doom's RAM, and this region rides along inside it. The
// engine reads saves directly via slot pointers (mem-mapped model), exactly
// like the upstream RP2040 flash backend.
//
#include <stdint.h>
#include <string.h>

#include "pico.h"        // uint, __noinline (Pico SDK compat) — before p_saveg.h
#include "doomtype.h"
#include "p_saveg.h"     // flash_slot_info_t + backend prototypes + SAVESTRINGSIZE

// 3 slots in 32K. rp2040-doom saves are heavily compressed (typically a few
// KB); a save that doesn't fit reports failure like vanilla "disk full".
#define SRAM_SLOTS     3
#define SRAM_SLOT_CAP  10880u                     // bytes per slot
#define SRAM_MAGIC     0x4D525344u                // "DSRM"

typedef struct {
    uint32_t magic;
    uint32_t len[SRAM_SLOTS];                     // 0 = empty
    uint8_t  data[SRAM_SLOTS][SRAM_SLOT_CAP];
} doom_sram_t;

// .bss (AXISRAM) -> captured by the savestate snapshot automatically.
static doom_sram_t doom_sram;

static void sram_init_once(void) {
    if (doom_sram.magic != SRAM_MAGIC) {
        memset(&doom_sram, 0, sizeof doom_sram);
        doom_sram.magic = SRAM_MAGIC;
    }
}

void P_SaveGameGetExistingFlashSlotAddresses(flash_slot_info_t *slots, int count) {
    sram_init_once();
    for (int i = 0; i < count; i++) {
        if (i < SRAM_SLOTS && doom_sram.len[i] >= SAVESTRINGSIZE) {
            slots[i].data = doom_sram.data[i];    // save begins with the description
            slots[i].size = (int)doom_sram.len[i];
        } else {
            slots[i].data = 0;
            slots[i].size = 0;
        }
    }
}

boolean __noinline P_SaveGameWriteFlashSlot(int slot, const uint8_t *buffer, uint size, uint8_t *buffer4k) {
    (void)buffer4k;
    sram_init_once();
    if (slot < 0 || slot >= SRAM_SLOTS)
        return false;
    if (!buffer) {                                // clear the slot
        doom_sram.len[slot] = 0;
        return true;
    }
    if (size > SRAM_SLOT_CAP)
        return false;                             // engine shows "save failed"
    memcpy(doom_sram.data[slot], buffer, size);
    doom_sram.len[slot] = size;
    return true;
}
