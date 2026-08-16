//
// oplverify — prove that a change to the emu8950 OPL core is BIT-EXACT.
//
// WHY THIS EXISTS. The OPL renderer is the single largest CPU cost in the
// port, so it attracts optimisation, and "it still sounds like music" is not
// verification -- an envelope that decays one sample early, or a phase that
// resumes at the wrong point after a re-key, is inaudible in casual listening
// and still wrong. This harness drives the core through a deterministic
// register sequence and dumps every rendered sample, so two builds can be
// byte-compared.
//
// It has already earned its keep: it caught an END_OF_NOTE early-out that
// looked free (once eg_out hits EG_MAX the slot emits silence, so stopping
// early "cannot" matter) but was not -- stopping early freezes pg_phase, and
// key-on does NOT reset phase, so the next note resumed at the wrong point.
// 22,919 of 102,400 samples differed. Nothing short of a byte-compare would
// have found that.
//
// USAGE: see run.sh, which builds this against a reference commit and against
// the working tree and compares the two dumps.
//
// The sequence deliberately exercises the paths where OPL bugs hide:
//   - all 9 melodic channels, both algorithms, feedback on and off
//   - AM and PM (vibrato/tremolo) both enabled and disabled
//   - all four waveforms
//   - key-on -> sustain -> key-off -> release -> RE-KEY, because phase
//     continuity across a re-key is exactly what naive early-outs break
//   - a spread of attack/decay/sustain/release rates so envelope state
//     transitions land at varied points within a buffer, not just at edges
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "emu8950.h"

#define NSAMPLES   256      /* matches the device's OPL chunk size */
#define NBUFFERS   400
#define OPL_CLK    3579552u
#define OPL_RATE   (OPL_CLK / 72u)

/* Deterministic PRNG: rand() differs across libc versions, which would make
 * the two builds diverge for reasons that have nothing to do with the code
 * under test. */
static uint32_t rng_state = 0x13579bdfu;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void program_channel(OPL *opl, int ch, uint32_t seed)
{
    /* Operator register offsets for melodic channel `ch`. */
    static const uint8_t op_off[9][2] = {
        {0x00, 0x03}, {0x01, 0x04}, {0x02, 0x05},
        {0x08, 0x0b}, {0x09, 0x0c}, {0x0a, 0x0d},
        {0x10, 0x13}, {0x11, 0x14}, {0x12, 0x15},
    };

    for (int op = 0; op < 2; op++) {
        uint8_t o = op_off[ch][op];
        uint32_t r = seed + op * 7919u;

        /* 0x20: AM | VIB | EG-type | KSR | multiple */
        uint8_t am   = (r & 1) ? 0x80 : 0;
        uint8_t vib  = (r & 2) ? 0x40 : 0;
        uint8_t egt  = (r & 4) ? 0x20 : 0;
        uint8_t ksr  = (r & 8) ? 0x10 : 0;
        uint8_t mult = (uint8_t)((r >> 4) & 0x0f);
        OPL_writeReg(opl, 0x20 + o, am | vib | egt | ksr | mult);

        /* 0x40: key-scale-level | total-level. Keep TL lowish so output is
         * loud enough that differences are not hidden under the noise floor. */
        uint8_t ksl = (uint8_t)(((r >> 8) & 3) << 6);
        uint8_t tl  = (uint8_t)((r >> 10) & 0x1f);
        OPL_writeReg(opl, 0x40 + o, ksl | tl);

        /* 0x60: attack | decay -- avoid 0 (infinitely slow) so envelopes
         * actually move within the captured window. */
        uint8_t ar = (uint8_t)(((r >> 12) & 0x0f) | 4);
        uint8_t dr = (uint8_t)(((r >> 16) & 0x0f) | 2);
        OPL_writeReg(opl, 0x60 + o, (uint8_t)((ar << 4) | dr));

        /* 0x80: sustain | release */
        uint8_t sl = (uint8_t)((r >> 20) & 0x0f);
        uint8_t rr = (uint8_t)(((r >> 24) & 0x0f) | 3);
        OPL_writeReg(opl, 0x80 + o, (uint8_t)((sl << 4) | rr));

        /* 0xe0: waveform select (all four) */
        OPL_writeReg(opl, 0xe0 + o, (uint8_t)((r >> 5) & 3));
    }

    /* 0xc0: feedback | algorithm -- exercise FB==0 and FB!=0, alg 0 and 1,
     * since the renderer specialises on both. */
    uint8_t fb  = (uint8_t)(((seed >> 3) & 7) << 1);
    uint8_t alg = (uint8_t)((seed >> 6) & 1);
    OPL_writeReg(opl, 0xc0 + ch, (uint8_t)(fb | alg));
}

static void set_note(OPL *opl, int ch, uint32_t fnum, uint32_t block, int keyon)
{
    OPL_writeReg(opl, 0xa0 + ch, (uint8_t)(fnum & 0xff));
    OPL_writeReg(opl, 0xb0 + ch,
                 (uint8_t)((keyon ? 0x20 : 0) | ((block & 7) << 2) | ((fnum >> 8) & 3)));
}

int main(int argc, char **argv)
{
    const char *out_path = (argc > 1) ? argv[1] : "opl_dump.bin";
    FILE *out = fopen(out_path, "wb");
    if (!out) { perror("fopen"); return 1; }

    OPL *opl = OPL_new(OPL_CLK, OPL_RATE);
    if (!opl) { fprintf(stderr, "OPL_new failed\n"); return 1; }
    OPL_reset(opl);

    /* Global: enable wave-select, and exercise both AM/PM depth settings. */
    OPL_writeReg(opl, 0x01, 0x20);
    OPL_writeReg(opl, 0xbd, 0xc0);   /* AM depth + VIB depth */

    for (int ch = 0; ch < 9; ch++)
        program_channel(opl, ch, rnd());

    static int32_t buf[NSAMPLES];
    uint64_t nonzero = 0, total = 0;
    int32_t peak = 0;

    for (int b = 0; b < NBUFFERS; b++) {
        /* Phase the note events so envelope transitions land at varied
         * offsets inside a buffer rather than always on a boundary. */
        for (int ch = 0; ch < 9; ch++) {
            int period = 40 + ch * 7;
            int phase  = (b + ch * 5) % period;

            if (phase == 0) {
                set_note(opl, ch, 0x200 + (rnd() & 0x1ff), rnd() & 7, 1);
            } else if (phase == period / 3) {
                set_note(opl, ch, 0x200 + (rnd() & 0x1ff), rnd() & 7, 0);  /* key off -> release */
            } else if (phase == (2 * period) / 3) {
                /* RE-KEY without reprogramming: this is the case that catches
                 * phase-continuity bugs. */
                set_note(opl, ch, 0x200 + (rnd() & 0x1ff), rnd() & 7, 1);
            }

            /* Occasionally reprogram a whole channel mid-flight. */
            if (((b * 9 + ch) % 137) == 0)
                program_channel(opl, ch, rnd());
        }

        memset(buf, 0, sizeof buf);
        OPL_calc_buffer_linear(opl, buf, NSAMPLES);

        if (fwrite(buf, sizeof buf[0], NSAMPLES, out) != NSAMPLES) {
            perror("fwrite"); return 1;
        }

        for (int i = 0; i < NSAMPLES; i++) {
            total++;
            if (buf[i]) nonzero++;
            int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak) peak = a;
        }
    }

    fclose(out);
    fprintf(stderr, "oplverify: %llu samples, %.1f%% non-zero, peak %d -> %s\n",
            (unsigned long long)total, 100.0 * (double)nonzero / (double)total,
            (int)peak, out_path);
    /* A dump that is mostly silence proves nothing; fail loudly if the
     * sequence degenerated. */
    if ((double)nonzero / (double)total < 0.5) {
        fprintf(stderr, "oplverify: FAILED - dump is mostly silence, "
                        "the register sequence is not exercising the core\n");
        return 2;
    }
    return 0;
}
