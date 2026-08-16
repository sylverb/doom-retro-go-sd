# Column decode: the 33 -> 75 fps win

Scene-specific frame drops (33-36 fps in a few rooms, 60 elsewhere) traced to
the renderer re-deriving identical pixels every frame. Fixed by caching decoded
columns properly and by storing columns uncompressed. Same geometry, same pixel
count, 2.2x the framerate.

| | before | after |
|---|---|---|
| fps | 33.8 (dipping to 30) | 74.6 locked |
| busy/frame | 22,768 us | 11,129 us |
| REGCOLS | 14,437 us | 5,061 us |
| cycles per delivered pixel | 99.1 | 42.2 |

Pixel volume never changed: 40,700/frame at 0.76x overdraw throughout.

## What it was NOT

Ruled out with hardware measurements. Do not re-investigate these:

- **Lighting / flicker interpolation.** The colormap is a constant-cost LUT
  index (`colormaps + 256 * c.colormap_index`); light level cannot change how
  long a column takes. FLATS/COMPOSE/OVERLAY were identical between a slow room
  and a fast one to within 3%.
- **Audio.** `I_Pico_UpdateSound` only works on a DMA half-flip, so it is a
  fixed tax per unit of wall time, not per scene. MIX per-call was identical.
- **Game logic / interpolation generally.** GTIC is ~155 us, 0.75% of the frame.
- **Overdraw / depth complexity.** Measured 0.76x -- fewer pixels shaded than
  the viewport holds. There is no wasted pixel work anywhere.
- **Column count.** Only 1.35x between slow and fast scenes, against a 3.34x
  cost difference.
- **Patch cache SIZE.** Raising `PATCH_CACHE_BYTES` 8K -> 136K changed nothing,
  because the hit rate was 0% either way. That null result is not evidence that
  size is irrelevant -- see below.

## What it was

`pcache_column` allocated a **whole patch** (`stride * w`, all columns) to serve
the handful of columns actually on screen. A 100px-wide sprite reserved ~12.8K
to cache ~5 visible columns, so ~24 drawables demanded ~300K against a 136K
cache. Measured: **0.00% hit rate, 17 evictions/frame, 696 decodes/frame** --
every sprite column Huffman-decoded from scratch, every frame.

## The fixes

1. **Per-column cache** (`pcol_find`/`pcol_commit`, `pd_render.cpp`). Keyed by
   (patch, column) instead of by patch. Live set is what is drawn (~1080
   columns) rather than total patch width. 2-way set associative.
2. **Separate sprite and wall tables.** One shared table let the two paths evict
   each other; walls key as `numlumps + texture_num`.
3. **`PCOL_STRIDE` 128 -> 136.** Composed wall columns need 129. The old 128
   also silently excluded every `h == 128` patch via the `h + 1 > stride` guard
   -- ~243 of 591 sprite columns per frame were bypassing the cache entirely.
   The stride floor itself is NOT tunable below 128: `col_render` samples
   `source[(frac >> FRACBITS) & 127]`, so every column buffer must be 128 bytes
   regardless of the patch's real height.
4. **`whd_gen -raw-columns`** (`RAW_COLUMNS=1`, default on). Columns stored
   uncompressed; Huffman decode becomes a memcpy. WHD 2.2MB -> 3.1MB (+40.7%),
   ending 24MB into a 64MB part. `RAW_COLUMNS=0` restores the old format; the
   engine reads both (patch header bit 3).

### Corruption bug -- do not reintroduce

`pcol_find` sets the victim's tag to -1 and the caller calls `pcol_commit` only
**after** filling the buffer. Tagging before the fill leaves an entry advertised
as valid holding stale pixels whenever a compose is skipped; the next frame
reads it as a hit. Symptom: distant tall wall columns fail to draw.

## Refresh rate and overclock

`DOOM_REFRESH_HZ` (60/72/75) sets both the firmware LTDC PLL3 and doom's pacing.

- 280MHz / 60Hz -> locked 59.7fps, busy 12,954us (22% headroom). Battery choice.
- 280MHz / 75Hz -> 63.8fps, 18% of frames taking two refreshes. Judder.
- 340MHz / 75Hz -> locked 74.6fps, busy 11,129us (16.5% headroom).

Two effects worth knowing:

- **Raising refresh steals memory bandwidth.** At 280MHz, 60Hz -> 75Hz raised
  per-frame busy ~8% with no rendering change: the LTDC scans out 25% more often
  and competes with the CPU for the bus.
- **Overclock only helps now.** The old "render is memory-bound so overclock
  doesn't help" finding was true while the renderer was memory-bound on Huffman
  decode. With decode gone the workload is compute-bound and clock converts into
  frames.

## Profiling notes

- `DWT CYCCNT` runs at the **core clock**. Tools must be told the overclock
  (`--cpu-hz`) or every timing is wrong by the clock ratio. A locked frame time
  equal to the refresh period is a good self-check that the clock is right.
- The trace slot pool keeps only the N *worst* frames and is gated by
  `TRACE_KEEP_MIN_BUSY_US`. If that gate is above the scene's frame times, the
  pool cannot capture the scene at all and silently holds stale frames. The
  free-running aggregates (`trace_acc_*`, used by `accpull.py`) do not have this
  problem and are the right tool for A/B work.
- `TRACE_NUM_SLOTS`/`TRACE_SLOT_EVENTS` steal from the patch cache. Use
  `TRACE_NUM_SLOTS=1 TRACE_SLOT_EVENTS=64 TRACE_PATCH_CACHE_BYTES=0x22000`.

## Still on the table

`TEV_BSP` never pairs, so the BSP walk + column build is unattributed -- roughly
5,000 us, ~45% of remaining busy time and the largest single unmeasured block.
Fix that pairing before chasing further headroom.
