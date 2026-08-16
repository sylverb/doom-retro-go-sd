#!/usr/bin/env python3
"""Long continuous flicker capture under QEMU.

Captures the LTDC framebuffer as fast as the monitor allows for a fixed
duration, checksums three regions per frame (whole / 3D view / status band),
and analyses the WHOLE series for repetition of any period -- not just period-2.

A region that should be visually stable but cycles through a small set of
distinct images (period 2, 3, N, or intermittent) is the flicker, and the
region tells you the mechanism. Only if every region simply advances (few
repeats, animation) is the frame logically clean.

The region split is in LTDC output rows. compose_frame() replicates doom's
200 rows into 240 LTDC rows, and the status bar (doom rows 168..199) maps to
roughly LTDC rows 202..239. We checksum three bands and report all.

Usage: qemu_flicker_long.py [--secs 10] [--mon PATH]
"""
import argparse
import os
import socket
import tempfile
import time
import zlib
from collections import Counter

LTDC_L1CFBAR = 0x500010AC
FB_W, FB_H = 320, 240
STBAR_TOP = 202     # LTDC row where the replicated status bar starts (~168*240/200)


class Mon:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(10); self.s.connect(path); time.sleep(0.2); self._drain()

    def _drain(self):
        self.s.settimeout(0.15); buf = b""
        try:
            while True:
                b = self.s.recv(65536)
                if not b: break
                buf += b
        except socket.timeout:
            pass
        self.s.settimeout(10); return buf.decode("utf-8", "replace")

    def cmd(self, c):
        self.s.sendall((c + "\n").encode()); time.sleep(0.05); return self._drain()

    def read_u32(self, addr):
        out = self.cmd(f"xp/1xw 0x{addr:08x}")
        for tok in out.replace(":", " ").split():
            if tok.startswith("0x") and tok != f"0x{addr:08x}":
                try: return int(tok, 16)
                except ValueError: pass
        return None

    def memsave(self, addr, size, path):
        if os.path.exists(path): os.unlink(path)
        self.cmd(f'memsave 0x{addr:08x} {size} "{path}"')
        for _ in range(40):
            if os.path.exists(path) and os.path.getsize(path) >= size:
                return True
            time.sleep(0.02)
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=float, default=10.0)
    ap.add_argument("--mon", default="/tmp/gnw-doom-qemu-mon.sock")
    a = ap.parse_args()

    m = Mon(a.mon)
    tmp = tempfile.mkdtemp(prefix="gnwlong-")
    p = os.path.join(tmp, "fb.bin")

    series = []   # (fb_addr, crc_whole, crc_view, crc_stbar)
    t_end = time.time() + a.secs
    while time.time() < t_end:
        fb = m.read_u32(LTDC_L1CFBAR)
        if not fb or not (0x24000000 <= fb < 0x24100000):
            continue
        if not m.memsave(fb, FB_W * FB_H, p):
            continue
        d = open(p, "rb").read()
        view = d[0:FB_W * STBAR_TOP]
        stbar = d[FB_W * STBAR_TOP:FB_W * FB_H]
        series.append((fb,
                       zlib.crc32(d) & 0xffffffff,
                       zlib.crc32(view) & 0xffffffff,
                       zlib.crc32(stbar) & 0xffffffff))

    n = len(series)
    print(f"captured {n} frames in {a.secs:.0f}s ({n/a.secs:.1f}/s)")
    if n < 20:
        print("too few frames"); return

    def analyse(vals, name):
        distinct = len(set(vals))
        cnt = Counter(vals)
        # how often a value equals one seen k frames back (k=1..6)
        backrepeat = {k: sum(1 for i in range(k, len(vals)) if vals[i] == vals[i-k])
                      for k in range(1, 7)}
        # longest run of frames drawn from a small recurring set:
        top = cnt.most_common(4)
        top_share = sum(c for _, c in top) / len(vals)
        print(f"\n{name}: {distinct} distinct / {len(vals)} frames")
        print(f"  match k-back: " + "  ".join(f"k{k}={backrepeat[k]}" for k in range(1, 7)))
        print(f"  top4 checksums cover {100*top_share:.0f}% of frames "
              f"(counts {[c for _,c in top]})")
        # verdict
        if distinct <= 6 and top_share > 0.9:
            print(f"  >>> LIKELY FLICKER: cycles through only {distinct} images")
        elif backrepeat[2] > 0.6 * len(vals):
            print(f"  >>> PERIOD-2 ALTERNATION")
        else:
            print(f"  (advancing -- looks like normal animation)")

    analyse([s[1] for s in series], "whole frame")
    analyse([s[2] for s in series], "3D view (rows 0..201)")
    analyse([s[3] for s in series], "status band (rows 202..239)")
    analyse([s[0] for s in series], "L1CFBAR address")


if __name__ == "__main__":
    main()
