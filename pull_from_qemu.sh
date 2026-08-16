#!/usr/bin/env bash
set -e
arm-none-eabi-gdb -nx --batch \
  -ex "target remote :1234" \
  -ex "dump binary memory trace_slots.bin &trace_slots (char*)&trace_slots + sizeof(trace_slots)" \
  build/shareware-trace/doom.out
python3 scripts/debug/tracepull.py build/trace.log build/shareware-trace/doom.out
