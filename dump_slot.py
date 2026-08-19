import struct
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend
import sys, subprocess

BIN = "build/shareware-trace/doom.out"
def sym(name):
    for l in subprocess.check_output(["arm-none-eabi-nm", "-S", BIN]).decode().splitlines():
        p = l.split()
        if len(p) >= 3 and p[-1] == name:
            return int(p[0], 16), (int(p[1], 16) if len(p) == 4 else None)
    return None, None

pool_addr, pool_size = sym("trace_slots")
SLOT_BYTES = 16 + 2048 * 8

b = OpenOCDBackend()
b.open()
b.halt()
if pool_size is not None and pool_size <= 8:
    pool_addr = struct.unpack("<I", b.read_memory(pool_addr, 4))[0]
raw = b.read_memory(pool_addr + SLOT_BYTES * 1, SLOT_BYTES)
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

frame_no, dur_cyc, count, trunc = struct.unpack_from("<IIII", raw, 0)
print(f"Slot 1: frame_no={frame_no} dur_cyc={dur_cyc} ({dur_cyc/280000.0:.2f} ms) count={count} trunc={trunc}")

evs = []
for j in range(count):
    off = 16 + j * 8
    cyc, ev, arg = struct.unpack_from("<IHH", raw, off)
    evs.append((cyc, ev, arg))

stack = {}
for cyc, ev, arg in evs:
    evname = name(ev)
    if evname.endswith("_BEG"):
        stack[evname[:-4]] = cyc
    elif evname.endswith("_END"):
        key = evname[:-4]
        if key in stack:
            dur = (cyc - stack[key]) / 280.0
            print(f"{key}: {dur:.1f} us")
            del stack[key]
