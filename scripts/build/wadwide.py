#!/usr/bin/env python3
"""Crop widescreen (>320px) patch lumps in a Unity-re-release IWAD to a
centered 320 columns, in place within each lump.

The Unity re-release ("doom-enhanced.wad") ships 426-wide art (TITLEPIC,
STBAR, INTERPIC, WIMAP*, ...). Vanilla-lineage engines hardcode 320 and
whd_gen happily bakes the wide patches through, which then fail
V_DrawPatch's RANGECHECK at runtime (x + width > SCREENWIDTH).

The patch format makes a lossless in-place crop trivial: column pixel data
is addressed by a columnofs[width] table of intra-lump offsets, so cropping
to a centered window is just (a) width := 320, (b) slide the columnofs
window by (w-320)/2 entries. Column data is untouched and stays at the same
offsets; the trailing columnofs entries become dead bytes. Lump sizes and
offsets are unchanged, so the directory needs no rewrite.

Usage: wadwide.py <in.wad> <out.wad>
"""
import struct
import sys

TARGET_W = 320


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = bytearray(open(sys.argv[1], 'rb').read())
    ident, numlumps, dirofs = struct.unpack_from('<4sii', data, 0)
    if ident not in (b'IWAD', b'PWAD'):
        sys.exit('not a WAD')
    cropped = []
    for i in range(numlumps):
        off, size, raw = struct.unpack_from('<ii8s', data, dirofs + 16 * i)
        name = raw.rstrip(b'\0').decode(errors='replace')
        if size < 16:
            continue
        w, h, lo, to = struct.unpack_from('<hhhh', data, off)
        if not (TARGET_W < w <= 1024 and 0 < h <= 200):
            continue
        # sanity check: it must really be a patch (columnofs inside lump)
        if any(struct.unpack_from('<I', data, off + 8 + 4 * c)[0] >= size
               for c in range(w)):
            continue
        start = (w - TARGET_W) // 2
        window = data[off + 8 + 4 * start: off + 8 + 4 * (start + TARGET_W)]
        struct.pack_into('<hhhh', data, off, TARGET_W, h, 0, to)
        data[off + 8: off + 8 + 4 * TARGET_W] = window
        cropped.append(f'{name}({w}x{h})')
    open(sys.argv[2], 'wb').write(data)
    print(f'wadwide: cropped {len(cropped)} lumps to {TARGET_W}w: '
          + ' '.join(cropped))


if __name__ == '__main__':
    main()
