# gnw-doom perf/60fps session — handoff

Continuation doc for a fresh session. Written 2026-07-23. Everything below is
verified in-session unless marked THEORY.

## Repos & branches (THREE repos move together)
- **Parent**: `/home/doug/Nerd/git/gnw-doom/gnw-doom` — branch `retro-go-compat`
- **Engine submodule**: `rp2040-doom/` — branch `perf-investigation` (the "newhope"
  renderer, `-DPICODOOM_RENDER_NEWHOPE=1`; fork of upstream rp2040-doom)
- **Firmware submodule**: `retro-go-porting-toolkit/` — branch `perf-investigation`
  (a minimal retro-go-style test firmware; publishes `gw_firmware_abi_t` at VTOR+0x400)
- Reference only (read): `/home/doug/Nerd/git/crispy-doom` (vanilla+interp renderer),
  `/home/doug/Nerd/git/game-and-watch-retro-go-sd` (real retro-go), `/home/doug/Nerd/git/qemu-gnw` (the G&W QEMU fork).

Commit only when the user asks. `INTFLASH_BANK ?= 1` in the Makefile is TEMP for testing
(do not "fix" to 2). Hardware-validation-before-commit is the rule.

## RESOLVED 2026-07-24 — uncapped framerate SHIPPED at 60@60, all HW-validated & committed
The green-collapse and jitter are FIXED (root causes below), a selectable refresh-rate feature
was added, and the perf ceiling was characterized. Default is **60 Hz @ 60 unique positions,
280 MHz** (user called it "damn near perfect"). Committed across the three repos.

- **"Green collapse" root cause = the player's OWN BODY sprite.** Interpolation moves the VIEW
  but not the mobj, so the camera lags the body; vanilla hides your body only incidentally
  (the sprite sits on the camera → `tz<MINZ` cull), and once the camera separates it projects —
  full-screen when near (the "green"), a recognizable doom-guy when farther. FIX: skip
  `thing == viewplayer->mo` in `R_ProjectSprite` (rp2040-doom r_things.c). This SUPERSEDES the
  earlier pd_render.cpp masked-column hack (that was a coincidental patch on the same symptom;
  it's reverted). The whole multi-layer pd_column/bbox/projection investigation below was chasing
  a symptom — the real bug was one never-culled sprite. Lesson: when an interpolated view shows
  geometry a tic-exact view never does, suspect what the camera-on-mobj coincidence was hiding.
- **"Movement happens twice" jitter root cause = interpolation gated on `fractionaltic != 0`.**
  R_UpdateFractionalTic forces frac=0 on the frame a tic commits; that frame then snapped to the
  NEW position while the next interpolated from OLD → every tic drew twice. FIX: gate on
  `uncapped_fps` instead, so frac==0 lerps to OLD (continuous). Matches Crispy (r_main.c).
- **Selectable refresh + present mode** (matches retro-go's lcd_set_refresh_rate approach):
  Makefile knob `DOOM_REFRESH_HZ` (60/72/75) sets the LTDC PLL3 in toolkit board.c AND doom's
  present mode; `DOOM_CPU_MHZ` (280/312/340) sets PLL1. PLL3: input 16 MHz, N=9, R=24/20/20,
  FRACN=0/0/3072 → 6.0/7.2/7.5 MHz pixel (panel 392×255 total). Only DIVR3EN is on (P/Q off) so
  audio/ADC clocks (PLL2) are untouched. Present pairing: 60/75 → uncapped interpolate + cap to
  panel; 72 → uncapped OFF = 35 tic-exact unique, panel shows each ~2×.
- **PERF CEILING: the render is MEMORY-BANDWIDTH bound, not CPU bound.** Overclock 280→340 MHz
  gave ~ZERO frame-time improvement (histogram flat; per-stage table µs drop is the cycle→µs
  divisor, while unattributed memory-stall time grows to fill the gap). ⇒ 280 MHz is the right
  operating point (same pacing, best battery — serves the "lowest CPU for battery" goal).
- **75 Hz tail = REGCOLS (wall+SPRITE columns), not floors/audio/interp.** 8.7% of frames >13.3 ms
  (worst ~31 ms). Sprites are the biggest marginal driver of the worst frames (each visible thing
  injects many masked columns). 75 Hz has a hard VBLANK CLIFF: any frame >13.3 ms drops to 37.5 fps
  (next vblank), which is the "hard fall to 35" the user saw. 60 Hz (16.7 ms budget, p95=14 ms) has
  NO tail → chosen. If 75 is ever revisited: adaptive present-pacing (skip the in-between present
  when the last frame was heavy) or a sprite-masked-column budget/LOD — NOT overclock, NOT floors.
- **Tooling/workflow:** `DOOM_REFRESH_HZ`/`DOOM_CPU_MHZ` Makefile knobs (firmware+doom; pass the
  matching `--cpu-hz` to accpull so cycle→µs stays correct). Tail profiling = tracepull worst-frames
  for stage mix + a tiny cache-safe per-frame geometry ring (rcc/visplanes/segs/sprites) — the big
  slot pool steals patch cache and inflates REGCOLS, so timing must come from a full-cache build.

### STILL AT ZERO (original goals, untouched): keybind swap, CI GWHB DOOM.bin, web builder.
### NEXT (optional): runtime 35/60/75 Video menu with live lcd_set_refresh_rate PLL3 switch.

## COMMITTED, real, hardware-confirmed wins
- **OPL −15.8%** (5,798→4,884 µs/frame). Bit-exactness proven by a host harness now in
  `tools/oplverify/`. rp2040-doom `01bbc281`. The full `slot_render.cpp` port was evaluated
  and REJECTED (doesn't fit ITCM — needs ~17.9K, only ~3.9K free — and not bit-exact); left
  behind `OPL_SLOT_RENDER=0` default-off build knob (`1eea6bf2`).
- **CLUT tearing fix**: toolkit `lcd_swap` now uses `HAL_LTDC_SetAddress_NoReload` so the
  swap defers to vblank (plain `HAL_LTDC_SetAddress` ends in `SRCR=IMR`, immediate reload
  mid-scanout = the grey diagonal band on damage/pickup flash). toolkit `05aba6e`. Also fixed
  an interrupt storm: the line ISR must ack ALL LTDC flags (RRIF from HAL_LTDC_Reload every
  frame), toolkit `fde6d6a`. USER-CONFIRMED on hardware.
- **60fps present cap**: gnw-doom `d1faadd` (a post-swap VBR-completion gate; removing it
  free-ran to 160fps).
- **Toolkit ABI conformance** (`948a260`): populated NULL ABI slots that were hard-faulting
  doom (audio_clear_*, odroid_system_emu_load_state, vprintf, dtcm_malloc), removed libc
  `while(1)` trap stubs that hid working code (strchr/strrchr/strdup), overclock 353→340MHz
  to match retro-go, added a global VOLUME slider.
- **ADC hang fix** (toolkit `be34c60`): `board_get_battery_raw` had an unbounded EOC poll;
  froze the device when the perf overlay read battery with the ADC clock-gated. Now guarded+bounded.
- **whd_gen synthetic vpatch** (rp2040-doom `fbe3807e`): menu-sized text graphics from hu_font,
  so new DOOM menu rows match. Used for the PERF HUD row.
- **DOOM HUD in the Options menu** (m_menu.c `282e1bd6` + perf_gnw.c `ad564b1`): the existing
  GAME+TIME HUD is now also a "PERF HUD OFF/ON/FULL" options row. Text is 2px-blocky (2x hu_font).
- **Tracer/measurement overhaul** (`3fa6aca`, `a419a75`, `9dacb89`): see "Measurement" below.
- **MangoHud-style firmware perf overlay**: SHELVED — pushed to toolkit branch `perf-overlay`
  + tag `perf-overlay-notes`. Works but can't show CPU headroom without an ABI change (ruled out:
  `rg_abi.h` checks ABI version for EQUALITY, so any table change breaks flashed binaries).

## Measurement harness — READ THIS or every number is wrong
- **THE PATCH-CACHE TRAP**: `TRACE=1` steals the 140K patch cache for the trace slot pool.
  Column decode is very cache-sensitive: REGCOLS reads 4,203µs at 88K cache vs 2,751µs at
  136K, and the ">=18ms busy" tail reads 6.2% vs 1.4%. The aggregate counters do NOT need the
  slot pool. For ALL A/B / accpull measurement build with:
  `make TRACE=1 TRACE_NUM_SLOTS=1 TRACE_SLOT_EVENTS=64 TRACE_PATCH_CACHE_BYTES=0x22000 WAD=...`
  (documented in the Makefile and accpull.py headers.) OPL is cache-insensitive.
- **`scripts/debug/accpull.py`**: aggregate per-stage profiler + busy-time histogram. Bakes the
  protocol: reboot → settle 10s (attract demo is deterministic — must start same point) → zero
  counters → untouched window (NO SWD halts mid-window; a halt inflates the open span) → read.
  Use `--window 900` for the tail: 90s windows understate it ~3.5x (reported 1.44%/0 dropped;
  900s showed 4.9%/0.18%).
- **`scripts/debug/tracepull.py`**: per-frame WORST-N detail (needs ≥2 slots — a 1-slot pool
  can't retain). `TRACE_KEEP_MIN_BUSY_US=N` gates retention to frames ≥N µs busy.
- **Measurement facts (280MHz, gameplay, corrected cache)**: mean busy ~9.4ms, ~46.5% CPU
  (NOT 33.7% — the pacer's TryRunTics wait AND audio-mixed-in-idle were mis-bucketed early on;
  audio mixing happens inside I_DisplayFrameFreedWait's idle spin). ~5% of gameplay frames >16.66ms
  busy, 0.18% >28.571ms (dropped). Worst gameplay ~33ms. DOOM II: mean busy 10.64ms, OPL −37%
  (MAP01 music is lighter), REGCOLS +26%.
- **Per-stage (shareware, 280MHz)**: MIX 5,521 / OPL 4,884 (nests in MIX) / REGCOLS 2,752 /
  FLATS 1,301 / COMPOSE 786 / GTIC 621 µs.

## Closed avenues (don't re-litigate)
- **DMA2D**: dead. DMA2D cannot output/blend into an L8 destination on the H7B0 (OPFCCR.CM is
  RGB-only); can't reach DTCM; LTDC already does L8→RGB free. Full research in agent report.
- **DTCM trig-table relocation**: ~0.5% overall, ~1.5% REGCOLS. Reverted, not worth it.
- **REGCOLS micro-opt**: near-irreducible (⅓ pixel loop, ⅓ Huffman decode the cache handles at
  shipping size, ⅓ composite). The 4,203µs "target" was the patch-cache-trap artifact.
- **72Hz frame-doubling / lcd_set_refresh_rate**: real (retro-go has PLL3 reprogram, 50/60/72/75).
  Deferred behind the interpolation work. The user wants selectable 35/60/75 UNIQUE view
  positions/sec (60 & 75 REQUIRE working interpolation).

## THE ACTIVE BUG: uncapped-framerate "green collapse" (NOT fixed)
`uncapped_fps` (rp2040-doom `r_main.c:~1135`, DEFAULT FALSE) decouples present from tic and
interpolates the viewpoint (Crispy-style, viewpoint+psprite only, ZERO RAM — old state lives on
`player_t`, not per-mobj; savegame format unchanged). Tear fix + 60fps cap make presentation
clean. BUT: on FORWARD motion, mid-interpolation frames (fractionaltic≈0.5) collapse — the whole
3D view becomes one floor flat (green), 1-frame transient (frame N good, N+1 collapsed, N+2 good).
psprite + status bar render fine throughout.

### Diagnosis, peeled through layers (all data-backed; layer-4 pd_column theory KILLED 2026-07-23)
1. **BSP frustum cull** dropped wall subtrees at off-tic viewpoints — WHD_SUPER_TINY compressed
   node bboxes truncate RIGHT/TOP edges ~1 unit INWARD (see the `subbox` decode in
   `R_RenderBSPNode`, r_bsp.c). FIX (real, conservative, NOT committed because it lightly touches
   the default 35fps path for an off feature) — in `R_CheckBBox`, after the local var decls:
   ```c
   #if WHD_SUPER_TINY
       node_coord_t bc[4];
       bc[BOXLEFT]=bspcoord[BOXLEFT]-1; bc[BOXRIGHT]=bspcoord[BOXRIGHT]+1;
       bc[BOXBOTTOM]=bspcoord[BOXBOTTOM]-1; bc[BOXTOP]=bspcoord[BOXTOP]+1;
       bspcoord=bc;
   #endif
   ```
   After this, seg counts are EQUAL raw-vs-interp at the same tic. So culling is fixed but green persists.
2. Walls are ADDED equally (seg count raw==interp) but only ~¼ the COLUMNS are drawn
   (measured 374 vs 1506) → floor floods → green.
3. NOT list_buffer/flat-cache contention: our build is `#if DOOMX` → separate `flat_cache_buf`
   (pd_render.cpp ~3028); `uh_oh_discard_columns` never runs.
4. **pd_column occlusion-merge theory — KILLED (hardware + QEMU confirmed, 2026-07-23).**
   Split `free_pd_column` counters (wall-freed, wall-freed-by-plane) read **exactly 0 on every
   collapse frame**; `free_wall_by_plane` fired only on healthy high-column frames — uncorrelated
   (anti-correlated) with the collapse. QEMU: 1479 contiguous frames, fwbp==0 throughout, no
   collapse. The merge, the column depth-SORT, and `PD_QUANTIZE` scale packing are all CLEARED.
   Also eliminated earlier by vanilla-diff: `R_ScaleFromGlobalAngle` (byte-identical to crispy),
   BSP partition precision (identical int16).

5. **WHAT THE DATA ACTUALLY SHOWS (the real redirection):** the collapse is columns **never
   allocated**, not over-freed. `render_col_count` — an allocation high-water mark that freeing
   does NOT lower — itself halves at collapse (709→377). Plane columns vanish (224→0), wall
   columns drop (~485→377), the floor merges into one dominant visplane → green. **The loss is
   UPSTREAM of the pd_column list** — in the seg→wall-column emit path (`R_StoreWallRange` /
   `R_RenderSegLoop`) and/or visplane generation. NOT a pd_render.cpp problem.

### Layers 5's "never allocated" reading was ALSO wrong — corrected below
`render_col_count` is a slot-REUSE high-water, not a monotone alloc count (`alloc_pd_column`,
pd_render.cpp:367 — freed slots are handed back out). So a low rcc does NOT mean "columns never
requested"; it means columns were freed and their slots recycled. Column PRODUCTION is in fact
near-identical healthy-vs-collapse (segs, cols_visited, wall_ok ~716→719, plane_ok ~731→724,
alloc-fail 0 both). Projection path (yl>yh) is CLEARED by data: cols_ylgtyh identical (~118)
both frames; sample-column pre-clip yl/yh == post-clip, interpolates smoothly (≤2px); max
|worldtop·rw_scale>>16| = 53.7M, ~40× below the 2³¹ FixedMul limit → 0 overflow (crispy's int64
WiggleFix would change nothing). Clip-array collapse also CLEARED (ceilingclip/floorclip pre==post).

### CONFIRMED MECHANISM (2026-07-23) — the pd_column merge DOES cause it; the earlier kill measured the wrong branch
The occlusion merge in `push_down_x_guts` (pd_render.cpp:421–618) has TWO free sites:
- **front** ("new column in front fully obscures existing", :472–475) — this is what the earlier
  session counted. It genuinely reads ~0 on collapse frames. That is why the merge was wrongly cleared.
- **back** ("new column is BEHIND, fully obscured by existing → free it", :551–555) — THIS fires
  ~**1428** times on collapse frames, **0** on healthy frames. Every column inserted at an
  interpolated viewpoint is judged fully hidden behind an already-present column at that x and
  discarded; the ~383 survivors don't cover the view → floor visplane floods → green.

### ROOT CAUSE — CONFIRMED & FIXED (2026-07-23), per-column data in QEMU
The mass free_back is caused by **one degenerate near masked column**, NOT the EPSILON/plane-bias
hypothesis (that, plus `<`→`<=` ties and a plane-can't-free-wall guard, were all REFUTED by the data —
the freed columns are killed by a WALL, not a plane, and it's a genuine full-depth near column, not a
scale tie). Per-x capture on the lt1385 collapse frame: a single **full-height column (yl=0..167) at
EVERY x, constant near scale, fd_num=2** (one masked two-sided line), emitted via **`pd_add_masked_columns`**
(not pd_add_column, not planes, not psprites). With uncapped_fps the linearly-interpolated viewpoint
grazes/enters a masked two-sided line that the collision-resolved player (16-unit radius) never reaches;
`spryscale`/`pd_scale` saturates toward the R_ScaleFromGlobalAngle 64·FRACUNIT clamp (measured 37–64·FRACUNIT
vs normal walls <5·FRACUNIT — clean gap); the seg projects full-height at every x, wins the depth-sort as
"nearest", and frees every column behind it → view collapses to one flat (green).

### THE FIX (applied, uncommitted, rp2040-doom branch perf-investigation, src/pd_render.cpp ~line 993)
```c
extern "C" fixed_t fractionaltic;   // r_main.c
void pd_add_masked_columns(...) {
    if (fractionaltic && pd_scale >= 16 * FRACUNIT) return;  // drop degenerate near masked column
    ...
```
Validated in QEMU (instrumentation since removed): collapse-frame free_back 1440→0; mid-frac rcc avg
1483 ≈ frac0 avg 1505 (was ~383); zero collapse-signature frames across a full-demo soak; 35fps path
byte-identical (fractionaltic==0 on every real tic); no weapon/masked regression (pspritescale~1·FRACUNIT,
legit masked <16·FRACUNIT — below the gate); max rcc ~2776 < RENDER_COL_MAX.

### STATUS 2026-07-23 (later): green collapse HW-VALIDATED FIXED; second bug found + fix applied
- GREEN COLLAPSE: user confirmed on hardware (uncapped_fps=true) the green flash is GONE. The
  pd_render.cpp masked-column guard is the validated fix. ✔
- SECOND BUG — "movement happens twice" (jitter): the interpolation guard in R_SetupFrame
  (r_main.c ~1196) gated on `fractionaltic != 0`. R_UpdateFractionalTic forces fractionaltic=0 on
  the frame a tic commits, so THAT frame took the else branch → snapped to the new (fully-advanced)
  position; the next present interpolated from old again → every tic's motion drew twice (snap to
  new, jump back to old, slide to new). FIX APPLIED (uncommitted): removed the `fractionaltic &&`
  from the guard so frac==0 lerps to `old` (continuous with previous frame's new), matching Crispy
  (when uncapped + in-level, ALWAYS interpolate). Rebuilt + flashed to bank1, awaiting HW confirm.

### TEMP build state (must revert before commit)
- rp2040-doom r_main.c:1135 `uncapped_fps = true` — TEMP for HW validation; revert to false.
- The two real fixes to keep: pd_render.cpp masked-column guard + r_main.c R_SetupFrame guard change.
- Validation build: `make flash-doom start-app WAD=doom1-shareware.wad INTFLASH_BANK=1` (firmware
  unchanged — no firmware-symbol-coupling reflash needed; only doom.bin changed, doom.whd unchanged).

### REMAINING after jitter validated
Commit the two fixes (revert the uncapped default to false first, OR gate uncapped behind the planned
menu). Then build the 35/60/75 selectable-refresh Video submenu (original 60fps-mode goal) + runtime
uncapped toggle. Original untouched goals still at ZERO: keybind fix, CI GWHB binary, web builder.

### The probe harness (throwaway scaffolding — re-add as needed)
A per-frame ring in `.bss` drained over gdb(QEMU :1234) or SWD. Fields per frame keyed by
`leveltime` (tic): fractionaltic, green% (dominance of top palette index over the 3D-view region —
sample `line[]` for `scanline<MAIN_VIEWHEIGHT` in `compose_frame`), seg count (increment a global
in `R_AddLine`), `render_col_count` (un-static it, pd_render.cpp:362), plus the candidate counter.
KEEP THE RING SMALL (~32 entries) — the AXI zone heap is TIGHT (ZONE_MIN link error if too big).
KEY ANALYSIS: per leveltime compare frac<3000 (raw, ≈tic view) vs frac>28000 (interpolated) frames
of the SAME tic. `uncapped_fps=true` is a TEMP test edit (keep committed default false).

## HARD-WON OPERATIONAL GOTCHAS
- **QEMU discipline**: ONE headless (`-display none`) instance, wrapped in `timeout N ...` as a
  SINGLE foreground command that launches (NO `-S`, auto-runs full speed), boots (~12s), polls the
  ring via `arm-none-eabi-gdb -batch -ex "target remote :1234" -ex "dump binary memory ..."`, then
  `pkill -x qemu-system-arm`. NEVER `-display gwemu`/GUI detached, NEVER setsid a GUI, NEVER open
  multiples, NEVER pkill while the user watches a window. A prior run froze the user's WM. QEMU is
  FUNCTIONAL-ONLY (TCG models no cycle timing/cache/bus — no valid perf numbers; instruction counts
  also invalid). `make qemu` / `scripts/debug/run_qemu.sh` exist; extflash packer is
  `scripts/build/make_extflash_image.py`. Firmware at 0x08000000, extflash at 0x90000000.
- **DISK**: a full disk (another session's Docker work on qemu-gnw transiently filled it to 0 bytes)
  causes QEMU to hang/die and mimics fork instability. `df -h /` before long runs. It recovered to
  225G free; watch it.
- **Stale objects**: `build/<variant>[-trace]/` is keyed on variant+TRACE only, NOT on other -D
  flags (TRACE_NUM_SLOTS, PATCH_CACHE_BYTES, EXTRA_DEFS, INTFLASH_BANK). `rm -rf build/<dir>` when
  changing any define, or you silently reuse stale objects (this bit us 3+ times, incl. a VTOR
  bank mismatch and a wrong patch-cache size).
- **WHD/binary coupling**: `whddata.h` changes shift WILV/CWILV indices — the WHD and binary must be
  reflashed TOGETHER or the intermission screen mis-draws (silently).
- **Firmware/doom symbol coupling**: rebuild+reflash firmware AND doom together after firmware changes.
- **Agents mis-report git state and "user stopped me"**: verify `git status` and device state
  yourself after any agent run. Several agents claimed committed/clean when they weren't.
- **EXTRA_DEFS** Makefile hook exists for ad-hoc A/B defines (e.g. `-DGNW_NOINTERP` to force raw view).
- Flash: `gnwmanager flash bank1 --file build/firmware.bin`; `make flash-doom WAD=...`; `make start-app`.
  SWD probe = ST-Link/V2 (0483:3748). If it drops off USB, physical replug needed — don't thrash it.

## REMAINING QUEUE (the user's ORIGINAL 3 goals, still at ZERO)
1. **Keybind fix**: swap GAME↔PAUSE and align to how celeste/smw/other cores map buttons.
   `src/gnw/i_input_gnw.c:102-104` has the VOLUME↔START swap to reconsider. Real retro-go convention
   (common.c): PAUSE/SET=ODROID_INPUT_VOLUME=firmware modifier, GAME=_START, TIME=_SELECT.
2. **CI-built GWHB binary** of shareware doom (named `DOOM.bin` per retro-go display-name behavior)
   so anyone can drop it in `/roms/homebrew/`. Ships `doom.bin` + `doom.whd` (2 files).
3. **GitHub Pages web builder** (à la `/home/doug/Nerd/git/gnw-web-builder`): take a wad, sha1-classify
   (`scripts/build/wad_plan.py`), emit the right binary. Design in more detail when reached.
Plus: fixed-address `.bss` block for ELF-free SWD reads (do after the CLUT/green work).

## Memory-layout facts (for any future RAM work)
ITCM 93.9% full (~3.9K free) — hot code incl. the OPL core already there. AHB app window 90.8%.
AXI .text_axis 92.3%. DTCM ~85-105K FREE but the toolkit ABI's `dtcm_malloc` is now populated
(was NULL). GWHB image is a RAM overlay at 0x2404B000. `itc_malloc`/`ram_malloc`/`lcd_get_bonus_pool`
were NULL in the toolkit historically — check before relying on any allocator.
