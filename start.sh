#!/usr/bin/env bash
# Launch gwemu (patched Zelda + retro-go dual-boot) on Linux.
# Counterpart to start.bat: run from the repo root; expects the native
# binary in build/ and firmware images in backup/qemu-images/.
# Hold Game+Left during the first ~2 seconds of boot for retro-go.
#
# Perf-probe env vars (GNW_MMIO_RATE / GNW_LTDC_TRACE / GNW_TIMER_LATE)
# are opt-in: export them =1 before launching when you need the trace
# numbers (JPEG register read rates, frame completions, pacing-timer
# lateness). Unset/empty/0 all mean off.
set -euo pipefail
cd "$(dirname "$0")"


/home/doug/Nerd/git/qemu-gnw/build/qemu-system-arm -M gnw-h7b0 \
    -global gnw-h7b0-soc.bank1-image=build/firmware.bin \
    -global gnw-h7b0-soc.extflash-image=build/extflash.bin \
    -audiodev none,id=snd0 -global gnw-h7b0-sai1.audiodev=snd0 \
    -s \
    -icount shift=3,sleep=off \
    -display gwemu "$@"
