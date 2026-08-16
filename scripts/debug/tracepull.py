#!/usr/bin/env python3
"""Pull the gnw-doom pipeline trace pool over SWD and write a timing report.

Priority-retention tracer (TRACE=1 builds; see src/gnw/trace_gnw.h): the
device keeps the N WORST frames it saw, each at full event detail, instead of
a rolling window. This script reads that pool, sorts the kept frames
slowest-first, and writes the per-frame + per-stage breakdown.

Usage: tracepull.py [out.log] [binary.out]

Pool geometry must match the build (-D overrides) — defaults below; or set
TRACE_NUM_SLOTS / TRACE_SLOT_EVENTS in the environment.
"""
import os, struct, subprocess, sys, collections
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

# DWT cycle counter runs at the core clock, which the overlay's OVERCLOCK item
# can change at runtime (280 / 312 / 340 MHz). Profiling at a boosted level with
# the stock value here would scale every duration wrong, so make it settable.
CPU_HZ = int(os.environ.get("CPU_HZ", 280_000_000))
BIN = sys.argv[2] if len(sys.argv) > 2 else "build/shareware/doom.out"

NUM_SLOTS   = int(os.environ.get("TRACE_NUM_SLOTS", 12))
SLOT_EVENTS = int(os.environ.get("TRACE_SLOT_EVENTS", 2048))
SLOT_HDR    = 16                         # frame_no, dur_cyc, count, truncated
SLOT_BYTES  = SLOT_HDR + SLOT_EVENTS * 8

EV = ["NONE","FRAME","TICS_BEG","TICS_END","GTIC_BEG","GTIC_END",
      "RENDER_BEG","RENDER_END","BSP_BEG","BSP_END","FLATS_BEG","FLATS_END",
      "FLATDEC_BEG","FLATDEC_END","PATCHDEC_BEG","PATCHDEC_END",
      "REGCOLS_BEG","REGCOLS_END","FUZZ_BEG","FUZZ_END",
      "OVERLAY_BEG","OVERLAY_END","COMPOSE_BEG","COMPOSE_END",
      "MIX_BEG","MIX_END","OPL_BEG","OPL_END","IDLE_BEG","IDLE_END","WIPE",
      "CMP_BASE","CMP_OVERLAY","CMP_OUT","LOAD_BEG","LOAD_END"]

def sym(name):
    for l in subprocess.check_output(["arm-none-eabi-nm", "-S", BIN]).decode().splitlines():
        p = l.split()
        if len(p) >= 3 and p[-1] == name:
            return int(p[0], 16), (int(p[1], 16) if len(p) == 4 else None)
    raise SystemExit(f"symbol {name} not found — TRACE=1 build flashed?")

pool_addr, pool_size = sym("trace_slots")
if pool_size:                            # cross-check geometry against the ELF
    derived = pool_size // SLOT_BYTES
    if derived and derived != NUM_SLOTS:
        print(f"note: ELF trace_slots holds {derived} slots, using that "
              f"(set TRACE_SLOT_EVENTS if events/slot differs)")
        NUM_SLOTS = derived

if os.path.exists("trace_slots.bin"):
    with open("trace_slots.bin", "rb") as f:
        raw = f.read(NUM_SLOTS * SLOT_BYTES)
else:
    b = OpenOCDBackend(); b.open()
    b.halt()
    raw = b.read_memory(pool_addr, NUM_SLOTS * SLOT_BYTES)
    b.resume(); b.close()

def us(c):
    return (c & 0xFFFFFFFF) / (CPU_HZ / 1_000_000)

# Decode every kept slot into (frame_no, dur_us, truncated, [events]).
slots = []
for i in range(NUM_SLOTS):
    base = i * SLOT_BYTES
    frame_no, dur_cyc, count, trunc = struct.unpack_from("<IIII", raw, base)
    if count == 0 and dur_cyc == 0:
        continue                         # never-filled slot
    evs = []
    for j in range(min(count, SLOT_EVENTS)):
        off = base + SLOT_HDR + j * 8
        cyc, ev, arg = struct.unpack_from("<IHH", raw, off)
        evs.append((cyc, ev, arg))
    slots.append((frame_no, dur_cyc, trunc, evs))

slots.sort(key=lambda s: s[1], reverse=True)   # slowest frame first

out = open(sys.argv[1] if len(sys.argv) > 1 else "build/trace.log", "w")
out.write(f"gnw-doom trace: {len(slots)} retained frames (the worst seen), "
          f"slowest first\ncycle clock {CPU_HZ/1e6:.0f} MHz; durations in us\n\n")

totals = collections.Counter()
counts = collections.Counter()
maxes  = collections.defaultdict(float)

def decode_frame(evs):
    """Pair BEG/END within one frame; return (stage_us, detail_lines)."""
    stack, stage, lines = {}, collections.Counter(), []
    for cyc, ev, arg in evs:
        name = EV[ev] if ev < len(EV) else f"?{ev}"
        if name.endswith("_BEG"):
            stack[name[:-4]] = (cyc, arg)
        elif name.endswith("_END"):
            key = name[:-4]
            if key in stack:
                c0, a0 = stack.pop(key)
                d = us(cyc - c0)
                stage[key] += d
                totals[key] += d; counts[key] += 1
                maxes[key] = max(maxes[key], d)
                if key in ("FLATDEC", "PATCHDEC", "OPL", "MIX"):
                    lines.append(f"      {key} arg={a0} {d:8.1f}us")
        elif name == "WIPE":
            lines.append(f"      WIPE wipe_min={arg}")
        elif name.startswith("CMP_"):
            stage[name] += arg
            totals[name] += arg; counts[name] += 1
            maxes[name] = max(maxes[name], arg)
    return stage, lines

out.write("=== retained frames (slowest first) ===\n")
for frame_no, dur_cyc, trunc, evs in slots:
    total = us(dur_cyc)
    stage, lines = decode_frame(evs)
    idle = stage.get("IDLE", 0.0)
    busy = total - idle
    cpu = 100 * busy / total if total > 0 else 0
    tag = f"  [TRUNCATED +{trunc} events]" if trunc else ""
    out.write(f"frame {frame_no}: total {total:8.1f}us busy {busy:8.1f}us "
              f"cpu {cpu:5.1f}%{tag}\n")
    for k in ("TICS","RENDER","BSP","FLATS","REGCOLS","FUZZ","OVERLAY",
              "COMPOSE","MIX","OPL","IDLE"):
        if k in stage:
            out.write(f"    {k:8} {stage[k]:9.1f}us\n")
    for l in lines:
        out.write(l + "\n")

out.write("\n=== per-stage totals across retained frames ===\n")
out.write(f"{'stage':10} {'calls':>7} {'total us':>12} {'avg us':>9} {'max us':>9}\n")
for k, t in totals.most_common():
    out.write(f"{k:10} {counts[k]:7d} {t:12.1f} {t/counts[k]:9.1f} {maxes[k]:9.1f}\n")
out.close()

dst = sys.argv[1] if len(sys.argv) > 1 else "build/trace.log"
print(f"retained frames: {len(slots)}; report: {dst}")
if slots:
    worst = us(slots[0][1])
    print(f"worst frame: {worst:.1f}us ({1e6/worst:.1f} fps-equivalent)")
    print("\nstage totals (us) across retained frames:")
    for k, t in totals.most_common(12):
        print(f"  {k:10} {t:12.1f} ({counts[k]} calls, avg {t/counts[k]:.1f}, max {maxes[k]:.1f})")
