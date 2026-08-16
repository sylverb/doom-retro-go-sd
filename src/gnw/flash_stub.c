//
// Legacy flash-slot primitives. Doom's savegames are now routed through the
// firmware's retro-go storage layer (rg_blob) in p_saveg.c, so the engine's
// native picoflash flash-slot path is no longer used. These remain as harmless
// no-op definitions for the (now dead) upstream write_flash_elements helper and
// any other references; the real OSPI writes happen in src/persist.c.
//

#include "pico.h"
#include "picoflash.h"
#include "picodoom.h"

#include "w_wad.h"

void picoflash_sector_program(uint32_t flash_offs, const uint8_t *data)
{
    (void)flash_offs;
    (void)data;
}

const uint8_t *get_end_of_flash(void)
{
    extern const uint8_t _binary_doom1_wad_end[];
    return _binary_doom1_wad_end;
}
