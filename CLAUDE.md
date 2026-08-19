# CLAUDE.md — Doom CORE (Retro-Go SD)

This repository is the **Doom** dynamic core for Game & Watch Retro-Go SD:
a freestanding Cortex-M7 binary loaded into RAM_EMU that talks to the
firmware **only** through `gw_firmware_abi_t`.

```
SD /cores/doom.bin  +  /roms/doom/*.whd
  → launcher copies CORE → RAM_EMU, jumps Thumb entry (app_main_doom)
  → stage-1 (src/gnw/gwhb_entry.c) unpacks ITCM / AXI segments
  → engine runs; WHD selected via ACTIVE_FILE
```

Build: `make` or `make docker` → `doom.bin`. Convert IWADs on host:
`make convert WAD=… OUT=….whd`. Details in `README.md`.

## Layout

| Path | Role |
| ---- | ---- |
| `sdk/` | Vendored ABI headers, `pack_core.py`, linker fragments |
| `src/gnw/` | G&W platform layer (video/sound/input/ABI/persist) |
| `rp2040-doom/` | Engine submodule (Chocolate Doom / rp2040-doom fork) |
| `linker.ld` | Doom RAM-overlay map (ITCM hot set, zone, …) |
| `assets/` | Pad/header logos for `pack_core.py` |

Sync ABI after a firmware change: `./scripts/sync_from_firmware.sh <firmware-tree>`.

## Memory map (STM32H7B0, SD Retro-Go)

`doom.bin` is a RAM overlay. The launcher copies it to `__RAM_EMU_START__`
(`0x2404B000`) and stage-1 (`src/gnw/gwhb_entry.c`) unpacks runtime sections
to their VMAs. WHD data stays separate (`/roms/doom/*.whd`) and is opened via
the firmware ABI.

Addresses must match firmware linker scripts under `sdk/ld/`; Doom placement is
asserted in `linker.ld`.

| Region | Address / size | Used by Doom |
| --- | --- | --- |
| **ITCM** | `0x00000000`, 64 KiB (minus optional null-guard) | `.itcram_hot` (renderer hot paths + selected objects: `pd_render.o`, `p_map.o`, `p_enemy.o`, `p_sight.o`, `p_maputl.o`) |
| **DTCM core window** | `0x20000000`, about 103 KiB usable by cores | `.dtcm_bss` as `NOLOAD` scratch; reserved at startup via `dtc_malloc()` |
| **AHB SRAM** | `0x30000000` | No fixed Doom sections; use runtime allocators only (`malloc` / `ahb_malloc`) |
| **AXI LCD pool** | `0x24000000`..`0x2404B000` | Firmware LUT8 buffers + bonus pool; patch cache placed at runtime via `lcd_get_bonus_pool()` (`PATCH_CACHE_BYTES`) |
| **AXI RAM_EMU payload** | starts `0x2404B000` | `.core_entry`, `.data`, `.bss`, zone heap (`_zone_start.._zone_end`) |
| **AXI cold/warm text** | `TEXT_AXIS_ORIGIN`..`0x24100000` | `.text_axis` (remaining code + rodata) |


### Watchdog and big clears

Feed `wdog_refresh()` during large memsets, WHD loads, and audio catch-up —
WWDG soft-resets with no useful log if starved.

### Cache / DMA

LCD and audio DMA are cache-blind. Clean D-cache before DMA2D/SAI reads
core-produced buffers in cacheable RAM.

## Audio

Firmware owns the SAI/DMA path. Use `audio_start_playing` /
`audio_get_active_buffer` / `common_emu_sound_sync` via the ABI. Sample rate
must be one of the firmware-supported rates; half-buffer ≤ `AUDIO_BUFFER_LENGTH`
(1077).

## Extending the ABI

Append-only on `gw_firmware_abi_t` while v2 is in flux. After firmware change:

```bash
./scripts/sync_from_firmware.sh /path/to/game-and-watch-retro-go-sd
```

`SDK_VERSION` records `FIRMWARE_ABI_VERSION` and `SYNC_DATE`.
