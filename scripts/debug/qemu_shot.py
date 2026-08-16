#!/usr/bin/env python3
"""Capture LTDC framebuffer(s) from QEMU and decode through the live CLUT to PNG.

The framebuffer is LUT8, so the palette must come from the guest's live CLUT
(LTDC L1CLUTWR is write-only, so we read doom's shadow copy via the ELF symbol
`clut`). Without that the dump is meaningless indices.

Usage: qemu_shot.py OUT_PREFIX [--count N] [--interval S] [--elf PATH]
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

LTDC_L1CFBAR = 0x500010AC
FB_W, FB_H = 320, 240


class Mon:
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
        time.sleep(0.2)
        return self._drain()

    def read_u32(self, addr):
        out = self.cmd(f"xp/1xw 0x{addr:08x}")
        for tok in out.replace(":", " ").split():
            if tok.startswith("0x") and tok != f"0x{addr:08x}":
                try:
                    return int(tok, 16)
                except ValueError:
                    pass
        return None

    def memsave(self, addr, size, path):
        if os.path.exists(path):
            os.unlink(path)
        self.cmd(f'memsave 0x{addr:08x} {size} "{path}"')
        for _ in range(60):
            if os.path.exists(path) and os.path.getsize(path) >= size:
                return True
            time.sleep(0.05)
        return False


def sym(elf, name):
    for l in subprocess.check_output(["arm-none-eabi-nm", "-S", elf]).decode().splitlines():
        p = l.split()
        if len(p) >= 3 and p[-1] == name:
            return int(p[0], 16), (int(p[1], 16) if len(p) == 4 else 0)
    return None, None


def write_png(path, w, h, rgb):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--count", type=int, default=4)
    ap.add_argument("--interval", type=float, default=0.0)
    ap.add_argument("--mon", default="/tmp/gnw-doom-qemu-mon.sock")
    ap.add_argument("--elf", default="build/shareware/doom.out")
    a = ap.parse_args()

    m = Mon(a.mon)
    tmp = tempfile.mkdtemp(prefix="gnwshot-")

    clut_addr, clut_sz = sym(a.elf, "clut")
    pal = None
    if clut_addr:
        p = os.path.join(tmp, "clut.bin")
        if m.memsave(clut_addr, 256 * 4, p):
            raw = open(p, "rb").read()
            pal = [struct.unpack_from("<I", raw, i * 4)[0] for i in range(256)]
    if not pal:
        print("warning: no CLUT, using greyscale", file=sys.stderr)
        pal = [(i << 16) | (i << 8) | i for i in range(256)]

    for i in range(a.count):
        fb = m.read_u32(LTDC_L1CFBAR)
        path = os.path.join(tmp, f"fb{i}.bin")
        if not fb or not m.memsave(fb, FB_W * FB_H, path):
            print(f"sample {i}: capture failed", file=sys.stderr)
            continue
        idx = open(path, "rb").read()
        rgb = bytearray(FB_W * FB_H * 3)
        for k, v in enumerate(idx):
            c = pal[v]
            rgb[k * 3] = (c >> 16) & 0xff
            rgb[k * 3 + 1] = (c >> 8) & 0xff
            rgb[k * 3 + 2] = c & 0xff
        out = f"{a.prefix}{i}.png"
        write_png(out, FB_W, FB_H, bytes(rgb))
        print(f"{out}  fb=0x{fb:08x}  crc={zlib.crc32(idx) & 0xffffffff:08x}")
        if a.interval:
            time.sleep(a.interval)


if __name__ == "__main__":
    main()
