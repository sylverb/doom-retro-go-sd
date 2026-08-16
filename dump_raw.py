import struct
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend
import sys
import subprocess

BIN = "build/shareware-trace/doom.out"
def sym(name):
    for l in subprocess.check_output(["arm-none-eabi-nm", "-S", BIN]).decode().splitlines():
        p = l.split()
        if len(p) >= 3 and p[-1] == name:
            return int(p[0], 16), (int(p[1], 16) if len(p) == 4 else None)
    return None, None

pool_addr, pool_size = sym("trace_slots")
NUM_SLOTS = 8
SLOT_EVENTS = 2048
SLOT_HDR = 16
SLOT_BYTES = SLOT_HDR + SLOT_EVENTS * 8

b = OpenOCDBackend()
b.open()
b.halt()
raw = b.read_memory(pool_addr, NUM_SLOTS * SLOT_BYTES)
trace_stage = struct.unpack("<I", b.read_memory(sym("trace_stage")[0], 4))[0]
b.resume()
b.close()

EV = ["NONE","FRAME","TICS_BEG","TICS_END","GTIC_BEG","GTIC_END",
      "RENDER_BEG","RENDER_END","BSP_BEG","BSP_END","FLATS_BEG","FLATS_END",
      "FLATDEC_BEG","FLATDEC_END","PATCHDEC_BEG","PATCHDEC_END",
      "REGCOLS_BEG","REGCOLS_END","FUZZ_BEG","FUZZ_END",
      "OVERLAY_BEG","OVERLAY_END","COMPOSE_BEG","COMPOSE_END",
      "MIX_BEG","MIX_END","OPL_BEG","OPL_END","IDLE_BEG","IDLE_END","WIPE",
      "CMP_BASE","CMP_OVERLAY","CMP_OUT","LOAD_BEG","LOAD_END"]

def name(ev):
    return EV[ev] if ev < len(EV) else f"?{ev}"

for i in range(NUM_SLOTS):
    base = i * SLOT_BYTES
    frame_no, dur_cyc, count, trunc = struct.unpack_from("<IIII", raw, base)
    print(f"Slot {i} (stage={trace_stage==i}): frame_no={frame_no} dur_cyc={dur_cyc} count={count} trunc={trunc}")
    
    # Try to parse the first 200 events regardless of count
    evs = []
    for j in range(200):
        off = base + SLOT_HDR + j * 8
        cyc, ev, arg = struct.unpack_from("<IHH", raw, off)
        if cyc == 0 and ev == 0: continue
        evs.append((cyc, ev, arg))
    
    # Print them
    if evs:
        print(f"  First few events:")
        for cyc, ev, arg in evs[:30]:
            print(f"    cyc={cyc:<10} ev={name(ev):<12} arg={arg}")
        print("  ...")
