#!/usr/bin/env bash
set -e

# Run QEMU in the background of this script
./start.sh &
QEMU_PID=$!

echo "Virtual soak test started (QEMU running as fast as possible)."
echo "Waiting 30 seconds for the attract demo to run through multiple scenes..."
sleep 30

echo "Pulling traces via GDB..."
./pull_from_qemu.sh

echo "Traces pulled! QEMU is still running."
echo "Waiting for you to close the emulator window..."
wait $QEMU_PID
