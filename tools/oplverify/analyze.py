#!/usr/bin/env python3
"""Characterise a difference between two oplverify dumps.

A raw "N samples differ" count cannot distinguish an intentional near-silence
artefact from a real defect. What matters is the magnitude of the samples that
differ: a deviation confined to samples that were already essentially inaudible
is acceptable, one that moves loud samples is not.

Usage: analyze.py REF.bin NEW.bin
"""
import struct, sys

ref = open(sys.argv[1], "rb").read()
new = open(sys.argv[2], "rb").read()
n = len(ref) // 4
A = struct.unpack(f"<{n}i", ref)
B = struct.unpack(f"<{n}i", new)

peak = max(max(abs(x) for x in A), 1)
diffs = [(i, x, y) for i, (x, y) in enumerate(zip(A, B)) if x != y]

print(f"samples          : {n}")
print(f"reference peak   : {peak}")
print(f"differing        : {len(diffs)} ({100.0*len(diffs)/n:.3f}%)")
if not diffs:
    sys.exit(0)

deltas = [abs(x - y) for _, x, y in diffs]
print(f"max |delta|      : {max(deltas)}  ({100.0*max(deltas)/peak:.2f}% of peak)")

# The decisive question: how loud were the samples that changed?
mags = sorted(max(abs(x), abs(y)) for _, x, y in diffs)
def pct(p):
    return mags[min(len(mags) - 1, int(p * len(mags)))]
print(f"magnitude of differing samples (max(|ref|,|new|)):")
print(f"    median {pct(0.5)}   p90 {pct(0.9)}   p99 {pct(0.99)}   max {mags[-1]}")

# Bucket by how loud the sample is relative to peak.
bands = [(0.0, 0.001), (0.001, 0.01), (0.01, 0.05), (0.05, 0.2), (0.2, 1.01)]
print("differing samples by loudness band (fraction of peak):")
for lo, hi in bands:
    c = sum(1 for m in mags if lo * peak <= m < hi * peak)
    if c:
        print(f"    {lo*100:6.1f}% - {hi*100:5.1f}% of peak : {c:6d}")

loud = [(i, x, y) for i, x, y in diffs if max(abs(x), abs(y)) > 0.05 * peak]
print(f"\ndiffering samples above 5% of peak: {len(loud)}")
for i, x, y in loud[:5]:
    print(f"    sample {i} (buf {i//256} off {i%256}): ref={x} new={y} delta={y-x}")
