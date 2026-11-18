/*
** Benchmark + bit-exactness harness for SBR HF generation (band patching).
**
** Compares the reference hf_generation (bench/hfgen_ref.c, snapshot of the
** original libfaad/sbr_hfgen.c) against the optimized one in libfaad/.
**
** Reports per-sample differences (must be exactly zero) and timings at
** three sample rates: 32000, 44100, 48000 Hz.
**
** Build:
**   gcc -O2 -DHAVE_INTTYPES_H=1 -DHAVE_STDINT_H=1 -DHAVE_SYS_TYPES_H=1 \
**       -DHAVE_STRING_H=1 -Ilibfaad -Iinclude -Ibench \
**       bench/hfgen_bench.c bench/hfgen_ref.c \
**       libfaad/sbr_hfgen.c libfaad/sbr_fbt.c libfaad/common.c \
**       -lm -o bench/hfgen_bench
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "common.h"
#include "structs.h"
#include "sbr_dec.h"
#include "sbr_syntax.h"
#include "sbr_hfgen.h"
#include "sbr_fbt.h"
#include "hfgen_ref.h"

#define ITERATIONS 4000
#define REPEATS 5

/* deterministic PRNG (xorshift32) */
static uint32_t rng_state = 0x12345678u;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static real_t frand(void)
{
    /* roughly [-1, 1) */
    return ((real_t)(xorshift32() & 0xFFFFFF) / (real_t)0x800000) - (real_t)1.0;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void setup_sbr(sbr_info *sbr, uint32_t sample_rate,
                      uint8_t start_freq, uint8_t stop_freq)
{
    uint8_t k2;

    memset(sbr, 0, sizeof(*sbr));

    /* mirror the defaults from sbrDecodeInit() */
    sbr->sample_rate = sample_rate;
    sbr->bs_freq_scale = 2;
    sbr->bs_alter_scale = 1;
    sbr->bs_noise_bands = 2;
    sbr->bs_limiter_bands = 2;
    sbr->bs_limiter_gains = 2;
    sbr->bs_interpol_freq = 1;
    sbr->bs_smoothing_mode = 1;
    sbr->bs_start_freq = start_freq;
    sbr->bs_stop_freq = stop_freq;
    sbr->bs_amp_res = 1;
    sbr->bs_samplerate_mode = 1;
    sbr->tHFGen = T_HFGEN;
    sbr->tHFAdj = T_HFADJ;
    sbr->numTimeSlotsRate = RATE * NO_TIME_SLOTS;
    sbr->numTimeSlots = NO_TIME_SLOTS;
    sbr->frame_len = 1024;
    sbr->Reset = 1;

    /* build the real frequency tables, as calc_sbr_tables() does */
    sbr->k0 = qmf_start_channel(sbr->bs_start_freq, sbr->bs_samplerate_mode,
                                sample_rate);
    k2 = qmf_stop_channel(sbr->bs_stop_freq, sample_rate, sbr->k0);
    master_frequency_table(sbr, sbr->k0, k2, sbr->bs_freq_scale,
                           sbr->bs_alter_scale);
    derived_frequency_table(sbr, 0, k2);

    /* one envelope covering the whole frame */
    sbr->L_E[0] = 1;
    sbr->t_E[0][0] = 0;
    sbr->t_E[0][1] = sbr->numTimeSlotsRate;

    /* exercise every inverse filtering mode so both the filtered and the
       plain-copy patching paths are hit */
    {
        uint8_t i;
        for (i = 0; i < 5; i++)
        {
            sbr->bs_invf_mode[0][i] = i % 4;
            sbr->bs_invf_mode_prev[0][i] = (i + 1) % 4;
        }
    }
}

typedef void (*hfgen_fn)(sbr_info *, qmf_t[MAX_NTSRHFG][64],
                         qmf_t[MAX_NTSRHFG][64]
#ifdef SBR_LOW_POWER
                         ,real_t *
#endif
                         ,uint8_t);

static double time_best(hfgen_fn fn, sbr_info *sbr,
                        qmf_t Xlow[MAX_NTSRHFG][64],
                        qmf_t Xhigh[MAX_NTSRHFG][64])
{
    double best = 1e30;
    int r, i;
    for (r = 0; r < REPEATS; r++)
    {
        double t0 = now_sec();
        for (i = 0; i < ITERATIONS; i++)
        {
            fn(sbr, Xlow, Xhigh
#ifdef SBR_LOW_POWER
               ,NULL
#endif
               ,0);
        }
        t0 = now_sec() - t0;
        if (t0 < best)
            best = t0;
    }
    return best;
}

static void fill_xlow(qmf_t Xlow[MAX_NTSRHFG][64])
{
    uint8_t l, k;
    for (l = 0; l < MAX_NTSRHFG; l++)
    {
        for (k = 0; k < 64; k++)
        {
            QMF_RE(Xlow[l][k]) = frand();
#ifndef SBR_LOW_POWER
            QMF_IM(Xlow[l][k]) = frand();
#endif
        }
    }
}

int main(void)
{
    static const uint32_t sample_rates[3] = { 32000, 44100, 48000 };
    /* start/stop freq chosen for a realistic ~32-band high frequency range */
    static const uint8_t start_freqs[3] = { 5, 5, 5 };
    static const uint8_t stop_freqs[3] = { 8, 9, 9 };
    static sbr_info sbr_ref, sbr_opt;
    static qmf_t Xlow[MAX_NTSRHFG][64];
    static qmf_t Xhigh_ref[MAX_NTSRHFG][64];
    static qmf_t Xhigh_opt[MAX_NTSRHFG][64];
    int sr_idx;
    int all_ok = 1;

    printf("%-10s %14s %14s %10s   %s\n", "rate(Hz)", "ref(us)", "opt(us)",
           "speedup", "samples");

    for (sr_idx = 0; sr_idx < 3; sr_idx++)
    {
        uint32_t sr = sample_rates[sr_idx];
        uint64_t mismatches = 0, compared = 0;
        double ref_sec, opt_sec;
        int i, l, k;

        setup_sbr(&sbr_ref, sr, start_freqs[sr_idx], stop_freqs[sr_idx]);
        setup_sbr(&sbr_opt, sr, start_freqs[sr_idx], stop_freqs[sr_idx]);

        /* ---- correctness: run several frames so that the bwArray
           smoothing reaches the plain-copy regime as well; compare
           every output sample of every frame ---- */
        rng_state = 0x12345678u;
        for (i = 0; i < 8; i++)
        {
            fill_xlow(Xlow);
            memset(Xhigh_ref, 0, sizeof(Xhigh_ref));
            memset(Xhigh_opt, 0, sizeof(Xhigh_opt));

            hf_generation_ref(&sbr_ref, Xlow, Xhigh_ref
#ifdef SBR_LOW_POWER
                              ,NULL
#endif
                              ,0);
            hf_generation(&sbr_opt, Xlow, Xhigh_opt
#ifdef SBR_LOW_POWER
                          ,NULL
#endif
                          ,0);

            for (l = 0; l < MAX_NTSRHFG; l++)
            {
                for (k = 0; k < 64; k++)
                {
                    compared++;
                    if (memcmp(&Xhigh_ref[l][k], &Xhigh_opt[l][k],
                               sizeof(qmf_t)) != 0)
                    {
                        if (mismatches < 5)
                        {
                            fprintf(stderr,
                                    "MISMATCH sr=%u frame=%d l=%d k=%d ref=%a opt=%a\n",
                                    (unsigned)sr, i, l, k,
                                    (double)QMF_RE(Xhigh_ref[l][k]),
                                    (double)QMF_RE(Xhigh_opt[l][k]));
                        }
                        mismatches++;
                    }
                }
            }
        }
        if (mismatches)
            all_ok = 0;

        /* steady-state hot path: tables are already built (Reset = 0),
           as in normal decoding after the first frame */
        sbr_ref.Reset = 0;
        sbr_opt.Reset = 0;

        /* ---- timing (best of REPEATS) ---- */
        ref_sec = time_best(hf_generation_ref, &sbr_ref, Xlow, Xhigh_ref);
        opt_sec = time_best(hf_generation, &sbr_opt, Xlow, Xhigh_opt);

        printf("%-10u %14.2f %14.2f %9.2fx   %llu/%llu %s\n", sr,
               ref_sec * 1e6 / ITERATIONS, opt_sec * 1e6 / ITERATIONS,
               ref_sec / opt_sec,
               (unsigned long long)(compared - mismatches),
               (unsigned long long)compared,
               mismatches ? "FAIL" : "identical");
    }

    if (!all_ok)
    {
        fprintf(stderr, "RESULT: FAIL (outputs differ)\n");
        return 1;
    }
    printf("RESULT: OK (bit-exact at all sample rates)\n");
    return 0;
}
