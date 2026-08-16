#!/usr/bin/env python3
"""Dump the 320x240 L8 framebuffer and decode it through Doom's live palette
into a PNG. Pure SWD, no camera.

Reads the `clut` array (u32 0x00RRGGBB) from the gnw-doom payload; picks the
first build/<variant>/doom.out that defines it. NOTE: predates the LUT8
double-buffer — FB is the fixed legacy address, may show the stale surface."""
import struct, subprocess, sys
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

def sym(binary, name):
    try:
        out = subprocess.check_output(["arm-none-eabi-nm", binary]).decode()
    except subprocess.CalledProcessError:
        return None
    for l in out.splitlines():
        p = l.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    return None

FB=0x24000000; W,H=320,240
b=OpenOCDBackend(); b.open()
try:
    b.halt()
    pal=[]
    ca = None
    for variant in ("shareware", "ultimate", "enhanced", "doom2"):
        ca = sym("build/%s/doom.out" % variant, "clut")
        if ca is not None:
            break
    if ca is None:
        sys.exit("no build/<variant>/doom.out with a clut symbol found")
    raw=b.read_memory(ca,256*4)
    for i in range(256):
        v=struct.unpack_from("<I",raw,i*4)[0]
        pal.append(((v>>16)&0xff,(v>>8)&0xff,v&0xff))
    which="gnw-doom clut"
    fb=b.read_memory(FB, W*H)
finally:
    b.resume(); b.close()

# write PNG
import zlib
def png(path,w,h,rgb):
    def chunk(t,d):
        c=t+d; return struct.pack(">I",len(d))+c+struct.pack(">I",zlib.crc32(c)&0xffffffff)
    raw=bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw += bytes(rgb[y*w+x])
    out=b"\x89PNG\r\n\x1a\n"
    out+=chunk(b"IHDR",struct.pack(">IIBBBBB",w,h,8,2,0,0,0))
    out+=chunk(b"IDAT",zlib.compress(bytes(raw),9))
    out+=chunk(b"IEND",b"")
    open(path,"wb").write(out)

rgb=[pal[idx] for idx in fb]
png(sys.argv[1] if len(sys.argv)>1 else "build/screenshot.png", W,H,rgb)
nz=sum(1 for v in fb if v!=fb[0])
print("palette source:", which)
print("framebuffer: %d/%d pixels differ from corner; %d unique indices"%(nz,W*H,len(set(fb))))
print("saved", sys.argv[1] if len(sys.argv)>1 else "build/screenshot.png")
