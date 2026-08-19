# gnw-doom build machinery. User-facing stages & variables live in
# Makefile.common (included below) — start there.
#
# Engine = the rp2040-doom/ submodule (fork branch gnw-stm32h7b0 = upstream rp2 +
# one minimal engine-hooks commit); everything G&W-specific (src/gnw, config.h,
# linker.ld) lives at the repo root. ONE IWAD per invocation -> build/doom.bin or
# build/doom2.bin (multi-WAD comma lists were dropped; they forced recursive make).

include Makefile.common

CROSS_COMPILE ?= arm-none-eabi-
CC      = $(CROSS_COMPILE)gcc
CXX     = $(CROSS_COMPILE)g++
OBJCOPY = $(CROSS_COMPILE)objcopy

.PHONY: all doom core convert pack build-firmware flash flash-firmware flash-doom \
        start-app flash-dummy clean extflash-image qemu FORCE
.DEFAULT_GOAL := all

FORCE: ;

# Vendored SDK in this repo (pack_core.py + ABI headers).
GNW_CORE_SDK ?= sdk
PACK_CORE := $(GNW_CORE_SDK)/tools/pack_core.py
PACKED_BIN := doom.bin
PAD_LOGO := assets/pad.png
HEADER_LOGO := assets/header.bmp

# CI / stage_release.py metadata (this tree is a CORE only).
PROJECT_KIND := core
CORE_NAME := doom

# --- external flash slots (test-firmware flow only; the payload itself is a
# RAM overlay and is link-address-independent of these) -----------------------
EXTFLASH_OFFSET := $(shell echo $$(($(EXTFLASH_OFFSET_MB) * 1024 * 1024)))
EXTFLASH_OFFSET_ALIGNED := $(shell echo $$(($(EXTFLASH_OFFSET) / 4096 * 4096)))
# WHD slot: right after the GWHB image slot (image <= 724K RAM overlay).
WHD_SLOT_OFFSET := 786432
# Do not export ALIGNED into every recipe env (re-eval races with empty OFFSET).

ENGINE = rp2040-doom
LINKER = linker.ld

# Force a single full-featured engine build (one /cores/doom.bin for all WHDs).
VARIANT := $(CORE_VARIANT)
OUTBIN := build/doom_payload.bin
OUTWHD := build/doom.whd

ifeq ($(VARIANT),)
NEED_WAD := $(filter convert,$(or $(MAKECMDGOALS),all))
# (core build does not require classifying WAD up front)
endif

# Objects land in build/core[/ -trace]
BUILD := build/$(VARIANT)$(if $(filter 1,$(TRACE)),-trace)

# Full WHD format for every convert (matches the full core binary).
WHDFLAGS := -no-super-tiny
FMT_DEFS :=

# RAW_COLUMNS=1 stores graphics columns uncompressed in the WHD. Costs flash,
# but removes the per-frame huffman column decode (the dominant renderer cost).
# The device read path auto-detects it from a patch flag bit, so the engine
# build is format-agnostic; only the data build changes.
RAW_COLUMNS ?= 1
ifeq ($(RAW_COLUMNS),1)
WHDFLAGS += -raw-columns
endif

ifeq ($(TRACE),1)
# The trace pool shares leftover LUT8 LCD bonus with the patch cache
# (lcd_get_bonus_pool). Both must fit in ~150K:
#   pool = NUM_SLOTS * (16 + SLOT_EVENTS*8) bytes.
#
# This SKEWS RESULTS BADLY if you get it wrong. Column decoding is very
# cache-size-sensitive: measured REGCOLS reads 4,203us at an 88K cache but
# 2,750us at 136K — a 35% error — and the ">=18ms busy" tail reads 6.2% vs
# 1.4%. The historical 8x2048 default leaves just 8K of cache (6% of shipping)
# and is worse still.
#
# The aggregate counters (trace_acc_*, the busy histogram) do NOT need the slot
# pool at all -- only tracepull.py's worst-frame detail does. So for any
# accpull.py measurement, shrink the pool to nothing and give the cache back:
#
#   ACCPULL (means, histogram — use this for ALL A/B work):
#     make TRACE=1 TRACE_NUM_SLOTS=1 TRACE_SLOT_EVENTS=64 \
#          TRACE_PATCH_CACHE_BYTES=0x25000        (148K, ~= shipping 150K)
#
#   TRACEPULL (per-frame event detail; accepts the skew as the price):
#     make TRACE=1 TRACE_NUM_SLOTS=3 TRACE_PATCH_CACHE_BYTES=0x16000  (88K)
# Keep TRACE_NUM_SLOTS/TRACE_SLOT_EVENTS in sync with tracepull.py (it reads the
# same names from the environment, and cross-checks slot count against the ELF).
TRACE_PATCH_CACHE_BYTES ?= 0x2000
TRACE_NUM_SLOTS   ?= 8
TRACE_SLOT_EVENTS ?= 2048
# Retain only frames at/above this busy time (us); 0 = keep the worst regardless.
# Set to the frametime budget you are chasing so every slot holds an offender
# instead of the pool being monopolised by one level-load outlier.
TRACE_KEEP_MIN_BUSY_US ?= 0
DEFS_TRACE = -DDOOMX_TRACE=1 -DPATCH_CACHE_BYTES=$(TRACE_PATCH_CACHE_BYTES) \
             -DTRACE_NUM_SLOTS=$(TRACE_NUM_SLOTS) -DTRACE_SLOT_EVENTS=$(TRACE_SLOT_EVENTS) \
             -DTRACE_KEEP_MIN_BUSY_US=$(TRACE_KEEP_MIN_BUSY_US)
LDFLAGS_TRACE =
else
DEFS_TRACE =
LDFLAGS_TRACE =
endif

# --- sources ---------------------------------------------------------------------
DOOM_SRCS = am_map.c d_items.c d_main.c d_net.c doomdef.c doomstat.c dstrings.c \
    f_finale.c f_wipe.c g_game.c hu_lib.c hu_stuff.c info.c m_menu.c m_random.c \
    p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c \
    p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c \
    p_spec.c p_switch.c p_telept.c p_tick.c p_user.c r_bsp.c r_data.c \
    r_data_whd.c r_draw.c r_main.c r_plane.c r_segs.c r_sky.c r_things.c \
    s_sound.c sounds.c statdump.c st_lib.c st_stuff.c wi_stuff.c \
    deh_ammo.c deh_bexstr.c deh_cheat.c deh_doom.c deh_frame.c deh_misc.c \
    deh_ptr.c deh_sound.c deh_thing.c deh_weapon.c

SRC_SRCS = aes_prng.c d_event.c d_iwad.c d_loop.c d_mode.c deh_str.c gusconf.c \
    i_oplmusic.c i_sound.c image_decoder.c m_argv.c m_bbox.c m_cheat.c \
    m_config.c m_controls.c m_fixed.c m_misc.c memio.c midifile.c mus2mid.c \
    musx_decoder.c net_client.c sha1.c tables.c tiny_huff.c v_diskicon.c \
    v_video.c w_checksum.c w_file.c w_file_memory.c w_main.c w_merge.c \
    w_wad.c z_zone.c

OPL_SRCS = opl_api.c emu8950.c

# OPL_SLOT_RENDER=1 swaps emu8950's per-slot loops for upstream's
# slot_render.cpp. NOT bit-exact with the default path (envelope transitions
# can shift by a sample) -- see tools/oplverify. Off by default.
#
# SLOT_RENDER_PORTABLE picks the plain-C++ renderer over the RP2040
# interpolator one; it MUST be global, because it changes struct SLOT_RENDER's
# layout and a per-file mismatch would corrupt memory silently (slot_render.h
# turns that into a hard #error).
OPL_SLOT_RENDER ?= 0
ifeq ($(OPL_SLOT_RENDER),1)
DEFS_SLOT_RENDER = -DEMU8950_SLOT_RENDER=1 -DEMU8950_NO_WAVE_TABLE_MAP=1 \
                   -DSLOT_RENDER_PORTABLE=1
OBJS_SLOT_RENDER = $(BUILD)/opl/slot_render.o
else
DEFS_SLOT_RENDER =
OBJS_SLOT_RENDER =
endif

GNW_SRCS = main_gnw.c i_video_gnw.c i_sound_gnw.c opl_gnw.c i_system_gnw.c \
    i_timer_gnw.c i_input_gnw.c flash_stub.c stubs.c i_glob.c perf_gnw.c \
    trace_gnw.c fastmem.c retrogo_persist.c gwhb_entry.c i_saveg_gnw.c gnw_libc.c

# SDK ABI bridge (not gw_core_entry.S — Doom keeps gwhb_entry.c + linker.ld).
REDEFINE_SYMS := $(GNW_CORE_SDK)/src/gw_core_bridge_redefine_syms.txt
BRIDGE_O := $(BUILD)/sdk/gw_core_bridge.o
# malloc/free → zone (gnw_libc.c); memcpy/memset → fastmem.c ITCM versions.
GW_CORE_BRIDGE_DISABLE_SDK_MALLOC ?= 1
GW_CORE_BRIDGE_DISABLE_SDK_MEMCPY ?= 1
GW_CORE_BRIDGE_DISABLE_SDK_MEMSET ?= 1
GW_CORE_BRIDGE_DISABLE_SDK_MEMMOVE ?= 0

OBJS  = $(DOOM_SRCS:%.c=$(BUILD)/doom/%.o)
OBJS += $(SRC_SRCS:%.c=$(BUILD)/src/%.o)
OBJS += $(OPL_SRCS:%.c=$(BUILD)/opl/%.o)
OBJS += $(GNW_SRCS:%.c=$(BUILD)/src/gnw/%.o)
OBJS += $(BUILD)/src/pd_render.o
OBJS += $(OBJS_SLOT_RENDER)
OBJS += $(BRIDGE_O)

# --- flags -------------------------------------------------------------------------
# compat/ first: freestanding stdio/stdlib shims for the engine.
# Then the core-template SDK headers (same set as sdk/Makefile) so
# gw_firmware_abi.h resolves like every other core — no private ABI mirror.
INCLUDES = -I. -Isrc/gnw -Isrc/gnw/compat \
    -I$(ENGINE)/src -I$(ENGINE)/src/doom -I$(ENGINE)/opl \
    -I$(GNW_CORE_SDK)/src \
    -I$(GNW_CORE_SDK)/include/Core/Inc \
    -I$(GNW_CORE_SDK)/include/Core/Inc/retro-go \
    -I$(GNW_CORE_SDK)/include/Core/Inc/porting \
    -I$(GNW_CORE_SDK)/include/Core/Src/porting/lib \
    -I$(GNW_CORE_SDK)/include/Core/Src/porting/lib/FatFs \
    -I$(GNW_CORE_SDK)/include/retro-go-stm32/components/odroid \
    -I$(GNW_CORE_SDK)/include/Drivers/STM32H7xx_HAL_Driver/Inc \
    -I$(GNW_CORE_SDK)/include/Drivers/STM32H7xx_HAL_Driver/Inc/Legacy \
    -I$(GNW_CORE_SDK)/include/Drivers/CMSIS/Device/ST/STM32H7xx/Include \
    -I$(GNW_CORE_SDK)/include/Drivers/CMSIS/Include

# Engine define lists, transcribed from rp2040-doom/src/CMakeLists.txt
# (doom_tiny / doom_tiny_nost).
# Dropped vs upstream: EMU8950_ASM + EMU8950_SLOT_RENDER (armv6m asm; the
# doomgeneric-proven path is EMU8950_LINEAR), USE_PICO_NET, PICO_SCANVIDEO_*,
# PICO_TIME_*/sbrk/i2c/debug-pin SDK knobs, TINY_WAD_ADDR (linker symbol),
# USE_ZONE_FOR_MALLOC (firmware libc heap serves the few engine mallocs),
# NO_IERROR (we keep I_Error messages).
DEFS_SMALL_COMMON = \
    -DSHRINK_MOBJ=1 -DDOOM_ONLY=1 -DDOOM_SMALL=1 -DDOOM_CONST=1 \
    -DNUM_SOUND_CHANNELS=8 \
    -DNO_USE_CHECKSUM=1 -DNO_USE_RELOAD=1 -DUSE_SINGLE_IWAD=1 -DNO_USE_WIPE=1 \
    -DNO_USE_JOYSTICK=1 -DNO_USE_DEH=1 -DNO_USE_MUSIC_PACKS=1 \
    -DUSE_FLAT_MAX_256=1 -DUSE_MEMMAP_ONLY=1 -DUSE_LIGHTMAP_INDEXES=1 \
    -DUSE_ERASE_FRAME=1 -DNO_DRAW_MID=1 -DNO_DRAW_TOP=1 -DNO_DRAW_BOTTOM=1 \
    -DNO_DRAW_MASKED=1 -DNO_DRAW_SKY=1 -DNO_DRAW_SPRITES=1 -DNO_DRAW_PSPRITES=1 \
    -DNO_VISPLANE_GUTS=1 -DNO_VISPLANE_CACHES=1 -DNO_DRAWSEGS=1 -DNO_VISSPRITES=1 \
    -DNO_MASKED_FLOOR_CLIP=1 \
    -DPD_DRAW_COLUMNS=1 -DPD_DRAW_MARKERS=1 -DPD_DRAW_PLANES=1 -DPD_SCALE_SORT=1 \
    -DPD_CLIP_WALLS=1 -DPD_QUANTIZE=1 -DPD_SANITY=1 -DPD_COLUMNS=1 \
    -DPICO_DOOM=1 -DNO_USE_DS_COLORMAP=1 -DNO_USE_DC_COLORMAP=1 \
    -DUSE_READONLY_MMAP=1 \
    -DNO_USE_TIMIDITY=1 -DNO_USE_GUS=1 -DNO_USE_LIBSAMPLERATE=1 \
    -DEMU8950_NO_TLL=1 \
    -DEMU8950_NO_TIMER=1 -DEMU8950_NO_TEST_FLAG=1 \
    -DEMU8950_LINEAR_SKIP=1 \
    -DEMU8950_NO_PERCUSSION_MODE=1 \
    -DEMU8950_LINEAR=1 \
    -DNO_USE_STATE_MISC \
    -DUSE_RAW_MAPNODE=1 -DUSE_RAW_MAPVERTEX=1 -DUSE_RAW_MAPSEG=1 \
    -DUSE_RAW_MAPLINEDEF=1 -DUSE_RAW_MAPTHING=1 \
    -DUSE_INDEX_LINEBUFFER=1 -DNO_USE_ZLIGHT=1 -DNO_Z_ZONE_ID=1 \
    -DZ_MALOOC_EXTRA_DATA=1 -DUSE_THINKER_POOL=1 -DNO_INTERCEPTS_OVERRUN=1 \
    -DTEMP_IMMUTABLE_DISABLED=1 -DUSE_CONST_SFX=1 -DUSE_CONST_MUSIC=1 \
    -DNO_DEMO_RECORDING=1 -DPICO_NO_TIMING_DEMO=1 -DNO_USE_EXIT=1

DEFS_DOOM_TINY = \
    -DDOOM_TINY=1 -DNO_RDRAW=1 \
    -DUSE_EMU8950_OPL=1 -DUSE_DIRECT_MIDI_LUMP=1 \
    -DNO_USE_NET=1 -DNO_FILE_ACCESS=1 \
    -DSAVE_COMPRESSED=1 -DLOAD_COMPRESSED=1 \
    -DNO_USE_ARGS=1 -DNO_USE_SAVE_CONFIG=1 -DNO_USE_FLOAT=1 \
    -DUSE_VANILLA_KEYBOARD_MAPPING_ONLY=1 -DNO_USE_LOADING_DISK=1 \
    -DUSE_WHD=1 -DNO_Z_MALLOC_USER_PTR=1 \
    -DFIXED_SCREENWIDTH=1 -DFLOOR_CEILING_CLIP_8BIT=1 \
    -DUSE_MUSX=1 -DMUSX_COMPRESSED=1 \
    -DNO_SCREENSHOT=1 -DNO_USE_BOUND_CONFIG=1 -DUSE_FPS=1 \
    -DUSE_MEMORY_WAD=1 -DEMU8950_NO_RATECONV=1 -DNO_ZONE_DEBUG=1

DEFS_RENDER = -DPICODOOM_RENDER_NEWHOPE=1 -DMERGE_DISTSCALE0_INTO_VIEWCOSSINANGLE=1

# Display name unused for WHD path (ACTIVE_FILE selects /roms/doom/*.whd).
GNW_NAME ?= Doom

DEFS_GNW = -DPICO_ON_DEVICE=1 -DPICO_BUILD=1 -DNO_USE_MOUSE=1 \
    -DSTM32H7B0xx -DUSE_HAL_DRIVER -DCOVERFLOW=0 -DCHEAT_CODES=0 \
    -DGW_LCD_MODE_LUT8 \
    -DDOOMX=1 -DDOOMX_SINGLE_CORE=1 -DDOOM_WIDE_PTRS=1 \
    -DDOOM_SAVE_SLOTS=3 -DDOOM_SAVE_AUTONAME=1 \
    -DEXTFLASH_OFFSET=$(EXTFLASH_OFFSET) \
    -DDOOMX_RUNTIME_WHD=1 -DDOOMX_PCACHE_SECTION=1 -DPATCH_CACHE_BYTES=0x25800

# EXTRA_DEFS: ad-hoc defines for A/B experiments, e.g.
#   make TRACE=1 EXTRA_DEFS=-DDOOMX_NO_DTCM_TABLES=1
# NOTE: the object dir is NOT keyed on this, so always build these into a clean
# tree (rm -rf build/<variant>[-trace]) or stale objects silently win.
EXTRA_DEFS ?=

# [gnw] Panel refresh rate + present mode (60/72/75). Drives the LTDC PLL3 in
# the toolkit board.c AND doom's uncapped_fps present mode. Passed to BOTH the
# doom and firmware compiles. NOTE: object dirs are not keyed on this, so build
# into a clean tree when changing it (rm -rf build/<variant> build/fw).
DOOM_REFRESH_HZ ?= 60

# [gnw] CPU overclock (MHz, even). 280 = stock; 312/340 raise PLL1 N. Above 280
# the AHB bus + flash run over datasheet spec (silicon lottery). Firmware-only;
# pass matching --cpu-hz to accpull so cycle->us conversion stays correct.
DOOM_CPU_MHZ ?= 280

# [gnw] Boot state of the perf overlay, in the OPTIONS menu's order:
# 0 = OFF (default), 1 = ON (FPS/CPU/MEM), 2 = FULL (+ memory detail).
# Runtime cycling (GAME+TIME, or OPTIONS > PERF HUD) works regardless; this
# only picks what the build boots with. Doom-side only.
DOOM_PERF_HUD ?= 0

DEFS = $(DEFS_SMALL_COMMON) $(DEFS_DOOM_TINY) $(DEFS_RENDER) $(DEFS_GNW) $(FMT_DEFS) $(DEFS_TRACE) $(EXTRA_DEFS) $(DEFS_SLOT_RENDER) -DDOOM_REFRESH_HZ=$(DOOM_REFRESH_HZ) -DDOOM_PERF_HUD=$(DOOM_PERF_HUD)

COMMON_FLAGS = -mcpu=cortex-m7 -mthumb -O2 -fno-strict-aliasing \
    -nostartfiles -nostdlib -ffreestanding -g -fms-extensions \
    -ffunction-sections -fdata-sections -fno-common -MMD -MP \
    -Wall -Wno-unused-function -Wno-unused-but-set-variable -Wno-unused-variable \
    -Wno-format-truncation \
    $(INCLUDES) $(DEFS)

CFLAGS = $(COMMON_FLAGS) -std=gnu11
CXXFLAGS = $(COMMON_FLAGS) -std=gnu++17 -fno-exceptions -fno-rtti \
    -fno-threadsafe-statics -fno-use-cxa-atexit

# Every core .o except the bridge is rewritten so fopen/lcd_swap/malloc/...
# become core_* and resolve against gw_core_bridge.c trampolines.
define DOOM_REDEFINE
	$(OBJCOPY) --redefine-syms=$(REDEFINE_SYMS) $@
endef

$(BUILD)/doom/%.o: $(ENGINE)/src/doom/%.c $(REDEFINE_SYMS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
	$(DOOM_REDEFINE)

$(BUILD)/src/%.o: $(ENGINE)/src/%.c $(REDEFINE_SYMS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
	$(DOOM_REDEFINE)

$(BUILD)/src/pd_render.o: $(ENGINE)/src/pd_render.cpp $(REDEFINE_SYMS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
	$(DOOM_REDEFINE)

$(BUILD)/opl/%.o: $(ENGINE)/opl/%.c $(REDEFINE_SYMS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
	$(DOOM_REDEFINE)

# slot_render.cpp is upstream's alternative slot renderer (EMU8950_SLOT_RENDER).
# It is C++, hence its own rule rather than the .c pattern above.
$(BUILD)/opl/slot_render.o: $(ENGINE)/opl/slot_render.cpp $(REDEFINE_SYMS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
	$(DOOM_REDEFINE)

$(BUILD)/src/gnw/%.o: src/gnw/%.c $(REDEFINE_SYMS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
	$(DOOM_REDEFINE)

# Do not compile the bridge against src/gnw/compat (FILE=void, fake SEEK_*).
BRIDGE_CFLAGS = $(filter-out -Isrc/gnw -Isrc/gnw/compat,$(CFLAGS)) \
    -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-memset \
    $(if $(filter 1,$(GW_CORE_BRIDGE_DISABLE_SDK_MALLOC)),-DGW_CORE_BRIDGE_DISABLE_SDK_MALLOC=1,) \
    $(if $(filter 1,$(GW_CORE_BRIDGE_DISABLE_SDK_MEMCPY)),-DGW_CORE_BRIDGE_DISABLE_SDK_MEMCPY=1,) \
    $(if $(filter 1,$(GW_CORE_BRIDGE_DISABLE_SDK_MEMSET)),-DGW_CORE_BRIDGE_DISABLE_SDK_MEMSET=1,) \
    $(if $(filter 1,$(GW_CORE_BRIDGE_DISABLE_SDK_MEMMOVE)),-DGW_CORE_BRIDGE_DISABLE_SDK_MEMMOVE=1,)

$(BRIDGE_O): $(GNW_CORE_SDK)/src/gw_core_bridge.c
	@mkdir -p $(dir $@)
	$(CC) $(BRIDGE_CFLAGS) -c $< -o $@

# GNW_NAME no longer baked into objects.
$(BUILD)/src/gnw/main_gnw.o: | $(BUILD)/.dir
$(BUILD)/.dir:
	@mkdir -p $(BUILD)

# --- whd_gen (host tool, shared across variants) + WHD/WHX data -------------------
# Built directly from the submodule's src/whd_gen (which carries the DOOM II
# name-stamp fix in the gnw-stm32h7b0 branch). adpcm-lib.c is C-only -> gcc.
WHD_HOST_CC  ?= gcc
WHD_HOST_CXX ?= g++
WHD_GEN_DIR  := $(ENGINE)/src/whd_gen
WHD_HOST_INC := -I$(ENGINE)/src -I$(ENGINE)/src/doom -I$(WHD_GEN_DIR) \
    -I$(ENGINE)/src/adpcm-xq -DIS_WHD_GEN=1
build-host/whd_gen: $(wildcard $(WHD_GEN_DIR)/*.cpp $(WHD_GEN_DIR)/*.h $(ENGINE)/src/adpcm-xq/*) \
                    $(ENGINE)/src/tiny_huff.c $(ENGINE)/src/musx_decoder.c $(ENGINE)/src/image_decoder.c
	@mkdir -p build-host/whdobj
	$(WHD_HOST_CC) -O2 -w $(WHD_HOST_INC) -c $(ENGINE)/src/tiny_huff.c     -o build-host/whdobj/tiny_huff.o
	$(WHD_HOST_CC) -O2 -w $(WHD_HOST_INC) -c $(ENGINE)/src/musx_decoder.c  -o build-host/whdobj/musx_decoder.o
	$(WHD_HOST_CC) -O2 -w $(WHD_HOST_INC) -c $(ENGINE)/src/image_decoder.c -o build-host/whdobj/image_decoder.o
	$(WHD_HOST_CC) -O2 -w $(WHD_HOST_INC) -c $(ENGINE)/src/adpcm-xq/adpcm-lib.c -o build-host/whdobj/adpcm-lib.o
	$(WHD_HOST_CXX) -O2 -std=gnu++17 -w \
	  -Wno-error=missing-template-arg-list-after-template-kw \
	  $(WHD_HOST_INC) \
	  $(WHD_GEN_DIR)/whd_gen.cpp $(WHD_GEN_DIR)/mus2seq.cpp $(WHD_GEN_DIR)/huff.cpp \
	  $(WHD_GEN_DIR)/lodepng.cpp $(WHD_GEN_DIR)/compress_mus.cpp $(WHD_GEN_DIR)/wad.cpp \
	  build-host/whdobj/*.o -o $@

# Unity-re-release WADs carry 426-wide widescreen art the 320-wide engine
# cannot draw (V_DrawPatch RANGECHECK); crop to centered 320 first. (Harmless
# on standard 320-wide WADs.)
$(BUILD)/wad-cropped.wad: $(WAD) scripts/build/wadwide.py
	@mkdir -p $(BUILD)
	python3 scripts/build/wadwide.py $(WAD) $@

# Converted output is named doom1.wad so the objcopy-baked _binary_doom1_wad_*
# symbols stay stable (w_file_memory.c expects them) regardless of game.
$(BUILD)/doom1.wad: $(BUILD)/wad-cropped.wad build-host/whd_gen
	build-host/whd_gen $< $@ $(WHDFLAGS)

# --- link --------------------------------------------------------------------------
$(BUILD)/doom.out: $(OBJS) $(LINKER)
	$(CC) $(CFLAGS) -T $(LINKER) -Wl,-Map=$(BUILD)/main.map -Wl,--gc-sections $(LDFLAGS_TRACE) -o $@ \
	-Wl,--start-group $(OBJS) -lgcc -Wl,--end-group
	@if grep -qE "__cxa_|_ZSt|operator new" $(BUILD)/main.map; then \
	  echo "ERROR: C++ runtime leakage in link map"; exit 1; fi
	@# Assert the hot renderer functions landed in ITCM (addr < 0x10000), not XIP
	@# flash — linker.ld places them by name, so a rename de-opts silently.
	@for f in R_RenderBSPNode R_RenderSegLoop R_StoreWallRange R_ClipSolidWallSegment \
	          R_ClipPassWallSegment R_AddLine R_CheckBBox R_Subsector R_PointOnSide \
	          R_ScaleFromGlobalAngle R_DrawMaskedColumn R_ProjectSprite R_AddSprites; do \
	  a=$$($(CROSS_COMPILE)nm $(BUILD)/doom.out | awk -v f=$$f '$$3==f{print $$1}'); \
	  if [ -z "$$a" ] || [ $$((0x$$a)) -ge $$((0x10000)) ]; then \
	    echo "ERROR: hot renderer $$f not in ITCM (addr 0x$$a) — fix the .itcram_hot list"; exit 1; fi; \
	done

# Flat RAM_EMU payload (stage-1 at offset 0). WHDs are separate /roms/doom/*.whd.
$(OUTBIN): $(BUILD)/doom.out FORCE
	@mkdir -p $(dir $(OUTBIN))
	$(OBJCOPY) -O binary $(BUILD)/doom.out $(OUTBIN)
	@echo "== $(OUTBIN) (full core payload) =="
	$(CROSS_COMPILE)size $(BUILD)/doom.out

# Optional sample WHD from $(WAD) (not required to pack the core).
$(OUTWHD): $(BUILD)/doom1.wad FORCE
	cp $(BUILD)/doom1.wad $@

# Pack CORE header for the launcher (/cores/doom.bin).
# ITCM/AHB code is already inside the RAM_EMU image (stage-1 unpacks it);
# do not auto-detect extra segments.
core pack: $(PACKED_BIN)

$(PACKED_BIN): $(OUTBIN) $(BUILD)/doom.out $(PAD_LOGO) $(HEADER_LOGO) $(PACK_CORE)
	python3 $(PACK_CORE) \
		--elf $(BUILD)/doom.out --bin $(OUTBIN) \
		--system-name "Doom" --dirname doom \
		--extensions "whd" \
		--core-name "Doom" \
		--version 1.0.0 \
		--pad-logo $(PAD_LOGO) \
		--header-logo $(HEADER_LOGO) \
        --logo-invert \
		--no-auto-segments \
		--out $(PACKED_BIN)
	@echo "== $(PACKED_BIN) → /cores/ =="

# Host WAD → WHD (-no-super-tiny for every IWAD; matches the full core).
convert: build-host/whd_gen
	@test -f "$(WAD)" || (echo "error: WAD=$(WAD) not found"; exit 1)
	@mkdir -p build/convert
	python3 scripts/build/wadwide.py "$(WAD)" build/convert/wad-cropped.wad
	build-host/whd_gen build/convert/wad-cropped.wad build/convert/out.whd $(WHDFLAGS)
	cp build/convert/out.whd "$(OUT)"
	@echo "== $(OUT) → copy to /roms/doom/ =="

-include $(OBJS:.o=.d)

# === test firmware (retro-go-porting-toolkit) ======================================
FWDIR = retro-go-porting-toolkit

FW_HAL_SOURCES = \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_ospi.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc_ex.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_gpio.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_spi.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash_ex.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_cortex.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr_ex.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_ltdc.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_mdma.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_adc.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_adc_ex.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rtc.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rtc_ex.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma.c \
$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma_ex.c \
$(FWDIR)/deps/Core/Src/flash.c

FW_SRCS = $(FWDIR)/src/boot.c $(FWDIR)/src/usart.c $(FWDIR)/src/ltdc.c $(FWDIR)/src/libc.c \
       $(FWDIR)/src/test.c $(FWDIR)/src/mm.c $(FWDIR)/src/audio_sai.c \
       $(FWDIR)/src/odroid_system.c $(FWDIR)/src/persist.c $(FWDIR)/src/input.c \
       $(FWDIR)/src/overlay.c $(FWDIR)/src/perf_overlay.c \
       $(FWDIR)/src/lfs_flash.c $(FWDIR)/src/loader.c \
       $(FWDIR)/src/firmware_abi.c \
       $(FWDIR)/deps/littlefs/lfs.c $(FWDIR)/deps/littlefs/lfs_util.c \
       $(FWDIR)/src/board/system_stm32h7xx.c \
       $(FWDIR)/src/board/board.c \
       $(FW_HAL_SOURCES)

FW_LINKER = $(FWDIR)/linker.ld
FW_OBJS = $(FW_SRCS:%.c=build/fw/%.o)

FW_CFLAGS = -mcpu=cortex-m7 -mthumb -std=c11 -mfpu=fpv5-d16 -mfloat-abi=hard --specs=nano.specs
FW_CFLAGS += -nostartfiles -g -nostdlib -ffreestanding
FW_CFLAGS += -I$(FWDIR)/include \
          -I$(FWDIR)/deps/Core/Inc \
          -I$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Inc \
          -I$(FWDIR)/deps/Drivers/STM32H7xx_HAL_Driver/Inc/Legacy \
          -I$(FWDIR)/deps/Drivers/CMSIS/Device/ST/STM32H7xx/Include \
          -I$(FWDIR)/deps/Drivers/CMSIS/Include \
          -I$(FWDIR)/src/board
# APP_SAVE_PREFIX keeps the on-device save names (doom-N.sav / doom.sram) that
# the toolkit's boilerplate default ("app") would otherwise orphan.
# INTFLASH_BANK=2 -> SystemInit sets VTOR to 0x08100000 (must match the link).
FW_CFLAGS += -DEXTFLASH_OFFSET=$(EXTFLASH_OFFSET_ALIGNED) -DAPP_SAVE_PREFIX='"doom"' \
          -DINTFLASH_BANK=$(INTFLASH_BANK) \
          -DDOOM_REFRESH_HZ=$(DOOM_REFRESH_HZ) \
          -DDOOM_CPU_MHZ=$(DOOM_CPU_MHZ) \
          -DUSE_HAL_DRIVER -DSTM32H7B0xx
# LittleFS (vendored): static buffers, no debug/trace/assert bloat.
FW_CFLAGS += -I$(FWDIR)/deps/littlefs -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN \
          -DLFS_NO_ERROR -DLFS_NO_TRACE -DLFS_NO_ASSERT

build/fw/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(FW_CFLAGS) -c $< -o $@

# Symbols only ever called from Doom (resolved via the ABI table), never from
# the firmware itself, so --gc-sections would drop them. Force-keep them.
FW_KEEP_SYMS = -Wl,--undefined=audio_start -Wl,--undefined=audio_stop \
            -Wl,--undefined=audio_pos -Wl,--undefined=audio_clean_range \
            -Wl,--undefined=odroid_system_emu_init -Wl,--undefined=odroid_system_get_path \
            -Wl,--undefined=odroid_system_emu_save_state -Wl,--undefined=odroid_system_emu_load_state \
            -Wl,--undefined=odroid_system_sram_save \
            -Wl,--undefined=rg_blob_write -Wl,--undefined=rg_blob_read \
            -Wl,--undefined=rg_blob_ptr -Wl,--undefined=rg_blob_erase \
            -Wl,--undefined=gnw_input_read -Wl,--undefined=odroid_input_read_gamepad \
            -Wl,--undefined=gnw_overlay_run \
            -Wl,--undefined=lfs_flash_mount -Wl,--undefined=lfs_flash_read \
            -Wl,--undefined=lfs_flash_write -Wl,--undefined=lfs_flash_remove

# Firmware lives in bank2 (0x08100000, the toolkit linker default); boot it
# with `make start-app` — no bank-swap option-byte dance. INTFLASH_BANK=1
# targets bank1 (0x08000000) instead — used for testing.
# NOTE: temporarily defaulted to bank1 for testing — do NOT commit this as 1.
INTFLASH_BANK ?= 1

# The bank must be applied in BOTH places or the image is silently broken:
# -DINTFLASH_BANK sets the VTOR SystemInit programs (system_stm32h7xx.c), while
# __INTFLASH_ORIGIN moves the linker's FLASH region (toolkit linker.ld defaults
# to bank2). Setting only the define would link at 0x08100000 an image whose
# vector table claims 0x08000000.
INTFLASH_ORIGIN = $(if $(filter 1,$(INTFLASH_BANK)),0x08000000,0x08100000)

# LUT8 pipeline -> 160K framebuffer pool (the toolkit linker defaults to the
# RGB565 300K reserve when __FB_BYTES isn't given).
build/firmware.out: $(FW_OBJS) $(FW_LINKER)
	$(CC) $(FW_CFLAGS) -T $(FW_LINKER) -Wl,-Map=build/fw/main.map -Wl,--gc-sections \
	-Wl,--defsym=__FB_BYTES=163840 -Wl,--defsym=__INTFLASH_ORIGIN=$(INTFLASH_ORIGIN) \
	$(FW_KEEP_SYMS) -o $@ \
	-Wl,--start-group $(FW_OBJS) -lgcc -lc -lm -lnosys -Wl,--end-group

build/firmware.bin: build/firmware.out
	$(OBJCOPY) -O binary $< $@

#######################################
# CI helpers + Docker (same image as firmware)
#######################################
.PHONY: print-PROJECT_KIND print-PACKED_BIN print-CORE_NAME print-DOCKER_IMAGE \
        docker docker_pull docker_shell

print-PROJECT_KIND:
	@echo $(PROJECT_KIND)
print-PACKED_BIN:
	@echo $(PACKED_BIN)
print-CORE_NAME:
	@echo $(CORE_NAME)
print-DOCKER_IMAGE:
	@echo $(DOCKER_IMAGE)

RELEASE_VERSION ?= v1.5
DOCKER_REPOSITORY ?= sylverb/retro-go-sd-builder
DOCKER_IMAGE ?= $(DOCKER_REPOSITORY):$(RELEASE_VERSION)

DOCKER_TTY_FLAG := $(shell if [ -t 0 ]; then echo -it; else echo; fi)
DOCKER_USER := $(shell id -u):$(shell id -g)
DOCKER_RUN := docker run --rm $(DOCKER_TTY_FLAG) \
	--user $(DOCKER_USER) \
	-v "$(CURDIR):/opt/workdir" \
	-w /opt/workdir \
	$(DOCKER_IMAGE)

docker: docker_pull
	$(V)$(ECHO) "[ DOCKER ]" $(DOCKER_IMAGE)
	$(V)$(DOCKER_RUN) make --no-print-directory -j$$(nproc) all

docker_pull:
	$(V)if ! docker image inspect $(DOCKER_IMAGE) >/dev/null 2>&1; then \
		$(ECHO) "[ PULL ]" $(DOCKER_IMAGE); \
		docker pull $(DOCKER_IMAGE); \
	fi

docker_shell: docker_pull
	$(DOCKER_RUN) bash

