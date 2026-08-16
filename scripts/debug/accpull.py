#!/usr/bin/env python3
"""Aggregate per-stage profiler for gnw-doom (TRACE=1 builds).

Unlike tracepull.py — which reports the N WORST frames, and is therefore
useless for A/B comparison because worst frames are atypical and vary wildly
in content — this sums EVERY occurrence of every stage over a fixed window
and reports the mean per frame. That is the number to compare between builds.

Protocol (matters more than it looks): the attract demo is deterministic, so
measurements must start at the same point after boot or two runs will be
comparing different demo segments. This script therefore:

    reboot -> wait SETTLE s (demo reaches steady state)
           -> zero the accumulators (opens the window)
           -> wait WINDOW s untouched (no SWD halts: a halt freezes the CPU
              mid-stage and inflates whatever span was open)
           -> read once

BUILD THE TARGET WITH A NEAR-SHIPPING PATCH CACHE, or every number here is
pessimistic. TRACE=1 steals the patch cache to make room for the slot pool, but
the counters this script reads do not use that pool at all:

    make TRACE=1 TRACE_NUM_SLOTS=1 TRACE_SLOT_EVENTS=64 \
         TRACE_PATCH_CACHE_BYTES=0x22000 WAD=...

Getting this wrong is not a rounding error: REGCOLS reads 4,203us at an 88K
cache vs 2,750us at 136K, and the ">=18ms busy" tail reads 6.2% vs 1.4%.
(OPL is cache-insensitive and measures the same either way.)

Usage: accpull.py <binary.out> [--settle S] [--window W] [--no-reboot]
                               [--label NAME] [--json OUT.json]

Compare two runs with:  accpull.py ... --json a.json   then   --json b.json
and diff the printed tables.
"""
import argparse, json, struct, subprocess, sys, time

from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

EV = ["NONE","FRAME","TICS","TICS_END","GTIC","GTIC_END",
      "RENDER","RENDER_END","BSP","BSP_END","FLATS","FLATS_END",
      "FLATDEC","FLATDEC_END","PATCHDEC","PATCHDEC_END",
      "REGCOLS","REGCOLS_END","FUZZ","FUZZ_END",
      "OVERLAY","OVERLAY_END","COMPOSE","COMPOSE_END",
      "MIX","MIX_END","OPL","OPL_END","IDLE","IDLE_END","WIPE",
      "CMP_BASE","CMP_OVERLAY","CMP_OUT","LOAD","LOAD_END"]

ap = argparse.ArgumentParser()
ap.add_argument("binary")
ap.add_argument("--settle", type=float, default=10.0)
ap.add_argument("--window", type=float, default=45.0)
ap.add_argument("--no-reboot", action="store_true")
ap.add_argument("--label", default="run")
ap.add_argument("--json")
ap.add_argument("--cpu-hz", type=int, default=280_000_000)
ap.add_argument("--skip-after-load", type=int, default=0,
                help="also exclude N frames following each level load from the "
                     "gameplay histogram (post-load cache warm-up)")
a = ap.parse_args()


def sym(name):
    """Address + size of a symbol, from the ELF the device is running."""
    for l in subprocess.check_output(["arm-none-eabi-nm", "-S", a.binary]).decode().splitlines():
        p = l.split()
        if len(p) >= 3 and p[-1] == name:
            return int(p[0], 16), (int(p[1], 16) if len(p) == 4 else 0)
    raise SystemExit(f"symbol {name} not found — is this a TRACE=1 build?")


acc_cyc, acc_cyc_sz = sym("trace_acc_cyc")
acc_n,   acc_n_sz   = sym("trace_acc_n")
frame_cyc, _        = sym("trace_acc_frame_cyc")
frames,    _        = sym("trace_acc_frames")
hist,    hist_sz    = sym("trace_busy_hist")
maxcyc,  _          = sym("trace_busy_max_cyc")
maxfr,   _          = sym("trace_busy_max_frame")
phist,   phist_sz   = sym("trace_play_hist")
pmaxcyc, _          = sym("trace_play_max_cyc")
pmaxfr,  _          = sym("trace_play_max_frame")
pframes, _          = sym("trace_play_frames")
lframes, _          = sym("trace_load_frames")
skipvar, _          = sym("trace_play_skip_after_load")
pl_sum,  pl_sz      = sym("trace_postload_sum_us")
pl_n,    _          = sym("trace_postload_n")
pl_max,  _          = sym("trace_postload_max_us")
a_o18,   _          = sym("trace_all_over18")
a_drop,  _          = sym("trace_all_dropped")
p_o18,   _          = sym("trace_play_over18")
p_drop,  _          = sym("trace_play_dropped")
topus,   topus_sz   = sym("trace_play_top_us")
NEV = acc_n_sz // 4
NBUCK = hist_sz // 4
NPOST = pl_sz // 4
BUCKET_US = 2000

if not a.no_reboot:
    subprocess.run(["gnwmanager", "start", "bank1"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"rebooted; settling {a.settle:.0f}s so the demo reaches steady state")
    time.sleep(a.settle)

b = OpenOCDBackend(); b.open()
b.halt()
b.write_memory(acc_cyc, b"\x00" * acc_cyc_sz)
b.write_memory(acc_n,   b"\x00" * acc_n_sz)
b.write_memory(frame_cyc, b"\x00" * 8)
b.write_memory(frames,    b"\x00" * 4)
b.write_memory(hist,      b"\x00" * hist_sz)
b.write_memory(maxcyc,    b"\x00" * 4)
b.write_memory(maxfr,     b"\x00" * 4)
b.write_memory(phist,     b"\x00" * phist_sz)
b.write_memory(pmaxcyc,   b"\x00" * 4)
b.write_memory(pmaxfr,    b"\x00" * 4)
b.write_memory(pframes,   b"\x00" * 4)
b.write_memory(lframes,   b"\x00" * 4)
b.write_memory(pl_sum,    b"\x00" * pl_sz)
b.write_memory(pl_n,      b"\x00" * pl_sz)
b.write_memory(pl_max,    b"\x00" * pl_sz)
for _z in (a_o18, a_drop, p_o18, p_drop):
    b.write_memory(_z, b"\x00" * 4)
b.write_memory(topus,     b"\x00" * topus_sz)
# Set BEFORE the window opens: the device classifies frames live.
b.write_memory(skipvar,   struct.pack("<I", a.skip_after_load))
b.resume(); b.close()

print(f"window open: {a.window:.0f}s untouched")
time.sleep(a.window)

b = OpenOCDBackend(); b.open(); b.halt()
raw_cyc = b.read_memory(acc_cyc, acc_cyc_sz)
raw_n   = b.read_memory(acc_n,   acc_n_sz)
tot_cyc, = struct.unpack("<Q", b.read_memory(frame_cyc, 8))
nfr,     = struct.unpack("<I", b.read_memory(frames, 4))
raw_hist = b.read_memory(hist, hist_sz)
mx,      = struct.unpack("<I", b.read_memory(maxcyc, 4))
mxf,     = struct.unpack("<I", b.read_memory(maxfr, 4))
raw_phist = b.read_memory(phist, phist_sz)
pmx,     = struct.unpack("<I", b.read_memory(pmaxcyc, 4))
pmxf,    = struct.unpack("<I", b.read_memory(pmaxfr, 4))
n_play,  = struct.unpack("<I", b.read_memory(pframes, 4))
n_load,  = struct.unpack("<I", b.read_memory(lframes, 4))
raw_plsum = b.read_memory(pl_sum, pl_sz)
raw_pln   = b.read_memory(pl_n,   pl_sz)
raw_plmax = b.read_memory(pl_max, pl_sz)
ao18,  = struct.unpack("<I", b.read_memory(a_o18, 4))
adrop, = struct.unpack("<I", b.read_memory(a_drop, 4))
po18,  = struct.unpack("<I", b.read_memory(p_o18, 4))
pdrop, = struct.unpack("<I", b.read_memory(p_drop, 4))
raw_top = b.read_memory(topus, topus_sz)
b.resume(); b.close()

if not nfr:
    sys.exit("no frames closed in the window — is the game actually running?")

us = lambda c: c / (a.cpu_hz / 1_000_000)
elapsed_us = us(tot_cyc)

rows = []
for i in range(NEV):
    c, = struct.unpack_from("<Q", raw_cyc, i * 8)
    n, = struct.unpack_from("<I", raw_n, i * 4)
    if n:
        rows.append((EV[i] if i < len(EV) else str(i), us(c), n))
rows.sort(key=lambda r: r[1], reverse=True)

print(f"\n=== {a.label} ===")
print(f"frames {nfr}   wall {elapsed_us/1000:.1f} ms   "
      f"mean frame {elapsed_us/nfr:.1f} us  ({1e6/(elapsed_us/nfr):.1f} fps)")
print(f"{'stage':<12}{'us/frame':>10}{'calls/frame':>13}{'us/call':>10}{'% frame':>9}")
for name, tot, n in rows:
    print(f"{name:<12}{tot/nfr:>10.1f}{n/nfr:>13.2f}{tot/n:>10.1f}"
          f"{100*tot/elapsed_us:>8.1f}%")

# --- busy-time distributions ------------------------------------------
# Wall time is always ~28.5ms (35fps lock), so only BUSY time reveals headroom
# and hitches. Two views: every frame, and gameplay only. Level loads are one
# enormous frame each and the attract demo reloads constantly, so the
# all-frames view is dominated by frames nobody plays through.
def stats(buckets, maxcyc_v, maxfr_v):
    tot = sum(buckets)
    if not tot:
        return None
    over = sum(c for i, c in enumerate(buckets) if (i * BUCKET_US) >= 18000)
    # Median/percentiles from the cumulative histogram; bucket-resolution only.
    def pct(q):
        want, run = q * tot, 0
        for i, c in enumerate(buckets):
            run += c
            if run >= want:
                return (i + 1) * BUCKET_US / 1000.0   # upper edge of the bucket
        return NBUCK * BUCKET_US / 1000.0
    mean_ms = sum(c * (i + 0.5) * BUCKET_US for i, c in enumerate(buckets)) / tot / 1000.0
    return dict(tot=tot, over=over, med=pct(0.5), p95=pct(0.95), p99=pct(0.99),
                mean=mean_ms, worst=us(maxcyc_v) / 1000.0, worstfr=maxfr_v)

all_b  = [struct.unpack_from("<I", raw_hist,  i * 4)[0] for i in range(NBUCK)]
play_b = [struct.unpack_from("<I", raw_phist, i * 4)[0] for i in range(NBUCK)]
A, P = stats(all_b, mx, mxf), stats(play_b, pmx, pmxf)

print(f"\nbusy-time distribution   (skip-after-load = {a.skip_after_load})")
print(f"  {'bucket':>10} {'ALL':>7} {'%cum':>7}   {'GAMEPLAY':>8} {'%cum':>7}")
ca = cp = 0
for i in range(NBUCK):
    if not (all_b[i] or play_b[i]):
        continue
    ca += all_b[i]; cp += play_b[i]
    lo = i * BUCKET_US // 1000
    lbl = f">={lo} ms" if i == NBUCK - 1 else f"{lo}-{lo+BUCKET_US//1000} ms"
    print(f"  {lbl:>10} {all_b[i]:>7} {100*ca/max(1,A['tot']):>6.2f}%   "
          f"{play_b[i]:>8} {100*cp/max(1,P['tot']) if P else 0:>6.2f}%")

BUDGET_MS = 28.571
for nm, d, o18, drop in (("ALL FRAMES", A, ao18, adrop),
                         ("GAMEPLAY ONLY", P, po18, pdrop)):
    if not d:
        continue
    t = d["tot"]
    print(f"\n  {nm}: {t} frames   mean {d['mean']:.2f} ms   "
          f"median {d['med']:.0f} ms   p95 {d['p95']:.0f} ms   p99 {d['p99']:.0f} ms")
    print(f"     worst {d['worst']:.2f} ms (frame {d['worstfr']})")
    # Exact counters, not bucket edges: an 18ms cliff needs better than 2ms
    # resolution, and a 99.99% claim needs exact counts.
    print(f"     >=18.000 ms (headroom target): {o18} "
          f"({100*o18/t:.4f}%)  -> {100*(t-o18)/t:.4f}% under")
    print(f"     >={BUDGET_MS:.3f} ms (DROPPED FRAME):  {drop} "
          f"({100*drop/t:.4f}%)  -> {100*(t-drop)/t:.4f}% under")
    # What resolution does this sample size actually support?
    print(f"     resolution: 1 frame = {100/t:.4f}%; "
          f"{'CAN' if t >= 10000 else 'CANNOT'} distinguish 99.99% "
          f"(needs >=10000 frames, have {t})")

tops = [struct.unpack_from("<I", raw_top, i * 4)[0] for i in range(topus_sz // 4)]
tops = [x for x in tops if x]
if tops:
    print(f"\n  worst {len(tops)} gameplay frames (us): "
          + " ".join(str(x) for x in tops))

print(f"\n  classified: {n_play} gameplay / {n_load} excluded (load + post-load skip)"
      f"  = {100*n_load/max(1,n_play+n_load):.1f}% excluded")

# --- post-load spillover: is a frame right after a load still expensive?
print("\n  post-load spillover (frames following a level load):")
print(f"  {'distance':>9} {'n':>5} {'mean us':>9} {'max us':>9}")
for i in range(NPOST):
    n, = struct.unpack_from("<I", raw_pln, i * 4)
    if not n:
        continue
    ssum, = struct.unpack_from("<I", raw_plsum, i * 4)
    smax, = struct.unpack_from("<I", raw_plmax, i * 4)
    print(f"  {i+1:>9} {n:>5} {ssum//n:>9} {smax:>9}")

if a.json:
    json.dump({"label": a.label, "frames": nfr, "elapsed_us": elapsed_us,
               "mean_frame_us": elapsed_us / nfr,
               "skip_after_load": a.skip_after_load,
               "all_frames": A, "gameplay": P,
               "n_play": n_play, "n_excluded": n_load,
               "stages": {r[0]: {"us_per_frame": r[1]/nfr, "calls": r[2]} for r in rows}},
              open(a.json, "w"), indent=2)
    print(f"\nwrote {a.json}")
