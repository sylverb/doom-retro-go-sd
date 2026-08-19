# gnw-doom

A [retro-go-sd](https://github.com/sylverb/game-and-watch-retro-go-sd) **CORE**
for DOOM I & II on the Nintendo Game & Watch (STM32H7B0), based on
[rp2040-doom](https://github.com/kilograham/rp2040-doom) /
[Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom).

The Game & Watch port was done by **/proc**; the original work lives in
[slash-proc/gnw-doom](https://github.com/slash-proc/gnw-doom). This repository
is that port packaged as a **standalone Retro-Go SD dynamic core** (vendored
ABI/SDK under `sdk/`, G&W platform code in `src/gnw/`, engine in the
`rp2040-doom/` submodule).

Originally based on [rota1001/stm32h7-baremetal-doom](https://github.com/rota1001/stm32h7-baremetal-doom)
and [doomgeneric](https://github.com/ozkl/doomgeneric) (see the `doomgeneric`
branch), later pivoted to rp2040-doom for its BSP-level loading and in-place
compression — yielding an XIP-able, compressed binary on flash.

One full CORE (`/cores/doom.bin`) loads any `/roms/doom/*.whd` selected by the
launcher (`ACTIVE_FILE`). Requires firmware ABI **v2**. Header logo by
[eduardofilo](https://github.com/eduardofilo).

## Building

Prerequisites:

- `arm-none-eabi-gcc` (a recent Arm GNU toolchain; hard-float `fpv5-d16`)
- host `gcc`/`g++` (builds `whd_gen`, the WAD→WHD converter)
- `python3` + Pillow (`pip install -r requirements.txt`)
- `git submodule update --init rp2040-doom`

```
make                  # → doom.bin (full engine CORE; drop under /cores/)
make convert WAD=doom1.wad OUT=doom1.whd   # host WAD→WHD (-no-super-tiny)
# copy *.whd to /roms/doom/ on the SD card

make docker           # same build inside sylverb/retro-go-sd-builder (no host toolchain)
```

Optional toolkit flash/qemu targets remain for developers (`make build-firmware`,
`make flash`, …) and need the optional `retro-go-porting-toolkit` submodule.

## WAD files

Convert IWADs **on a host PC** (never on-device). Every WHD is built with
`-no-super-tiny` so one full CORE can open shareware, Ultimate, and DOOM II.

- `doom1-shareware.wad` / `doom1.wad` / `doom2.wad` (gitignored): place IWADs
  here locally. Unity re-release WADs are cropped automatically at conversion
  time.

Copy the resulting `.whd` files to `/roms/doom/` on the SD card.

## Memory map (RAM-overlay model)

`doom.bin` is a RAM overlay. The launcher copies it to `__RAM_EMU_START__`
(`0x2404B000`) and jumps to stage-1 (`src/gnw/gwhb_entry.c`), which unpacks
runtime sections to their VMAs. WHD game data stays separate (`/roms/doom/*.whd`)
and is opened through the firmware ABI.

Current model (authoritative sources: `linker.ld`, firmware `sdk/ld/*`):

| Region | Address / size | Used by Doom |
| --- | --- | --- |
| **ITCM** | `0x00000000`, 64 KiB (minus optional null-guard) | `.itcram_hot` only (renderer hot paths + selected objects such as `pd_render.o`, `p_map.o`, `p_enemy.o`, `p_sight.o`, `p_maputl.o`) |
| **DTCM core window** | `0x20000000`, about 103 KiB usable by cores | `.dtcm_bss` as `NOLOAD` scratch. Reserved at startup with `dtc_malloc()` so it remains coherent with firmware's bump allocator |
| **AHB SRAM** | `0x30000000` | **No fixed Doom sections linked here** (firmware-owned persistent/data/audio + heap). Use runtime allocators only (`malloc`/`ahb_malloc`) |
| **AXI LCD pool** | `0x24000000`..`0x2404B000` | Firmware LUT8 buffers + bonus pool. Doom patch cache is runtime-placed from `lcd_get_bonus_pool()` (`PATCH_CACHE_BYTES`) |
| **AXI RAM_EMU payload** | starts `0x2404B000` | `.core_entry` (stage-1), `.data`, `.bss`, zone heap (`_zone_start.._zone_end`) |
| **AXI cold/warm text** | `TEXT_AXIS_ORIGIN`..`0x24100000` | `.text_axis` (remaining code + rodata) |

Important tunables and guards:

- `TEXT_AXIS_ORIGIN` and `ZONE_MIN` in `linker.ld` trade code/rodata headroom vs zone heap.
- `PATCH_CACHE_BYTES` controls runtime patch cache size inside AXI bonus pool.
- ITCM and DTCM placements are `ASSERT`-guarded in `linker.ld` to fail fast on overflow.

## Repo structure (for devs)

```
Makefile.common      user-facing stages & variables (start here)
Makefile             build machinery: WAD classification, engine/platform
                     compile rules, whd_gen host build, dual-link reloc,
                     test-firmware build, gnwmanager flash targets
config.h             stand-in for rp2040-doom's CMake-generated config.h
linker.ld            payload linker script (XIP from extflash, ITCM hot set,
                     .reloc_hdr at image offset 0)
src/gnw/             the G&W platform layer: video/sound/input/timer/system
                     backends, ABI binding (rg_abi.h, abi_stubs.c, rg_data.h),
                     perf overlay, persistence, relocation header, fast mem,
                     pico-SDK compat shims (compat/)
scripts/build/       wad_plan.py (sha1 IWAD classifier), wadwide.py (Unity
                     widescreen crop), gen_reloc_table.py (dual-link diff ->
                     relocation table appended to the blob)
scripts/debug/       doom-specific SWD tools: tracepull.py (TRACE=1 pipeline
                     timing), screenshot.py (framebuffer+CLUT dump)
rp2040-doom/         the engine (submodule): fork branch gnw-stm32h7b0 =
                     upstream rp2 + one minimal engine-hooks commit (~22 files)
retro-go-porting-toolkit/
                     the test firmware (submodule, update=none): minimal
                     retro-go API surface published as an ABI table at
                     VTOR+0x400; owns deps/ (ST HAL, CMSIS, littlefs) and the
                     generic SWD debug tools (scripts/debug/)
```

How it fits together: the build compiles the engine out of `rp2040-doom/`
against `src/gnw/` and links it twice (1 MB apart); `gen_reloc_table.py` diffs
the two images to derive the exact absolute-pointer relocation table, which is
appended to the blob behind the `.reloc_hdr`. The firmware finds the blob on
external flash by header magic, relocates it in place to its install address,
and jumps in; the blob resolves every firmware service at runtime through the
`gw_firmware_abi_t` table at VTOR+0x400 — no link-time coupling in either
direction. Replacing the test firmware with real retro-go is a consumer-side
change only (see the toolkit README).

## License

This project is licensed under **GPLv2**. The shareware WAD remains the
property of id Software and is distributed under its original shareware terms.
