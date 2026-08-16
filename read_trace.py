import struct
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

b = OpenOCDBackend()
b.open()
b.halt()

# Read trace_play_top_us
raw_top = b.read_memory(0x2406a3a8, 64)
top_us = struct.unpack("<16I", raw_top)

# Read trace_play_hist
raw_hist = b.read_memory(0x2406a470, 68)
hist = struct.unpack("<17I", raw_hist)

b.resume()
b.close()

print("Top 16 worst frames (busy time in microseconds):")
for i, us in enumerate(top_us):
    if us > 0:
        print(f"  {i+1}: {us} us ({us/1000.0:.2f} ms)")

print("\nHistogram of frame busy times (2ms buckets):")
for i, count in enumerate(hist):
    if count > 0:
        if i == 16:
            print(f"  >= 32ms : {count}")
        else:
            print(f"  {i*2:2d}-{i*2+2:2d}ms : {count}")
