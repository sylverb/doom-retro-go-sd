//
// Pipeline tracer (build with TRACE=1): every render/audio stage logs
// begin/end events with the DWT cycle counter (280 MHz, 3.57 ns). Compiled
// out entirely otherwise.
//
// PRIORITY RETENTION. A flat ring would fill with whatever ran most recently —
// mostly fast, uninteresting frames. Instead events accumulate per-frame into
// a small pool of slots; at each frame boundary the just-finished frame is
// KEPT only if it is slow enough to matter (slower than the fastest frame the
// pool currently holds, once the pool is full). So the pool converges to the N
// WORST frames seen across the whole session — exactly the ones worth reading —
// in a fraction of the RAM a ring would need. scripts/debug/tracepull.py drains
// it over SWD and writes the per-frame timing report.
//
#ifndef DOOMX_TRACE_GNW_H
#define DOOMX_TRACE_GNW_H

#include <stdint.h>

// Event IDs. *_BEG/*_END pairs nest; MARKs are points with an argument.
enum {
    TEV_NONE = 0,
    TEV_FRAME_MARK,      // arg = frame number low 16 (present boundary)
    TEV_TICS_BEG, TEV_TICS_END,             // TryRunTics (game logic + wait)
    TEV_GTIC_BEG, TEV_GTIC_END,             // one G_Ticker (pure game logic)
    TEV_RENDER_BEG, TEV_RENDER_END,         // D_Display whole render
    TEV_BSP_BEG, TEV_BSP_END,               // R_RenderPlayerView BSP+segs walk
    TEV_FLATS_BEG, TEV_FLATS_END,           // draw_visplanes total
    TEV_FLATDEC_BEG, TEV_FLATDEC_END,       // one flat decode, arg=picnum
    TEV_PATCHDEC_BEG, TEV_PATCHDEC_END,     // one patch decoder fetch, arg=patch
    TEV_REGCOLS_BEG, TEV_REGCOLS_END,       // draw_regular_columns (walls/sprites)
    TEV_FUZZ_BEG, TEV_FUZZ_END,             // fuzz columns
    TEV_OVERLAY_BEG, TEV_OVERLAY_END,       // ST/HU/menu drawers (list build)
    TEV_COMPOSE_BEG, TEV_COMPOSE_END,       // i_video compose to LTDC fb
    TEV_MIX_BEG, TEV_MIX_END,               // SAI mixer batch, arg=samples
    TEV_OPL_BEG, TEV_OPL_END,               // emu8950 chunk render, arg=samples
    TEV_IDLE_BEG, TEV_IDLE_END,             // pacer/tic waits (true idle)
    TEV_WIPE_MARK,                          // wipe frame presented
    TEV_CMP_BASE,                           // compose: base scanlines, arg=us
    TEV_CMP_OVERLAY,                        // compose: vpatch overlays, arg=us
    TEV_CMP_OUT,                            // compose: LTDC row writes, arg=us
    TEV_LOAD_BEG, TEV_LOAD_END,             // P_SetupLevel (the wipe-hitch span)
    TEV_COUNT
};

#if DOOMX_TRACE

typedef struct {
    uint32_t cyc;       // DWT->CYCCNT at event
    uint16_t ev;
    uint16_t arg;
} trace_entry_t;

// Pool geometry (override with -D). NUM_SLOTS frames retained, each up to
// SLOT_EVENTS events. Total = NUM_SLOTS * (16 + SLOT_EVENTS*8) bytes.
//   default 8 * (16 + 2048*8) = ~128 KB — the 8 worst frames at full detail.
// A frame emitting more than SLOT_EVENTS events is truncated (flagged).
#ifndef TRACE_NUM_SLOTS
#define TRACE_NUM_SLOTS  8u
#endif
#ifndef TRACE_SLOT_EVENTS
#define TRACE_SLOT_EVENTS 2048u
#endif

/* Only retain frames whose BUSY time reaches this many microseconds; frames
 * inside budget never occupy a slot. 0 = keep whatever is worst (old
 * behaviour). Set it to the frametime target you are chasing. */
#ifndef TRACE_KEEP_MIN_BUSY_US
#define TRACE_KEEP_MIN_BUSY_US 0u
#endif

typedef struct {
    uint32_t frame_no;   // sequential id of the frame this slot holds
    uint32_t dur_cyc;    // frame wall time (FRAME_MARK..next FRAME_MARK)
    uint32_t count;      // events stored (<= TRACE_SLOT_EVENTS)
    uint32_t truncated;  // events dropped past SLOT_EVENTS
    trace_entry_t ev[TRACE_SLOT_EVENTS];
} trace_slot_t;

// The slot pool + the index of the slot the in-progress frame writes into.
// trace_slots is a pointer into leftover LCD bonus (see trace_place).
extern trace_slot_t *trace_slots;
extern uint32_t trace_pool_len;
extern volatile uint32_t trace_stage;   // staging slot index

void trace_place(void *pool);
void trace_init(void);

/* ---- Free-running aggregate counters (A/B measurement) ------------------
 * The slot pool retains the N WORST frames, which is right for finding
 * pathological hitches but WRONG for comparing two builds: worst frames are
 * atypical by construction and vary wildly in content, so the same stage can
 * read 5ms or 15ms depending on what the demo happened to be doing.
 *
 * These counters instead sum EVERY occurrence over the whole session, so
 * "total REGCOLS cycles / frames" is a stable mean. Never reset by the
 * firmware — a host tool zeroes them to open a measurement window.
 * Indexed by the BEG event id; 64-bit because CYCCNT wraps every ~15s at
 * 280 MHz and windows are longer than that. */
extern uint64_t trace_acc_cyc[TEV_COUNT];   /* summed BEG..END, per stage */
extern uint32_t trace_acc_n[TEV_COUNT];     /* occurrences, per stage     */
extern uint64_t trace_acc_frame_cyc;        /* summed frame durations     */
extern uint32_t trace_acc_frames;           /* frames closed              */

/* ---- Busy-time histogram ------------------------------------------------
 * The goal is "no frame ever exceeds N ms of WORK", which neither the worst-3
 * slot pool (too few samples, and dominated by level loads) nor the mean can
 * answer. What matters is BUSY time, not the frame's wall duration: we are
 * locked at 35 fps, so every frame's wall time is ~28.5 ms regardless of how
 * much work it did — the pacer just idles longer. Busy = frame duration minus
 * the idle spans inside it.
 *
 * 2 ms buckets to 32 ms, then one overflow bucket. Cheap enough for the hot
 * path (one subtract, one shift, one increment per frame). */
#define TRACE_HIST_BUCKET_US 2000u
#define TRACE_HIST_BUCKETS   17u          /* 0-2,2-4,...,30-32,>=32 ms */
extern uint32_t trace_busy_hist[TRACE_HIST_BUCKETS];
extern uint32_t trace_busy_max_cyc;        /* worst busy frame seen      */
extern uint32_t trace_busy_max_frame;      /* its frame number           */
extern uint32_t trace_frame_idle_cyc;      /* idle within the open frame */

/* ---- Gameplay-only histogram -------------------------------------------
 * The all-frames histogram above is dominated by P_SetupLevel: a level load
 * is one enormous "frame" (no FRAME_MARK fires while D_DoomLoop is not
 * iterating), and the attract demo reloads E1M1 constantly. Those frames are
 * not gameplay and are explicitly out of scope, so they drown the number we
 * actually care about.
 *
 * A frame is a LOAD frame if a TEV_LOAD span begins, ends, or is still open
 * inside it. trace_play_* accumulate only over the rest. */
extern uint32_t trace_play_hist[TRACE_HIST_BUCKETS];
extern uint32_t trace_play_max_cyc;        /* worst gameplay busy frame  */
extern uint32_t trace_play_max_frame;
extern uint32_t trace_play_frames;         /* frames counted as gameplay */
extern uint32_t trace_load_frames;         /* frames excluded (load + skip) */

/* Frames immediately after a load may still be paying for cold caches. Rather
 * than guess how many to drop, the spillover is MEASURED per distance (1..N
 * frames after a load) below; trace_play_skip_after_load is then set by the
 * host to whatever the data justifies. 0 = exclude load frames only. */
extern uint32_t trace_play_skip_after_load;

#define TRACE_POSTLOAD_N 8u
extern uint32_t trace_postload_sum_us[TRACE_POSTLOAD_N];
extern uint32_t trace_postload_n[TRACE_POSTLOAD_N];
extern uint32_t trace_postload_max_us[TRACE_POSTLOAD_N];

/* Set when a LOAD span touches the frame currently being accumulated. */
extern uint32_t trace_frame_had_load;

/* ---- Exact tail counters ------------------------------------------------
 * 2 ms histogram buckets cannot resolve an 18 ms cliff, and a 99.99% claim
 * needs exact counts rather than bucket edges. Two thresholds matter:
 *
 *   18000 us  - the headroom target we are chasing.
 *   28571 us  - the actual frame budget at the 35 fps lock. Only a frame
 *               exceeding THIS drops a frame and is visible as a hitch;
 *               18 ms is a safety margin, not a smoothness threshold.
 */
#define TRACE_FRAME_BUDGET_US 28571u
#define TRACE_TARGET_US       18000u
extern uint32_t trace_all_over18,  trace_all_dropped;
extern uint32_t trace_play_over18, trace_play_dropped;

/* Worst N gameplay frames, busy microseconds, descending. Answers "how far
 * over, and how often" without needing finer buckets everywhere. */
#define TRACE_TOPN 16u
extern uint32_t trace_play_top_us[TRACE_TOPN];

/* ---- Column pixel accounting -------------------------------------------
 * REGCOLS is otherwise a black box: it reports total time but not whether the
 * cost is MORE columns or TALLER ones. render_col_count answers the first;
 * these answer the second, which is the only remaining unmeasured input.
 *
 * Every column funnels through col_render(), so one add there catches all of
 * them. Deliberately counters, NOT trace events: a TRACE_EVT pair per column
 * would be ~2000 events and tens of microseconds per frame on a 15 ms
 * measurement, i.e. the instrumentation would become the finding. An add plus
 * a compare is ~0.2% of the frame and does not move the number.
 *
 * Split wall (composite texture) vs sprite (patch) because they are different
 * code paths with different fixes. The class is set once per drawable loop,
 * not per column.
 *
 * Overdraw = pix / (SCREENWIDTH * viewheight): 1.0 means every shaded pixel
 * reached the screen, 6.0 means five sixths of the work was painted over. */
#define TRACE_COL_WALL   0u
#define TRACE_COL_SPRITE 1u
extern uint64_t trace_pix[2];        /* pixels shaded, per class  */
extern uint32_t trace_ncol[2];       /* columns drawn, per class  */
extern uint32_t trace_colh_max[2];   /* tallest column seen       */
extern uint32_t trace_col_class;     /* which class is drawing now */

/* BEG/END pairs occupy TICS_BEG..IDLE_END and LOAD_BEG..LOAD_END, even=BEG.
 * The marks in between (WIPE/CMP_*) are points, not spans, and must not be
 * treated as pairs. */
static inline int trace_is_pair(uint16_t ev)
{
    return (ev >= TEV_TICS_BEG && ev <= TEV_IDLE_END) || ev >= TEV_LOAD_BEG;
}
// Called at each present boundary (FRAME_MARK): score the finished frame and
// either keep it (evicting the fastest kept frame when full) or discard it.
void trace_frame_boundary(uint32_t cyc);

static inline void trace_emit(uint16_t ev, uint16_t arg)
{
    uint32_t c = *(volatile uint32_t *)0xE0001004;   // DWT->CYCCNT (read first)

    // Aggregate accumulation (see trace_acc_* above). Kept before the
    // staging logic so it is unaffected by keep/discard decisions — every
    // occurrence counts, not just the ones in retained frames.
    if (trace_is_pair(ev)) {
        static uint32_t open_cyc[TEV_COUNT];
        if ((ev & 1u) == 0u) {                       // even -> BEG
            open_cyc[ev] = c;
            // Mark on BEG as well as END: a load is long enough that its two
            // ends can land in different frames, and both are load frames.
            if (ev == TEV_LOAD_BEG) trace_frame_had_load = 1;
        } else {                                     // odd  -> END
            uint16_t beg = (uint16_t)(ev - 1u);
            if (open_cyc[beg]) {                     // ignore an END with no BEG
                uint32_t d = (uint32_t)(c - open_cyc[beg]);
                trace_acc_cyc[beg] += d;
                trace_acc_n[beg]++;
                if (beg == TEV_IDLE_BEG)
                    trace_frame_idle_cyc += d;       // subtracted at the boundary
                if (beg == TEV_LOAD_BEG) trace_frame_had_load = 1;
                open_cyc[beg] = 0;
            }
        }
    }

    if (ev == TEV_FRAME_MARK)
        trace_frame_boundary(c);                     // may re-stage
    trace_slot_t *s = &trace_slots[trace_stage];
    uint32_t n = s->count;
    if (n < TRACE_SLOT_EVENTS) {
        s->ev[n].cyc = c;
        s->ev[n].ev  = ev;
        s->ev[n].arg = arg;
        s->count = n + 1;
    } else {
        s->truncated++;
    }
}

#define TRACE_EVT(ev, arg) trace_emit((ev), (uint16_t)(arg))

/* Set once per drawable loop in draw_regular_columns, not per column. */
#define TRACE_COL_CLASS(c) (trace_col_class = (c))

/* One add + one compare per column. `n` is col_render's pixel count. */
#define TRACE_COL_PIX(n) do {                                   \
        uint32_t _h = (uint32_t)(n);                            \
        uint32_t _c = trace_col_class;                          \
        trace_pix[_c] += _h;                                    \
        trace_ncol[_c]++;                                       \
        if (_h > trace_colh_max[_c]) trace_colh_max[_c] = _h;   \
    } while (0)
#else
#define TRACE_EVT(ev, arg) ((void)0)
#define TRACE_COL_CLASS(c) ((void)0)
#define TRACE_COL_PIX(n)   ((void)0)
static inline void trace_init(void) {}
#endif

// Level-load audio pump (functional, not tracing): P_SetupLevel can run long
// enough to drain the ~85 ms SAI buffer, so the DMA loops a stale tone (the
// "carried tone" hitch). Pump the mixer between load stages; each stage is well
// under the buffer. No-op on hosts/upstream (see tracehooks.h). Defined for ALL
// DOOMX builds, not just TRACE — these are not instrumentation.
#ifndef __cplusplus
void I_UpdateSound(void);   // C TUs only; pd_render.cpp has its own extern "C" decl
void pd_warm_flat_cache(void);
void pd_warm_sprite_cache(void);
#endif
#define LOAD_PUMP() I_UpdateSound()

// Warm the flat + sprite decode caches during the level-load pause instead of
// storming decodes over the first rendered frames (both pump the mixer too).
#define LOAD_WARM() do { pd_warm_flat_cache(); pd_warm_sprite_cache(); } while (0)

#endif
