#!/usr/bin/env python3
"""Detect and localise per-present flicker in the LTDC framebuffer under QEMU.

A flicker is a period-2 alternation: consecutive presents differ, but every
OTHER present matches. Eyeballing a window cannot tell that apart from normal
animation; checksums can, and they also say WHICH REGION alternates, which is
what identifies the mechanism.

Regions follow the compose split in src/gnw/i_video_gnw.c:150-158 --
rows 0..MAIN_VIEWHEIGHT-1 come from doom's frame_buffer (the 3D view), rows
below that are zero-filled and then fully overdrawn by the status-bar overlay
vpatches. If only the lower band alternates, the overlay list is the culprit;
if the whole frame alternates, it is the framebuffer ping-pong.

Usage: qemu_flicker.py [--samples N] [--mon PATH]
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time
import zlib

LTDC_L1CFBAR = 0x500010AC     # layer-1 colour frame buffer address register
FB_W, FB_H = 320, 240
MAIN_VIEWHEIGHT = 168         # i_video.h: SCREENHEIGHT(200) - ST_HEIGHT(32)
DOOM_H = 200                  # doom's logical image; rows below are letterbox


class Mon:
    """QEMU human-monitor over a unix socket."""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(10)
        self.s.connect(path)
        time.sleep(0.3)
        self._drain()

    def _drain(self):
        self.s.settimeout(0.35)
        buf = b""
        try:
            while True:
                b = self.s.recv(65536)
                if not b:
                    break
                buf += b
        except socket.timeout:
            pass
        self.s.settimeout(10)
        return buf.decode("utf-8", "replace")

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        time.sleep(0.25)
        return self._drain()

    def read_u32(self, addr):
        out = self.cmd(f"xp/1xw 0x{addr:08x}")
        for tok in out.replace(":", " ").split():
            if tok.startswith("0x") and len(tok) >= 8 and tok != f"0x{addr:08x}":
                try:
                    return int(tok, 16)
                except ValueError:
                    pass
        return None

    def memsave(self, addr, size, path):
        if os.path.exists(path):
            os.unlink(path)
        self.cmd(f"memsave 0x{addr:08x} {size} \"{path}\"")
        for _ in range(40):
            if os.path.exists(path) and os.path.getsize(path) >= size:
                return True
            time.sleep(0.05)
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=16)
    ap.add_argument("--mon", default="/tmp/gnw-doom-qemu-mon.sock")
    ap.add_argument("--interval", type=float, default=0.05)
    a = ap.parse_args()

    m = Mon(a.mon)
    tmp = tempfile.mkdtemp(prefix="gnwfb-")

    rows = []
    for i in range(a.samples):
        fb = m.read_u32(LTDC_L1CFBAR)
        if not fb or not (0x24000000 <= fb < 0x24100000):
            print(f"sample {i}: bad L1CFBAR {fb!r}", file=sys.stderr)
            time.sleep(a.interval)
            continue
        path = os.path.join(tmp, f"fb{i}.bin")
        if not m.memsave(fb, FB_W * FB_H, path):
            print(f"sample {i}: memsave failed", file=sys.stderr)
            continue
        data = open(path, "rb").read()
        view = data[0:FB_W * MAIN_VIEWHEIGHT]
        stbar = data[FB_W * MAIN_VIEWHEIGHT:FB_W * DOOM_H]
        rows.append((fb,
                     zlib.crc32(data) & 0xffffffff,
                     zlib.crc32(view) & 0xffffffff,
                     zlib.crc32(stbar) & 0xffffffff,
                     sum(stbar)))
        time.sleep(a.interval)

    if len(rows) < 4:
        sys.exit("not enough samples")

    print(f"{'#':>2} {'L1CFBAR':>10} {'whole':>10} {'3Dview':>10} {'statusbar':>10} {'stbar_sum':>10}")
    for i, r in enumerate(rows):
        print(f"{i:>2} 0x{r[0]:08x} {r[1]:>10x} {r[2]:>10x} {r[3]:>10x} {r[4]:>10}")

    def alternation(vals, name):
        """period-2 score: how often v[i] == v[i-2] while v[i] != v[i-1]."""
        if len(vals) < 4:
            return
        same2 = sum(1 for i in range(2, len(vals)) if vals[i] == vals[i - 2])
        diff1 = sum(1 for i in range(1, len(vals)) if vals[i] != vals[i - 1])
        distinct = len(set(vals))
        print(f"  {name:<12} distinct={distinct:<3} "
              f"consecutive-differ={diff1}/{len(vals)-1}  "
              f"matches-two-back={same2}/{len(vals)-2}"
              + ("   <-- PERIOD-2 ALTERNATION" if distinct == 2 and same2 >= len(vals) - 3 else ""))

    print("\nperiodicity:")
    alternation([r[1] for r in rows], "whole")
    alternation([r[2] for r in rows], "3D view")
    alternation([r[3] for r in rows], "status bar")
    alternation([r[0] for r in rows], "L1CFBAR")

    blank = sum(1 for r in rows if r[4] == 0)
    print(f"\nstatus bar fully blank in {blank}/{len(rows)} samples"
          + ("   <-- STATUS BAR DROPPING OUT" if 0 < blank < len(rows) else ""))


if __name__ == "__main__":
    main()
