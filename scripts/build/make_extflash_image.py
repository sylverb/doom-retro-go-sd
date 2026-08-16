#!/usr/bin/env python3
"""Pack a flat external-flash image for the QEMU Game & Watch machine model.

On real hardware `make flash-doom` writes two blobs into the device's external
flash at fixed byte offsets (via gnwmanager).  QEMU's gnw-h7b0 machine instead
wants one flat file mapped at 0x90000000, so this script reproduces the same
layout in a file:

    <EXTFLASH_OFFSET_ALIGNED>                     build/doom.bin  (GWHB image)
    <EXTFLASH_OFFSET_ALIGNED> + <WHD_SLOT_OFFSET> build/doom.whd  (game data)

Everything else is 0xFF (erased flash).

Usage (defaults match the Makefile's own variables):
    scripts/build/make_extflash_image.py
    scripts/build/make_extflash_image.py --bin build/doom2.bin --whd build/doom2.whd
"""

import argparse
import os
import sys

DEFAULT_SIZE = 64 * 1024 * 1024
DEFAULT_IMAGE_OFFSET = 20 * 1024 * 1024
DEFAULT_WHD_SLOT = 786432


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bin", default="build/doom.bin", help="GWHB payload image")
    p.add_argument("--whd", default="build/doom.whd", help="converted WAD data")
    p.add_argument("-o", "--out", default="build/extflash.bin", help="output image")
    p.add_argument("--offset", type=int,
                   default=int(os.environ.get("EXTFLASH_OFFSET_ALIGNED",
                                              DEFAULT_IMAGE_OFFSET)),
                   help="byte offset of the payload image")
    p.add_argument("--whd-slot", type=int,
                   default=int(os.environ.get("WHD_SLOT_OFFSET", DEFAULT_WHD_SLOT)),
                   help="offset of the WHD relative to --offset")
    p.add_argument("--size", type=int, default=DEFAULT_SIZE,
                   help="total image size in bytes (default 64 MiB)")
    args = p.parse_args()

    parts = [(args.offset, args.bin), (args.offset + args.whd_slot, args.whd)]

    image = bytearray(b"\xff" * args.size)
    end = 0
    for at, path in parts:
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        if at + len(data) > args.size:
            print(f"error: {path} ({len(data)} B) at {at} overflows the "
                  f"{args.size} B image", file=sys.stderr)
            return 1
        image[at:at + len(data)] = data
        end = max(end, at + len(data))
        print(f"  {path:24s} -> 0x{at:08x} .. 0x{at + len(data):08x} "
              f"({len(data)} B)")

    if parts[0][0] + len(open(args.bin, 'rb').read()) > parts[1][0]:
        print("error: payload image overruns the WHD slot -- raise "
              "WHD_SLOT_OFFSET", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(image)
    print(f"wrote {args.out} ({args.size} B, last byte used 0x{end:08x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
