#!/usr/bin/env bash
# Run the DOOM port under the local QEMU Game & Watch machine model (gwemu,
# a hard fork of QEMU with a real STM32H7B0 model -- see ../qemu-gnw).
#
# This is a FUNCTIONAL dev loop only. QEMU/TCG does not model M7 cycle
# timing, caches, or bus contention, so nothing you measure with a wall
# clock in here means anything about real-hardware framerate.
#
#   scripts/debug/run_qemu.sh            # build image if needed, run windowed
#   scripts/debug/run_qemu.sh --gdb      # same, but halted for gdb on :1234
#
# Env:
#   QEMU_GNW   path to the qemu-gnw checkout (default ../../qemu-gnw)
#   GNW_DISPLAY  display backend (default gwemu; 'sdl' is SDL2 and is not
#                built in this fork's binary)
#
# Notes learned the hard way:
#  - Do NOT add `-d unimp`. It writes unbounded and filled a 16G /tmp in
#    under a minute. `-d guest_errors` alone is safe-ish; still bound it.
#  - The gwemu display backend starts the VM PAUSED (prelaunch). This
#    script issues `cont` over the monitor socket for you.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QEMU_GNW="${QEMU_GNW:-$REPO/../../qemu-gnw}"
QEMU="$QEMU_GNW/build/qemu-system-arm"
MON=/tmp/gnw-doom-qemu-mon.sock   # keep short: AF_UNIX paths max out at 108

[ -x "$QEMU" ] || { echo "no qemu binary at $QEMU (set QEMU_GNW)" >&2; exit 1; }

cd "$REPO"
make build-firmware
make
"$REPO/scripts/build/make_extflash_image.py"

GDB_ARGS=(-s)
[ "${1:-}" = "--gdb" ] && GDB_ARGS=(-s -S)

rm -f "$MON"
"$QEMU" -M gnw-h7b0 \
    -device loader,file=build/firmware.bin,addr=0x08000000,force-raw=on \
    -device loader,file=build/extflash.bin,addr=0x90000000,force-raw=on \
    -audiodev pa,id=snd0 -global gnw-h7b0-sai1.audiodev=snd0 \
    -serial file:build/qemu-serial.log \
    -display "${GNW_DISPLAY:-gwemu}" \
    -monitor "unix:$MON,server,nowait" \
    "${GDB_ARGS[@]}" &
QPID=$!

if [ "${1:-}" != "--gdb" ]; then
    for _ in $(seq 40); do [ -S "$MON" ] && break; sleep 0.25; done
    printf 'cont\n' | socat - "unix-connect:$MON" >/dev/null 2>&1 || true
fi
echo "qemu pid $QPID; monitor: socat - unix-connect:$MON"
echo "guest serial (firmware printf): $REPO/build/qemu-serial.log"
wait $QPID
