#!/usr/bin/env python3
"""Build the app's relocation table by DUAL-LINK DIFF (ground truth).

The app is linked twice at bases differing by a known delta. Every 4-byte word that
differs by *exactly* that delta is a true absolute reference into the image —
compiler relocations, linker-synthesized veneers, everything — with zero false
positives (coincidental in-range data doesn't change with the base) and nothing
missed. This is exact where parsing R_ARM_ABS32 + guessing at veneers was not.

Fills the .reloc_hdr at image offset 0 and appends the offset table, writing the
result to <out.bin>. The firmware then rewrites each listed offset by
(install_addr - link_base) to run the app XIP from wherever it was installed.

Usage: gen_reloc_table.py <doom.out> <main.bin> <alt.bin> <delta> <EXTFLASH_OFFSET> <out.bin>
"""
import sys, struct
from elftools.elf.elffile import ELFFile

elf_path, main_path, alt_path = sys.argv[1], sys.argv[2], sys.argv[3]
delta = int(sys.argv[4], 0)
ext_off = int(sys.argv[5], 0)
out_path = sys.argv[6]
LINK_BASE = 0x90000000 + ext_off
MAGIC = 0x31525844  # "DXR1"

a = open(main_path, "rb").read()
b = open(alt_path, "rb").read()
n = min(len(a), len(b))
img = bytearray(a)
image_size = len(img)

offs = []
for o in range(0, n - 3, 4):
    va = struct.unpack_from("<I", a, o)[0]
    vb = struct.unpack_from("<I", b, o)[0]
    if ((vb - va) & 0xFFFFFFFF) == delta:
        offs.append(o)

# entry: doom_entry symbol (just after the header).
f = ELFFile(open(elf_path, "rb"))
entry_vaddr = None
for s in f.get_section_by_name(".symtab").iter_symbols():
    if s.name == "doom_entry":
        entry_vaddr = s["st_value"] & ~1
        break
assert entry_vaddr is not None, "doom_entry symbol not found"
entry_offset = entry_vaddr - LINK_BASE

# Patch header (magic already set by the C source) and append the table.
struct.pack_into("<IIIIII", img, 0, MAGIC, LINK_BASE, entry_offset,
                 image_size, image_size, len(offs))
for o in offs:
    img += struct.pack("<I", o)

open(out_path, "wb").write(img)
print(f"reloc(diff): link_base=0x{LINK_BASE:08x} entry_off=0x{entry_offset:x} "
      f"image={image_size} relocs={len(offs)} (final {len(img)} bytes)")
