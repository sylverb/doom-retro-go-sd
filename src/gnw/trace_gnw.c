//
// Pipeline tracer buffer + frame-priority retention. See trace_gnw.h.
//
#include "trace_gnw.h"

#if DOOMX_TRACE

// The slot pool lives in leftover LUT8 LCD bonus after the patch cache
// (see I_InitGraphics / lcd_get_bonus_pool). There is no separate scratch
// frame: the in-progress frame writes straight into trace_stage, and the
// boundary decides whether to commit (advance to a fresh staging slot) or
// discard (reset the same slot and reuse it for the next frame).
trace_slot_t *trace_slots;
uint32_t trace_pool_len = sizeof(trace_slot_t) * TRACE_NUM_SLOTS;
volatile uint32_t trace_stage;          // slot the current frame fills

// Free-running aggregates (see trace_gnw.h). Deliberately NOT in .trace_buf:
// they are tiny and a host tool zeroes them directly to open a window.
uint64_t trace_acc_cyc[TEV_COUNT];
uint32_t trace_acc_n[TEV_COUNT];
uint64_t trace_acc_frame_cyc;
uint32_t trace_acc_frames;

uint32_t trace_busy_hist[TRACE_HIST_BUCKETS];
uint32_t trace_busy_max_cyc;
uint32_t trace_busy_max_frame;
uint32_t trace_frame_idle_cyc;

uint32_t trace_play_hist[TRACE_HIST_BUCKETS];
uint32_t trace_play_max_cyc;
uint32_t trace_play_max_frame;
uint32_t trace_play_frames;
uint32_t trace_load_frames;
uint32_t trace_play_skip_after_load;     // host-settable; 0 = loads only
uint32_t trace_postload_sum_us[TRACE_POSTLOAD_N];
uint32_t trace_postload_n[TRACE_POSTLOAD_N];
uint32_t trace_postload_max_us[TRACE_POSTLOAD_N];
uint32_t trace_frame_had_load;
uint32_t trace_all_over18, trace_all_dropped;
uint32_t trace_play_over18, trace_play_dropped;
uint32_t trace_play_top_us[TRACE_TOPN];

// Column pixel accounting (see trace_gnw.h). Free-running like trace_acc_*;
// a host tool zeroes them to open a measurement window.
uint64_t trace_pix[2];
uint32_t trace_ncol[2];
uint32_t trace_colh_max[2];
uint32_t trace_col_class;

// Insert into the descending top-N list. Runs once per frame and exits
// immediately for the common case (frame slower than nothing in the list).
static void top_insert(uint32_t v)
{
    if (v <= trace_play_top_us[TRACE_TOPN - 1u]) return;
    uint32_t i = TRACE_TOPN - 1u;
    while (i > 0u && trace_play_top_us[i - 1u] < v) {
        trace_play_top_us[i] = trace_play_top_us[i - 1u];
        i--;
    }
    trace_play_top_us[i] = v;
}

// Distance in frames since the last LOAD frame: 0 = not tracking, 1 = the
// frame immediately after a load, etc. Stops counting past the measurement
// window so it cannot run away.
static uint32_t s_post_load;

static uint32_t s_frame_no;             // sequential frame counter
static uint32_t s_stage_start;          // CYCCNT at the staging frame's open
static uint32_t s_filled;               // committed slots so far (<= NUM_SLOTS)

// Pick the next staging slot: a still-empty one while filling, else the
// fastest KEPT slot (smallest dur_cyc) — the one we are willing to overwrite
// next, and only when the incoming frame proves slower.
static uint32_t pick_victim(void)
{
    if (s_filled < TRACE_NUM_SLOTS)
        return s_filled;                // next empty slot
    uint32_t best = 0;
    for (uint32_t i = 1; i < TRACE_NUM_SLOTS; i++)
        if (trace_slots[i].dur_cyc < trace_slots[best].dur_cyc)
            best = i;
    return best;
}

void trace_frame_boundary(uint32_t cyc)
{
    trace_slot_t *cur = &trace_slots[trace_stage];

    // First boundary just opens the first frame.
    if (s_stage_start == 0 && cur->count == 0) {
        cur->frame_no = s_frame_no;
        s_stage_start = cyc ? cyc : 1u;
        return;
    }

    // Score the frame that just ended.
    cur->dur_cyc = cyc - s_stage_start;

    // Aggregate wall time. Summing per-frame deltas (each far below the ~15s
    // CYCCNT wrap at 280 MHz) avoids the wrap entirely, unlike a single
    // start/end span over a long window.
    trace_acc_frame_cyc += cur->dur_cyc;
    trace_acc_frames++;

    // Busy = wall duration minus idle spent inside this frame. Guard the
    // subtraction: a frame can close with an IDLE span still open (the
    // boundary fires from inside the pacer), which would otherwise underflow.
    // Computed ONCE here: both the histogram and the keep-gate below need it,
    // and trace_frame_idle_cyc is reset at the end of this function.
    uint32_t idle = trace_frame_idle_cyc;
    uint32_t busy = (cur->dur_cyc > idle) ? (cur->dur_cyc - idle) : 0u;
    // 280 cycles per us at 280 MHz; runs once per frame, so a divide is fine.
    uint32_t busy_us = busy / 280u;
    uint32_t bucket = busy_us / TRACE_HIST_BUCKET_US;
    if (bucket >= TRACE_HIST_BUCKETS) bucket = TRACE_HIST_BUCKETS - 1u;
    {
        trace_busy_hist[bucket]++;
        if (busy > trace_busy_max_cyc) {
            trace_busy_max_cyc  = busy;
            trace_busy_max_frame = cur->frame_no;
        }
        if (busy_us >= TRACE_TARGET_US)       trace_all_over18++;
        if (busy_us >= TRACE_FRAME_BUDGET_US) trace_all_dropped++;
    }
    trace_frame_idle_cyc = 0;

    // ---- Gameplay-only classification ----------------------------------
    // A load frame is any frame a LOAD span touched. trace_frame_had_load is
    // set on both LOAD_BEG and LOAD_END; a load spanning a whole frame with
    // neither end inside it cannot happen here, because no FRAME_MARK fires
    // while P_SetupLevel runs (D_DoomLoop is not iterating), so the load is
    // always bounded by the frame that contains it.
    int is_load = (trace_frame_had_load != 0);
    {
        trace_frame_had_load = 0;

        if (is_load) {
            trace_load_frames++;
            s_post_load = 1;               // next frame sits at distance 1
        } else {
            // Measure the post-load spillover instead of assuming it.
            if (s_post_load && s_post_load <= TRACE_POSTLOAD_N) {
                uint32_t i = s_post_load - 1u;
                trace_postload_sum_us[i] += busy_us;
                trace_postload_n[i]++;
                if (busy_us > trace_postload_max_us[i])
                    trace_postload_max_us[i] = busy_us;
            }

            if (s_post_load == 0 || s_post_load > trace_play_skip_after_load) {
                trace_play_hist[bucket]++;
                trace_play_frames++;
                if (busy > trace_play_max_cyc) {
                    trace_play_max_cyc   = busy;
                    trace_play_max_frame = cur->frame_no;
                }
                if (busy_us >= TRACE_TARGET_US)       trace_play_over18++;
                if (busy_us >= TRACE_FRAME_BUDGET_US) trace_play_dropped++;
                top_insert(busy_us);
            } else {
                trace_load_frames++;       // excluded as post-load warm-up
            }

            if (s_post_load) {
                s_post_load++;
                if (s_post_load > TRACE_POSTLOAD_N) s_post_load = 0;
            }
        }
    }

    int keep;
    // Threshold gate: a frame already inside budget is not a problem frame, so
    // never spend a slot on one. Without this the pool fills with whatever ran
    // first and then only ever swaps for the single worst outlier (a level
    // load), so the frames we actually want to study -- the ones just over
    // budget -- are never captured. With it, every slot holds a real offender.
    // 0 disables the gate.
    if (is_load) {
        keep = 0;
    } else if (TRACE_KEEP_MIN_BUSY_US && busy_us < TRACE_KEEP_MIN_BUSY_US) {
        keep = 0;
    } else if (s_filled < TRACE_NUM_SLOTS) {
        keep = 1;                        // still filling: keep everything
    } else {
        // Full: keep only if slower than the fastest frame we currently hold
        // (i.e. it belongs among the worst). Equal-or-faster frames drop.
        uint32_t fastest = trace_slots[0].dur_cyc;
        for (uint32_t i = 1; i < TRACE_NUM_SLOTS; i++)
            if (trace_slots[i].dur_cyc < fastest) fastest = trace_slots[i].dur_cyc;
        keep = cur->dur_cyc > fastest;
    }

    uint32_t next;
    if (keep) {
        if (s_filled < TRACE_NUM_SLOTS) s_filled++;
        next = pick_victim();            // empty slot, or the fastest to evict
        if (next == trace_stage)         // never evict the frame we just kept
            next = (next + 1u) % TRACE_NUM_SLOTS;
    } else {
        next = trace_stage;              // discard: reuse this slot
    }

    trace_slot_t *ns = &trace_slots[next];
    ns->count = 0;
    ns->truncated = 0;
    ns->dur_cyc = 0;
    ns->frame_no = ++s_frame_no;
    trace_stage = next;
    s_stage_start = cyc ? cyc : 1u;
}

void trace_place(void *pool)
{
    trace_slots = (trace_slot_t *)pool;
}

void trace_init(void)
{
    // Enable DWT cycle counter: DEMCR.TRCENA then DWT_CTRL.CYCCNTENA.
    *(volatile uint32_t *)0xE000EDFC |= (1u << 24);
    *(volatile uint32_t *)0xE0001000 |= 1u;

    if (!trace_slots)
        return;

    for (uint32_t i = 0; i < TRACE_NUM_SLOTS; i++) {
        trace_slots[i].count = 0;
        trace_slots[i].truncated = 0;
        trace_slots[i].dur_cyc = 0;
        trace_slots[i].frame_no = 0;
    }
    trace_stage = 0;
    s_frame_no = 0;
    s_stage_start = 0;
    s_filled = 0;
    s_post_load = 0;
    trace_frame_had_load = 0;
    TRACE_EVT(TEV_FRAME_MARK, 0);        // opens the first frame
}

#endif
