#!/usr/bin/env python3
#
# wad_plan.py — classify ONE IWAD for the gnw-doom build.
#
# Usage:  wad_plan.py <wad_path>
#
# Prints "<variant> <output>" on stdout (e.g. "shareware doom.bin",
# "doom2 doom2.bin"); unusable input -> message on stderr, exit 1. The Makefile
# maps the variant onto compile-time defines and builds build/<output>.
# Recognized by sha1; an unrecognized-but-valid IWAD is classified by its first
# map lump (E1M1 -> DOOM I, MAP01 -> DOOM II) with a warning on stderr.
#
import sys, os, hashlib

# sha1 -> (variant, output, human description)
KNOWN = {
    "5b2e249b9c5133ec987b3ea77596381dc0d6bc1d": ("shareware", "doom.bin",  "DOOM shareware"),
    "87651324502044f9a6eed403e48853aa16c93e49": ("ultimate",  "doom.bin",  "Ultimate DOOM"),
    "2a8a1ce0f29497a2781b2902c76115fd60d8bbf8": ("enhanced",  "doom.bin",  "DOOM enhanced / Unity"),
    "2921cf667359fd3a80aba3c0cf62ab39297e7e9e": ("doom2",     "doom2.bin", "DOOM II"),
}

def fail(msg):
    sys.stderr.write("gnw-doom WAD: %s\n" % msg)
    sys.exit(1)

def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def has_lump(path, name):
    """True if the WAD's lump directory contains `name` (e.g. E1M1 / MAP01)."""
    try:
        with open(path, "rb") as f:
            hdr = f.read(12)
            if hdr[:4] not in (b"IWAD", b"PWAD"):
                return False
            numlumps = int.from_bytes(hdr[4:8], "little")
            diroff   = int.from_bytes(hdr[8:12], "little")
            f.seek(diroff)
            d = f.read(numlumps * 16)
            for i in range(numlumps):
                nm = d[i*16+8:i*16+16].rstrip(b"\x00").decode("ascii", "replace")
                if nm == name:
                    return True
    except Exception:
        pass
    return False

def main():
    if len(sys.argv) != 2:
        fail("internal: wad_plan.py <wad_path>")
    path = sys.argv[1]
    if not os.path.exists(path):
        fail("file not found: %s (set WAD=<your-iwad> or provide doom1.wad/doom2.wad)" % path)

    s = sha1_of(path)
    if s in KNOWN:
        variant, output, _ = KNOWN[s]
    elif has_lump(path, "MAP01"):
        variant, output = "doom2", "doom2.bin"
        sys.stderr.write("gnw-doom WAD: WARNING: %s (sha1 %s) is NOT in the "
                         "tested-working IWAD list; building as DOOM II on a "
                         "best-effort basis.\n" % (path, s))
    elif has_lump(path, "E1M1"):
        variant, output = "ultimate", "doom.bin"   # safest full-DOOM-I format
        sys.stderr.write("gnw-doom WAD: WARNING: %s (sha1 %s) is NOT in the "
                         "tested-working IWAD list; building as DOOM I "
                         "('ultimate' format) on a best-effort basis.\n" % (path, s))
    else:
        fail("%s is not a recognizable DOOM IWAD (no E1M1 / MAP01)" % path)

    print("%s %s" % (variant, output))

if __name__ == "__main__":
    main()
