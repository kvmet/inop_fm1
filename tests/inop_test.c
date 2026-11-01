/* SPDX-License-Identifier: GPL-3.0-only */
/* Host tests of the INOP model and sequencer.
 *   cc -std=c11 -Wall -Wextra -Wno-unused-function -Ifirmware/inop -Ifirmware/src -Ibuild/gen tests/inop_test.c -o /tmp/inop_test && /tmp/inop_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "model.h"
#include "model.c"
#include "seq.c"
#include "overdub.c"
#include "voice_chain.c"
#include "master.c"
#include "fm_engine.c"
#include "perc_engine.c"
#include "swarm_engine.c"
#include "felucca_wave_tables.h"
#include "wave_engine.c"
#include "fx_sends.c"
#include "voice_steal.c"
#include "arp.c"
#define FM1_TICKS_PER_US 24u
static uint32_t fm1_ticks(void) { return 0; }
#define __attribute__(x)         /* the section names are the firmware's linker's */
#include "voices.c"
#undef __attribute__

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static sound_t pool[NSOUND];
static pattern_t patterns[NPATTERN];

static void reset(void)
{
    uint32_t i, k;
    for (i = 0; i < NSOUND; i++)
        sound_init(&pool[i], i % 8u);
    for (i = 0; i < NPATTERN; i++)
        for (k = 0; k < NTRK; k++)
            track_init(&patterns[i].track[k], k);
}

static void test_resolve_layers(void)
{
    track_pat_t *t = &patterns[0].track[0];
    pkt_t p;
    reset();
    t->steps[3].note = 72;
    pool[0].v[PV_FLT_CUT] = 10;
    pool[5].v[PV_FLT_CUT] = 20;
    pool[5].v[PV_PAN] = 7;
    packet_resolve(&p, pool, t, 3);
    CHECK(p.v[PV_FLT_CUT] == 10);                       /* base */
    CHECK(p.v[PV_NOTE] == 72);
    t->steps[3].sound = 5;
    packet_resolve(&p, pool, t, 3);
    CHECK(p.v[PV_FLT_CUT] == 20);                       /* the sound lock replaces the base */
    CHECK(p.v[PV_PAN] == 7);
    CHECK(lock_set(t, 3, PV_FLT_CUT, 30) == 0);
    packet_resolve(&p, pool, t, 3);
    CHECK(p.v[PV_FLT_CUT] == 30);                       /* the lock overrides the sound */
    CHECK(p.v[PV_PAN] == 7);
    packet_resolve(&p, pool, t, 4);
    CHECK(p.v[PV_FLT_CUT] == 10);                       /* other steps do not see it */
}

static void test_locks_stay_sorted(void)
{
    track_pat_t *t = &patterns[0].track[0];
    uint32_t i;
    reset();
    CHECK(lock_set(t, 9, 2, 1) == 0);
    CHECK(lock_set(t, 1, 8, 2) == 0);
    CHECK(lock_set(t, 9, 1, 3) == 0);
    CHECK(lock_set(t, 1, 8, 4) == 0);                   /* the same place: replaced */
    CHECK(t->nlock == 3);
    for (i = 1; i < t->nlock; i++)
        CHECK((t->locks[i - 1].step << 8 | t->locks[i - 1].id) < (t->locks[i].step << 8 | t->locks[i].id));
    lock_clear(t, 9, 1);
    CHECK(t->nlock == 2);
    lock_clear_step(t, 9);
    CHECK(t->nlock == 1 && t->locks[0].step == 1 && t->locks[0].value == 4);
    lock_clear_step(t, 63);
    CHECK(t->nlock == 1);
}

static void test_lock_capacity(void)
{
    track_pat_t *t = &patterns[0].track[0];
    uint32_t i;
    reset();
    for (i = 0; i < TRACK_LOCKS; i++)
        CHECK(lock_set(t, i / 4u, i % 4u, 1) == 0);
    CHECK(TRACK_LOCKS >= 4u * NSTEP);                   /* every step can hold 4 locks */
    CHECK(lock_set(t, 63, 50, 1) == -1);
    CHECK(t->nlock == TRACK_LOCKS);
    CHECK(lock_set(t, 0, 0, 9) == 0);                   /* full: replacing still works */
    CHECK(t->locks[0].value == 9);
}

static uint32_t fired[NTRK][256], nfired[NTRK];
static uint32_t emit_log(uint32_t k, uint32_t step, const track_pat_t *t, uint32_t hits)
{
    (void)t;
    (void)hits;
    fired[k][nfired[k]++ & 255u] = step;
    return 1;
}
static const track_pat_t *last_track;
static uint32_t emit_track(uint32_t k, uint32_t step, const track_pat_t *t, uint32_t hits)
{
    (void)k;
    (void)step;
    (void)hits;
    last_track = t;
    return 1;
}

static void test_polymeter(void)
{
    seq_t q;
    uint32_t i;
    reset();
    memset(nfired, 0, sizeof nfired);
    patterns[0].track[0].length = 16;
    patterns[0].track[1].length = 3;
    for (i = 0; i < 16; i++) {
        patterns[0].track[0].steps[i].on = 1;
        patterns[0].track[1].steps[i].on = 1;
    }
    seq_init(&q, 120);
    for (i = 0; i < 7; i++)
        seq_advance(&q, patterns, emit_log);
    CHECK(fired[0][6] == 7);
    CHECK(fired[1][0] == 1 && fired[1][1] == 2 && fired[1][2] == 0 && fired[1][6] == 1);   /* 3 steps, wrapping */
}

static void test_step_inherits_from_sound(void)
{
    track_pat_t *t = &patterns[0].track[0];
    pkt_t p;
    reset();
    pool[t->base].v[PV_NOTE] = 36;
    pool[t->base].v[PV_VEL] = 77;
    pool[t->base].v[PV_LEN] = 3;
    packet_resolve(&p, pool, t, 2);
    CHECK(p.v[PV_NOTE] == 36 && p.v[PV_VEL] == 77 && p.v[PV_LEN] == 3);
    t->steps[2].note = 40;
    packet_resolve(&p, pool, t, 2);
    CHECK(p.v[PV_NOTE] == 40 && p.v[PV_VEL] == 77);     /* a step's own value wins, the others still come from the sound */
    pool[t->base].v[PV_NOTE] = 50;
    packet_resolve(&p, pool, t, 3);
    CHECK(p.v[PV_NOTE] == 50);                          /* editing the sound moves every step that has not set its own */
}

static uint32_t stop_emits, stop_last_step;
static uint32_t stop_emit(uint32_t track, uint32_t step, const track_pat_t *t, uint32_t hits)
{
    (void)track;
    (void)t;
    (void)hits;
    stop_emits++;
    stop_last_step = step;
    return 1;
}

/* blocks of a square wave, so a block's peak is the amplitude. Returns the last block's largest output */
static int32_t master_run(master_t *m, const master_params_t *p, int32_t amp, uint32_t blocks)
{
    int32_t l[CTL], r[CTL], peak = 0;
    uint32_t b, i;
    for (b = 0; b < blocks; b++) {
        peak = 0;
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = i & 1u ? amp : -amp;
        master_block(m, l, r, p);
        for (i = 0; i < CTL; i++)
            peak = abs(l[i]) > peak ? abs(l[i]) : peak;
    }
    return peak;
}

static void test_master_log_exp(void)
{
    int32_t v;
    CHECK(abs(log2_q8(MASTER_LIMIT) - MASTER_LIMIT_LOG_Q8) <= 1);
    CHECK(log2_q8(1) == 0 && log2_q8(256) == 2048 && log2_q8(65536) == 4096);
    for (v = -3000; v <= 1000; v += 37) {
        double want = 4096.0 * pow(2.0, v / 256.0);
        CHECK(fabs(exp2_q12(v) - want) <= want * 0.004 + 2.0);
    }
    for (v = 1; v < 100000; v = v * 5 / 4 + 1)
        CHECK(fabs(log2_q8((uint32_t)v) - 256.0 * log2(v)) <= 4.0);
}

/* every stage at its neutral value is the limiter and soft clip alone, sample for sample */
static void test_master_neutral(void)
{
    const master_params_t p = MASTER_NEUTRAL;
    master_t a = MASTER_INIT, b = MASTER_INIT;
    uint32_t n, i, seed = 12345u;
    for (n = 0; n < 200u; n++) {
        int32_t l[CTL], r[CTL], x[CTL], y[CTL];
        for (i = 0; i < CTL; i++) {
            seed = seed * 1664525u + 1013904223u;
            l[i] = x[i] = (int32_t)(seed >> 16) - 32768;
            r[i] = y[i] = (int32_t)(seed & 0xFFFFu) - 32768;
        }
        master_block(&a, l, r, &p);
        for (i = 0; i < CTL; i++) {
            master_sample(&b, &x[i], &y[i]);
            CHECK(l[i] == x[i] && r[i] == y[i]);
        }
    }
    CHECK(a.reduction_q16 == 0 && a.gain_q12 == MASTER_GAIN_UNITY_Q12);
}

static void test_master_compressor_static(void)
{
    master_params_t p = MASTER_NEUTRAL;
    int32_t comp, slope;
    p.attack = 0;
    p.release = 0;
    for (comp = 32; comp <= 127; comp += 95) {
        master_t m = MASTER_INIT;
        int32_t threshold = comp_threshold_q8(comp), reduction, want;
        p.comp = comp;
        slope = comp_slope_q8(comp);
        master_run(&m, &p, exp2_q12(threshold + 256 - 12 * 256) , 1000);   /* 1 octave over */
        reduction = m.reduction_q16 >> 8;
        CHECK(abs(reduction - slope) <= 8);
        want = exp2_q12(comp_makeup_q8(comp) - reduction);
        CHECK(abs(m.gain_q12 - want) <= want / 50);
        master_run(&m, &p, exp2_q12(threshold - 256 - 12 * 256), 3000);    /* 1 octave under */
        CHECK(m.reduction_q16 >> 8 <= 2);
    }
    CHECK(comp_makeup_q8(0) == 0 && comp_makeup_q8(127) == 480);   /* half of 30 dB at 4:1: 11 dB */
    CHECK(comp_reduction_q8(3000, 127) > comp_reduction_q8(3000, 64));
    CHECK(comp_threshold_q8(127) == MASTER_LIMIT_LOG_Q8 - 5 * 256);
    CHECK(comp_slope_q8(127) == 192);                   /* 4:1 removes 3/4 */
}

static void test_master_compressor_times(void)
{
    master_params_t p = MASTER_NEUTRAL;
    master_t m = MASTER_INIT;
    int32_t amp = exp2_q12(comp_threshold_q8(127) + 512 - 12 * 256), full;
    uint32_t blocks;
    p.comp = 127;
    p.attack = 64;                                      /* 4 octaves: 16 blocks */
    p.release = 64;                                     /* 3 octaves: 320 blocks */
    master_run(&m, &p, amp, 2000);
    full = m.reduction_q16;
    CHECK(full > 0);
    m = (master_t)MASTER_INIT;
    for (blocks = 0; m.reduction_q16 < full * 63 / 100 && blocks < 1000u; blocks++)
        master_run(&m, &p, amp, 1);
    CHECK(blocks >= 10u && blocks <= 24u);
    master_run(&m, &p, amp, 2000);
    for (blocks = 0; m.reduction_q16 > full * 37 / 100 && blocks < 5000u; blocks++)
        master_run(&m, &p, 1, 1);
    CHECK(blocks >= 200u && blocks <= 450u);
}

/* COMP back at 0 returns to unity and then costs nothing */
static void test_master_compressor_off(void)
{
    master_params_t p = MASTER_NEUTRAL;
    master_t m = MASTER_INIT;
    p.comp = 127;
    master_run(&m, &p, 8000, 200);
    CHECK(m.gain_q12 != MASTER_GAIN_UNITY_Q12);
    p.comp = 0;
    master_run(&m, &p, 8000, 20000);
    CHECK(m.gain_q12 == MASTER_GAIN_UNITY_Q12 && m.reduction_q16 == 0);
}

static int32_t master_sine_peak(int32_t tilt, double hz)
{
    master_params_t p = MASTER_NEUTRAL;
    master_t m = MASTER_INIT;
    int32_t peak = 0;
    uint32_t n, i;
    p.tilt = tilt;
    for (n = 0; n < 200u; n++) {
        int32_t l[CTL], r[CTL];
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = (int32_t)(500.0 * sin(2.0 * M_PI * hz * (n * CTL + i) / FS));
        master_block(&m, l, r, &p);
        for (i = 0; n > 100u && i < CTL; i++)
            peak = abs(l[i]) > peak ? abs(l[i]) : peak;
    }
    return peak;
}

static void test_master_tilt(void)
{
    int32_t low = master_sine_peak(0, 100.0), high = master_sine_peak(0, 5000.0);
    CHECK(master_sine_peak(MASTER_TILT_MAX, 100.0) > low * 3 / 2);
    CHECK(master_sine_peak(MASTER_TILT_MAX, 5000.0) < high * 3 / 4);
    CHECK(master_sine_peak(-MASTER_TILT_MAX, 100.0) < low * 3 / 4);
    CHECK(master_sine_peak(-MASTER_TILT_MAX, 5000.0) > high * 3 / 2);
}

static void test_master_width(void)
{
    master_params_t p = MASTER_NEUTRAL;
    master_t m = MASTER_INIT;
    int32_t l[CTL], r[CTL];
    uint32_t i;
    p.width = 0;
    for (i = 0; i < CTL; i++) {
        l[i] = 1000;
        r[i] = -600;
    }
    master_block(&m, l, r, &p);
    for (i = 0; i < CTL; i++)
        CHECK(l[i] == r[i]);                            /* width 0 is mono */
    m = (master_t)MASTER_INIT;
    p.width = 127;
    for (i = 0; i < CTL; i++) {
        l[i] = 1000;
        r[i] = -600;
    }
    master_block(&m, l, r, &p);
    CHECK(l[0] - r[0] > 2800);                          /* the side about doubles */
}

static void test_master_drive(void)
{
    master_params_t p = MASTER_NEUTRAL;
    master_t a = MASTER_INIT, b = MASTER_INIT;
    int32_t flat = master_run(&a, &p, 400, 10);
    p.drive = 12;                                       /* 6 dB */
    CHECK(abs(master_run(&b, &p, 400, 10) - flat * 2) <= flat / 20);
    p.drive = -12;
    CHECK(abs(master_run(&b, &p, 400, 10) - flat / 2) <= flat / 20);
}

static void test_master_extremes_bound(void)
{
    uint32_t c, b, i;
    for (c = 0; c < 32u; c++) {
        master_params_t p = MASTER_NEUTRAL;
        master_t m = MASTER_INIT;
        p.comp = c & 1u ? 127 : 0;
        p.tilt = c & 2u ? MASTER_TILT_MAX : -MASTER_TILT_MAX;
        p.width = c & 4u ? 127 : 0;
        p.drive = c & 8u ? MASTER_DRIVE_MAX : -MASTER_DRIVE_MAX;
        p.attack = c & 16u ? 127 : 0;
        for (b = 0; b < 200u; b++) {
            int32_t l[CTL], r[CTL];
            for (i = 0; i < CTL; i++) {
                l[i] = (i >> 2) & 1u ? 500000 : -500000;
                r[i] = (i >> 3) & 1u ? 500000 : -500000;
            }
            master_block(&m, l, r, &p);
            for (i = 0; i < CTL; i++)
                CHECK(abs(l[i]) <= MASTER_CEILING && abs(r[i]) <= MASTER_CEILING);
        }
    }
}

/* the volume scales the output and leaves the stages before it alone */
static void test_master_volume_last(void)
{
    master_params_t p = MASTER_NEUTRAL;
    master_t a = MASTER_INIT, b = MASTER_INIT;
    int32_t full, quiet;
    p.comp = 100;
    full = master_run(&a, &p, 9000, 500);
    p.volume_q12 = 1024;
    quiet = master_run(&b, &p, 9000, 500);
    CHECK(a.reduction_q16 == b.reduction_q16 && a.env == b.env && a.gain_q12 == b.gain_q12);
    CHECK(abs(quiet - full / 4) <= 1);
}

static void test_mod_random_source(void)
{
    pkt_t p;
    chain_t a, b;
    int32_t first;
    uint32_t i;
    sound_init(&p, 0);
    p.v[PV_SLOT_SRC5] = MOD_SRC_RANDOM;
    p.v[PV_SLOT_DST5] = MOD_DST_PAN;
    p.v[PV_SLOT_AMT5] = 127;
    chain_start(&a, &p);
    chain_start(&b, &p);
    CHECK(a.random != b.random);                        /* each trigger draws its own */
    chain_mod(&a, &p, 0);
    first = a.off[MOD_DST_PAN];
    CHECK(first == (a.random * 127) >> 7);              /* the slots past the fourth run */
    for (i = 0; i < 100u; i++)
        chain_mod(&a, &p, i);
    CHECK(a.off[MOD_DST_PAN] == first);                 /* and it holds for the whole note */
}

static void test_mod_each_lfo_is_its_own_source(void)
{
    pkt_t p;
    chain_t c, copy;
    int32_t lfo1, lfo2;
    sound_init(&p, 0);
    p.v[PV_LFO1_SHAPE] = LFO_SINE;
    p.v[PV_LFO2_SHAPE] = LFO_SQUARE;
    p.v[PV_SLOT_SRC5] = MOD_SRC_LFO1;
    p.v[PV_SLOT_DST5] = MOD_DST_PAN;
    p.v[PV_SLOT_AMT5] = 127;
    p.v[PV_SLOT_SRC6] = MOD_SRC_LFO2;
    p.v[PV_SLOT_DST6] = MOD_DST_CUT;
    p.v[PV_SLOT_AMT6] = 127;
    chain_start(&c, &p);
    copy = c;
    lfo1 = lfo_block(&copy.lfo[0], 0);
    lfo2 = lfo_block(&copy.lfo[1], 0);
    CHECK(abs(lfo1 - lfo2) > 8000);
    chain_mod(&c, &p, 0);
    CHECK(c.off[MOD_DST_PAN] == (lfo1 * 127) >> 7);
    CHECK(c.off[MOD_DST_CUT] == (lfo2 * 127) >> 7);
}

static void test_mod_lfo_rate_route(void)
{
    pkt_t p;
    chain_t c;
    sound_init(&p, 0);
    p.v[PV_VEL] = 127;
    p.v[PV_LFO1_RATE] = 40;
    p.v[PV_SLOT_SRC4] = MOD_SRC_VEL;
    p.v[PV_SLOT_DST4] = MOD_DST_LFO1_RATE;
    p.v[PV_SLOT_AMT4] = 24;
    chain_start(&c, &p);
    chain_mod(&c, &p, 0);
    CHECK(abs((c.off[MOD_DST_LFO1_RATE] >> 8) - 24) <= 1);
    CHECK(c.lfo[0].inc == lfo_inc(40 + (c.off[MOD_DST_LFO1_RATE] >> 8)));
    CHECK(c.lfo[0].inc > lfo_inc(40) * 3u);             /* 24 values are 2 octaves */
    CHECK(c.lfo[1].inc == lfo_inc(p.v[PV_LFO2_RATE]));  /* the other LFO stays */
    p.v[PV_SLOT_AMT4] = 0;
    chain_mod(&c, &p, 1);
    CHECK(c.lfo[0].inc == lfo_inc(40));                 /* the rate returns when the route ends */
    p.v[PV_LFO1_PHASE] = LFO_FREE;
    p.v[PV_SLOT_AMT4] = 24;
    chain_start(&c, &p);
    chain_mod(&c, &p, 0);
    CHECK(c.lfo[0].inc == lfo_inc(40));                 /* a free LFO's rate does not move */
}

/* the starting route gives the sound FM had when its envelope was wired to the index */
static void test_fm_index_route_keeps_sound(void)
{
    pkt_t p;
    chain_t c;
    fm_voice_t routed, wired;
    uint32_t b, i;
    int32_t worst = 0, loudest = 0;
    sound_init(&p, 1);
    p.v[PV_NOTE] = 48;
    p.v[PV_E4] = p.v[PV_E5] = 100;
    p.v[PV_MOD_D] = 80;
    p.v[PV_MOD_S] = 20;
    chain_start(&c, &p);
    fm_start(&routed, &p);
    fm_start(&wired, &p);
    for (b = 0; b < 800u; b++) {
        int32_t x[CTL] = {0}, y[CTL] = {0};
        chain_mod(&c, &p, b);
        CHECK(abs(chain_fm_index_q15(&c) - (c.mod_env.env >> 9)) <= 3);
        fm_render(&routed, &p, x, 0, 0, chain_fm_index_q15(&c), 0);
        fm_render(&wired, &p, y, 0, 0, c.mod_env.env >> 9, 0);
        for (i = 0; i < CTL; i++) {
            worst = abs(x[i] - y[i]) > worst ? abs(x[i] - y[i]) : worst;
            loudest = abs(y[i]) > loudest ? abs(y[i]) : loudest;
        }
    }
    CHECK(loudest > 1000);
    CHECK(worst <= loudest / 20);                       /* at this index a few parts in 32768 of depth move a peak about 4 % */
}

static void test_fm_index_routes(void)
{
    pkt_t p;
    chain_t c;
    uint32_t b;
    sound_init(&p, 0);
    p.v[PV_MOD_D] = 20;
    p.v[PV_MOD_S] = 0;
    p.v[PV_MOD2_S] = 127;
    chain_start(&c, &p);
    for (b = 0; b < 3000u; b++)
        chain_mod(&c, &p, b);
    CHECK(chain_fm_index_q15(&c) < 200);                /* the route follows the envelope down */
    p.v[PV_SLOT_SRC4] = MOD_SRC_ENV2;
    chain_start(&c, &p);
    for (b = 0; b < 3000u; b++)
        chain_mod(&c, &p, b);
    CHECK(chain_fm_index_q15(&c) > 32000);              /* a second envelope drives it apart from the first */
    CHECK(c.mod_env.env >> 9 < 200);
    p.v[PV_SLOT_SRC4] = MOD_SRC_OFF;
    chain_start(&c, &p);
    for (b = 0; b < 3000u; b++)
        chain_mod(&c, &p, b);
    CHECK(chain_fm_index_q15(&c) == 32768);             /* no route: the knob's value */
    p.v[PV_SLOT_SRC4] = MOD_SRC_ENV2;
    p.v[PV_SLOT_AMT4] = 0;
    chain_start(&c, &p);
    chain_mod(&c, &p, 0);
    CHECK(chain_fm_index_q15(&c) == 0);                 /* amount 0 is the end of the knob's range, with no jump to full */
    p.v[PV_SLOT_AMT4] = -127;
    chain_start(&c, &p);
    for (b = 0; b < 3000u; b++)
        chain_mod(&c, &p, b);
    CHECK(chain_fm_index_q15(&c) == 0);                 /* a scale does not go below 0 */
    chain_release(&c);
    CHECK(c.mod2_env.stage == ENV_RELEASE);             /* the second envelope ends with the note */
}

static void test_lfo_sync(void)
{
    enum { RATE_1_16 = 10, RATE_1_BAR = 79 };
    lfo_t a, b;
    seq_t q;
    lfo_start(&a, LFO_SINE, LFO_SYNC, RATE_1_16, 0);
    CHECK(a.cycle == LFO_SYNC_UNITS_PER_STEP);
    lfo_sync_pos = 0;
    CHECK(abs(lfo_block(&a, 0)) < 300);                 /* a cycle starts on the grid */
    lfo_sync_pos = LFO_SYNC_UNITS_PER_STEP / 4u;
    CHECK(lfo_block(&a, 12345) > 32000);                /* a quarter of a step on: the peak, whatever the block clock says */
    lfo_sync_pos = LFO_SYNC_UNITS_PER_STEP / 4u + LFO_SYNC_UNITS_PER_STEP * 7u;
    CHECK(lfo_block(&a, 0) > 32000);                    /* 7 cycles later, the same */
    lfo_start(&b, LFO_SINE, LFO_SYNC, RATE_1_BAR, 0);
    CHECK(b.cycle == 16u * LFO_SYNC_UNITS_PER_STEP);
    lfo_sync_pos = 4u * LFO_SYNC_UNITS_PER_STEP;
    CHECK(lfo_block(&b, 0) > 32000);                    /* 4 steps are a quarter of a bar */
    seq_init(&q, 120);
    seq_block(&q, patterns, q.samples_per_step * 3u + q.samples_per_step / 2u, stop_emit);
    CHECK(seq_sync_pos(&q) == 3u * LFO_SYNC_UNITS_PER_STEP + LFO_SYNC_UNITS_PER_STEP / 2u);
    seq_stop(&q);
    CHECK(seq_sync_pos(&q) == 0u);                      /* stop rewinds it */
}

/* voices that share a clock hold one S&H value, and a late voice joins with the same value */
static void test_lfo_shared_sample_hold(void)
{
    static const int MODES[2] = {LFO_FREE, LFO_SYNC};
    uint32_t m, n, changes;
    for (m = 0; m < 2u; m++) {
        lfo_t a, b;
        int32_t v = 0;
        lfo_start(&a, LFO_SH, MODES[m], 60, 0);
        changes = 0;
        for (n = 0; n < 3000u; n++) {
            int32_t now;
            lfo_sync_pos = n * 40u;
            now = lfo_block(&a, n);
            if (n == 1500u)
                lfo_start(&b, LFO_SH, MODES[m], 60, 0);
            if (n >= 1500u)
                CHECK(lfo_block(&b, n) == now);
            changes += n && now != v;
            v = now;
        }
        CHECK(changes > 2u);
    }
    lfo_sync_pos = 0;
}

static void follower_voices_clear(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        voices[i].active = 0;
    follower_input = follower_level = 0;
}

static void follower_blocks(uint32_t blocks)
{
    int32_t l[CTL], r[CTL], d[CTL], w[CTL];
    uint32_t b, i;
    for (b = 0; b < blocks; b++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        voices_render(l, r, d, w);
        follower_block(0, 50);
    }
}

static void follower_sound(pkt_t *p, int32_t send)
{
    sound_init(p, 0);
    p->v[PV_ENGINE] = ENGINE_WAVE;
    p->v[PV_E1] = 64;
    p->v[PV_AMP_S] = 127;
    p->v[PV_SEND_FOLLOW] = (int8_t)send;
}

static void test_follower_send(void)
{
    pkt_t p;
    int32_t loud;
    follower_voices_clear();
    follower_sound(&p, 0);
    voice_start(&voices[0], &p, KEY_NONE, 0);
    follower_blocks(100);
    CHECK(follower_level == 0);                         /* no send, no signal */
    follower_voices_clear();
    follower_sound(&p, 127);
    voice_start(&voices[0], &p, KEY_NONE, 0);
    follower_blocks(100);
    loud = follower_level;
    CHECK(loud > 8000);
    follower_voices_clear();
    follower_sound(&p, 127);
    p.v[PV_LEVEL] = 0;
    voice_start(&voices[0], &p, KEY_NONE, 0);
    follower_blocks(100);
    CHECK(abs(follower_level - loud) <= loud / 10);     /* LEVEL does not reach the send: a silent track can drive it */
    follower_voices_clear();
    follower_sound(&p, 64);
    voice_start(&voices[0], &p, KEY_NONE, 0);
    follower_blocks(100);
    CHECK(abs(follower_level - loud / 2) <= loud / 8);  /* the send scales it */
    voices[0].active = 0;
    follower_blocks(3000);
    CHECK(follower_level == 0);                         /* it falls to nothing */
}

/* a route reads one value in a block, whatever the voice's place among the voices */
static void test_follower_ducks_any_voice(void)
{
    pkt_t sender, ducked;
    follower_voices_clear();
    follower_sound(&sender, 127);
    follower_sound(&ducked, 0);
    ducked.v[PV_SLOT_SRC5] = MOD_SRC_FOLLOW;
    ducked.v[PV_SLOT_DST5] = MOD_DST_LEVEL;
    ducked.v[PV_SLOT_AMT5] = -127;
    voice_start(&voices[0], &ducked, KEY_NONE, 0);
    voice_start(&voices[1], &sender, KEY_NONE, 0);
    voice_start(&voices[2], &ducked, KEY_NONE, 0);
    follower_blocks(100);
    CHECK(voices[0].chain.off[MOD_DST_LEVEL] < -(40 << 8));
    CHECK(voices[0].chain.off[MOD_DST_LEVEL] == voices[2].chain.off[MOD_DST_LEVEL]);
    follower_voices_clear();
}

static void test_follower_times(void)
{
    uint32_t b;
    follower_voices_clear();
    for (b = 0; follower_level < 20000 && b < 1000u; b++) {     /* 63 % of 32000 */
        follower_input = 32000;
        follower_block(64, 64);                         /* 4 octaves: 16 blocks */
    }
    CHECK(b >= 10u && b <= 24u);
    follower_level = 32000;
    follower_input = 0;
    for (b = 0; follower_level > 11840 && b < 5000u; b++)   /* 37 % */
        follower_block(64, 64);                         /* 3 octaves: 320 blocks */
    CHECK(b >= 200u && b <= 450u);
    follower_voices_clear();
}

static void test_mod_route_slot(void)
{
    pkt_t p;
    uint32_t i;
    sound_init(&p, 0);
    CHECK(mod_route_slot(&p, MOD_SRC_LFO1, MOD_DST_PITCH) == 1);   /* the route that exists, at amount 0 */
    CHECK(mod_route_slot(&p, MOD_SRC_LFO1, MOD_DST_PAN) == 0);     /* a new route: the first slot doing nothing */
    p.v[PV_SLOT_AMT0] = 10;
    CHECK(mod_route_slot(&p, MOD_SRC_LFO1, MOD_DST_PAN) == 1);     /* a slot in use stays */
    CHECK(mod_route_slot(&p, MOD_SRC_ENV, MOD_DST_INDEX) == 4);
    p.v[PV_SLOT_AMT4] = 0;
    for (i = 0; i < 4u; i++)
        p.v[PV_SLOT_AMT0 + i] = 10;
    CHECK(mod_route_slot(&p, MOD_SRC_LFO1, MOD_DST_PAN) == 5);     /* a route to the index at 0 is in use: it closes the index */
    for (i = 0; i < MOD_SLOTS; i++) {
        p.v[PV_SLOT_SRC0 + i] = MOD_SRC_VEL;
        p.v[PV_SLOT_DST0 + i] = MOD_DST_CUT;
        p.v[PV_SLOT_AMT0 + i] = 10;
    }
    CHECK(mod_route_slot(&p, MOD_SRC_LFO1, MOD_DST_PAN) == -1);    /* all in use */
    p.v[PV_SLOT_DST3] = MOD_DST_OFF;
    CHECK(mod_route_slot(&p, MOD_SRC_LFO1, MOD_DST_PAN) == 3);     /* no destination: does nothing at any amount */
    p.v[PV_SLOT_DST3] = MOD_DST_CUT;
    p.v[PV_SLOT_SRC2] = MOD_SRC_OFF;
    CHECK(mod_route_slot(&p, MOD_SRC_LFO1, MOD_DST_PAN) == 2);     /* no source: the same */
}

/* every destination has a knob that leads to it */
static void test_mod_dst_of_value(void)
{
    uint32_t seen = 1u << MOD_DST_OFF, id;
    int32_t engine;
    for (engine = 0; engine < ENGINES; engine++)
        for (id = 0; id < PV_COUNT; id++)
            seen |= 1u << mod_dst_of_value(id, engine);
    CHECK(seen == (1u << MOD_DSTS) - 1u);
    CHECK(mod_dst_of_value(PV_E4, ENGINE_FM) == MOD_DST_INDEX && mod_dst_of_value(PV_E4, ENGINE_WAVE) == MOD_DST_OFF);
    CHECK(mod_dst_of_value(PV_E1, ENGINE_WAVE) == MOD_DST_TIMBRE && mod_dst_of_value(PV_E2, ENGINE_SWARM) == MOD_DST_TIMBRE);
    CHECK(mod_dst_of_value(PV_E1, ENGINE_FM) == MOD_DST_OFF);
    CHECK(mod_dst_of_value(PV_AMP_D, ENGINE_FM) == MOD_DST_OFF);
}

#define HIT_LOG 2048u
static struct { uint32_t at; uint8_t track, step, hits; } hit_log[HIT_LOG];
static uint32_t hit_count, hit_now, hit_span = 1;
static seq_t hit_q;
static uint32_t emit_hit(uint32_t k, uint32_t step, const track_pat_t *t, uint32_t hits)
{
    (void)t;
    if (hit_count < HIT_LOG) {
        hit_log[hit_count].at = hit_now;
        hit_log[hit_count].track = (uint8_t)k;
        hit_log[hit_count].step = (uint8_t)step;
        hit_log[hit_count].hits = (uint8_t)hits;
        hit_count++;
    }
    return hit_span;
}

static void hit_run(uint32_t samples)
{
    uint32_t i;
    for (i = 0; i < samples; i++) {
        hit_now++;
        seq_block(&hit_q, patterns, 1, emit_hit);
    }
}

#define SPB 5512u                /* a step at 120 BPM */

/* one track on, step 0 not, the clock at 0 */
static void hit_start(void)
{
    uint32_t k;
    reset();
    seq_init(&hit_q, 120);
    CHECK(hit_q.samples_per_step == SPB);
    for (k = 0; k < NTRK; k++)
        track_init(&patterns[0].track[k], 0);
    hit_count = 0;
    hit_now = 0;
    hit_span = 1;
}

static void test_seq_late_step(void)
{
    hit_start();
    patterns[0].track[0].steps[1].on = 1;
    patterns[0].track[0].steps[1].time = 12;
    hit_run(SPB + SPB / 4u + 10u);
    CHECK(hit_count == 1);
    CHECK(hit_log[0].at == SPB + 12u * SPB / SEQ_TIME_UNITS);
    hit_run(SPB);
    CHECK(hit_count == 1);                              /* once */
}

static void test_seq_early_step(void)
{
    hit_start();
    patterns[0].track[0].steps[2].on = 1;
    patterns[0].track[0].steps[2].time = -12;
    hit_run(3u * SPB);
    CHECK(hit_count == 1);                              /* not again on the grid */
    CHECK(hit_log[0].at == 2u * SPB - 12u * SPB / SEQ_TIME_UNITS);
    CHECK(hit_q.fired[0] == 2);
    hit_start();
    patterns[0].track[0].length = 4;                    /* an early step 1 belongs to the end of the last step */
    patterns[0].track[0].steps[0].on = 1;
    patterns[0].track[0].steps[0].time = -SEQ_TIME_MAX;
    hit_run(4u * SPB + 10u);
    CHECK(hit_count == 1 && hit_log[0].at == 4u * SPB - SPB / 2u && hit_log[0].step == 0);
}

static void test_seq_early_step_edits(void)
{
    hit_start();
    patterns[0].track[0].steps[1].on = 1;
    patterns[0].track[0].steps[1].time = -6;
    hit_run(SPB / 2u + 100u);                           /* looked ahead, not fired */
    patterns[0].track[0].steps[1].on = 0;
    hit_run(SPB);
    CHECK(hit_count == 0);                              /* turned off in time */
    hit_start();
    patterns[0].track[0].steps[1].on = 1;
    patterns[0].track[0].steps[1].time = -6;
    hit_run(SPB / 2u + 100u);
    hit_q.skip[0] = 2;                                      /* played in live */
    hit_run(SPB);
    CHECK(hit_count == 0 && hit_q.skip[0] == 0);
    hit_start();
    hit_run(SPB / 2u + 100u);
    patterns[0].track[0].steps[1].on = 1;               /* an edit after the look ahead still fires, on the grid */
    patterns[0].track[0].steps[1].time = -6;
    hit_run(SPB);
    CHECK(hit_count == 1 && hit_log[0].at == SPB);
    hit_start();
    patterns[0].track[0].steps[1].on = 1;
    patterns[0].track[0].steps[1].time = 6;
    patterns[0].track[0].steps[1].chance = 0;
    hit_run(2u * SPB);
    patterns[0].track[0].steps[1].chance = 100;
    CHECK(hit_count == 0);
    hit_start();
    patterns[0].track[0].steps[1].on = 1;               /* a step on the grid decides on the grid */
    patterns[0].track[0].steps[1].chance = 0;
    hit_run(SPB - 10u);
    patterns[0].track[0].steps[1].chance = 100;
    hit_run(20u);
    CHECK(hit_count == 1 && hit_log[0].at == SPB);
}

static void test_seq_early_step_pattern_change(void)
{
    uint32_t k;
    hit_start();
    for (k = 0; k < NTRK; k++) {
        track_init(&patterns[1].track[k], 0);
        patterns[0].track[k].length = 2;
        patterns[1].track[k].length = 2;
    }
    patterns[0].track[0].steps[0].on = 1;
    patterns[0].track[0].steps[0].time = -2;
    patterns[1].track[0].steps[0].on = 1;
    patterns[1].track[0].steps[0].time = -6;
    hit_q.next = 1;                                         /* chosen in time: the new pattern's step fires early */
    hit_run(2u * SPB + 5u);
    CHECK(hit_count == 1 && hit_log[0].at == 2u * SPB - 6u * SPB / SEQ_TIME_UNITS);
    CHECK(hit_q.playing == 1);
    hit_start();
    for (k = 0; k < NTRK; k++) {
        track_init(&patterns[1].track[k], 0);
        patterns[0].track[k].length = 2;
        patterns[1].track[k].length = 2;
    }
    patterns[0].track[0].steps[0].on = 1;
    patterns[0].track[0].steps[0].time = -2;
    patterns[1].track[0].steps[1].on = 1;
    hit_run(2u * SPB - SPB / 4u);                       /* looked ahead, not fired */
    hit_q.next = 1;                                         /* chosen late: the old pattern's step must not fire */
    hit_run(SPB / 4u + SPB + 5u);
    CHECK(hit_count == 1 && hit_log[0].step == 1 && hit_log[0].at == 3u * SPB);
    hit_start();
    for (k = 0; k < NTRK; k++) {
        track_init(&patterns[1].track[k], 0);
        patterns[0].track[k].length = 2;
        patterns[1].track[k].length = 2;
    }
    hit_span = 4;                                       /* a new pattern ends a ratchet from the old one */
    patterns[0].track[0].steps[1].on = 1;
    patterns[0].track[0].steps[1].hits = 8;
    hit_q.next = 1;
    hit_run(3u * SPB);
    CHECK(hit_count == 2 && hit_q.playing == 1);
}

static void test_seq_ratchet(void)
{
    uint32_t i, kept = 0;
    hit_start();
    patterns[0].track[0].steps[1].on = 1;
    patterns[0].track[0].steps[1].hits = 4;
    hit_run(3u * SPB);
    CHECK(hit_count == 4);
    for (i = 0; i < 4u; i++) {
        CHECK(hit_log[i].at == SPB + i * (SPB / 4u));
        CHECK(hit_log[i].hits == 4 && hit_log[i].step == 1);
    }
    hit_start();
    hit_span = 2;                                       /* a longer note: the hits spread over it */
    patterns[0].track[0].steps[1].on = 1;
    patterns[0].track[0].steps[1].hits = 3;
    hit_run(4u * SPB);
    CHECK(hit_count == 3 && hit_log[1].at == SPB + 2u * SPB / 3u && hit_log[2].at == SPB + 2u * (2u * SPB / 3u));
    hit_start();
    hit_span = 4;                                       /* the next step on the track ends the ratchet */
    patterns[0].track[0].steps[1].on = 1;
    patterns[0].track[0].steps[1].hits = 8;
    patterns[0].track[0].steps[2].on = 1;
    hit_run(2u * SPB + SPB / 2u);
    CHECK(hit_count == 3 && hit_log[0].step == 1 && hit_log[1].step == 1 && hit_log[2].step == 2 && hit_log[2].at == 2u * SPB);
    hit_start();                                        /* chance decides for the whole step */
    patterns[0].track[0].length = 2;
    patterns[0].track[0].steps[1].on = 1;
    patterns[0].track[0].steps[1].hits = 2;
    patterns[0].track[0].steps[1].chance = 50;
    hit_run(200u * SPB);
    CHECK(hit_count % 2u == 0u && hit_count > 60u && hit_count < 140u);
    for (i = 0; i + 1u < hit_count; i += 2u) {
        kept += hit_log[i + 1u].at - hit_log[i].at == SPB / 2u;
    }
    CHECK(kept == hit_count / 2u);
    hit_start();                                        /* a late ratchet moves as one */
    patterns[0].track[0].steps[1].on = 1;
    patterns[0].track[0].steps[1].hits = 2;
    patterns[0].track[0].steps[1].time = 6;
    hit_run(3u * SPB);
    CHECK(hit_count == 2 && hit_log[0].at == SPB + 6u * SPB / SEQ_TIME_UNITS && hit_log[1].at == hit_log[0].at + SPB / 2u);
    hit_start();                                        /* stop drops what waits */
    patterns[0].track[0].steps[1].on = 1;
    patterns[0].track[0].steps[1].hits = 4;
    hit_run(SPB + 10u);
    CHECK(hit_count == 1);
    seq_stop(&hit_q);
    patterns[0].track[0].steps[1].on = 0;
    seq_start(&hit_q);
    hit_run(3u * SPB);
    CHECK(hit_count == 1);
}

static void test_step_timing_clears(void)
{
    track_pat_t *t = &patterns[0].track[0];
    reset();
    t->steps[3].hits = 4;
    CHECK(step_locked(t, 3));
    step_clear_locks(t, 3);
    CHECK(t->steps[3].hits == 1 && !step_locked(t, 3));
    t->steps[3].time = -5;
    CHECK(step_locked(t, 3));
    step_clear_locks(t, 3);
    CHECK(t->steps[3].time == 0 && !step_locked(t, 3));
}

static void test_seq_swing(void)
{
    uint32_t i;
    hit_start();
    patterns[0].track[0].swing = 12;
    for (i = 1; i < 5u; i++) {
        patterns[0].track[0].steps[i].on = 1;
        patterns[0].track[1].steps[i].on = 1;
    }
    hit_run(5u * SPB);
    CHECK(hit_count == 8);
    for (i = 0; i < hit_count; i++) {
        uint32_t step = hit_log[i].step, late = hit_log[i].track == 0 && (step & 1u) ? 12u * SPB / SEQ_TIME_UNITS : 0u;
        CHECK(hit_log[i].at == step * SPB + late);      /* the second of each pair, on this track only */
    }
}

static void test_seq_human(void)
{
    int64_t sum = 0, squares = 0, lag = 0;
    int32_t last = 0, peak = 0;
    uint32_t i, n = 0;
    const int32_t human = 6;
    hit_start();
    for (i = 0; i < NSTEP; i++)
        patterns[0].track[0].steps[i].on = 1;
    patterns[0].track[0].length = 16;
    hit_run(8u * SPB);
    for (i = 0; i < hit_count; i++)
        CHECK(hit_log[i].at == hit_log[i].step * SPB || i == 0);   /* 0: the grid, exactly */
    hit_start();
    for (i = 0; i < 16u; i++)
        patterns[0].track[0].steps[i].on = 1;
    patterns[0].track[0].human = (uint8_t)human;
    hit_run(1000u * SPB);
    CHECK(hit_count >= 950u);
    for (i = 5; i < hit_count; i++) {
        uint32_t grid = (hit_log[i].at + SPB / 2u) / SPB * SPB;
        int32_t offset = (int32_t)hit_log[i].at - (int32_t)grid;
        CHECK(hit_log[i].step == grid / SPB % 16u);     /* still on its own step */
        sum += offset;
        squares += (int64_t)offset * offset;
        lag += (int64_t)offset * last;
        peak = abs(offset) > peak ? abs(offset) : peak;
        last = offset;
        n++;
    }
    {
        double unit = (double)SPB / SEQ_TIME_UNITS, spread = sqrt((double)squares / n);
        CHECK(fabs((double)sum / n) < spread * 0.2);    /* no lean ahead or behind */
        CHECK(spread > human * unit * 0.6 && spread < human * unit * 1.4);
        CHECK((double)lag / (double)squares > 0.5);     /* each offset stays near the last: a wander */
        CHECK(peak <= (int32_t)(SPB / 2u));
    }
}

/* the amounts add, and stop at half a step, so hits never swap */
static void test_seq_offset_limit(void)
{
    uint32_t i;
    hit_start();
    for (i = 0; i < 16u; i++) {
        patterns[0].track[0].steps[i].on = 1;
        patterns[0].track[0].steps[i].time = (i & 1u) ? SEQ_TIME_MAX : -SEQ_TIME_MAX;
    }
    patterns[0].track[0].swing = SEQ_TIME_MAX;
    patterns[0].track[0].human = SEQ_TIME_MAX;
    hit_run(200u * SPB);
    CHECK(hit_count > 150u);
    for (i = 1; i < hit_count; i++) {
        int32_t cycle = 16 * (int32_t)SPB, from_grid = ((int32_t)hit_log[i].at - hit_log[i].step * (int32_t)SPB + cycle / 2 + 1000 * cycle) % cycle - cycle / 2;
        CHECK(hit_log[i].at >= hit_log[i - 1u].at);
        CHECK(abs(from_grid) <= (int32_t)(SPB / 2u));
    }
}

static void test_voice_ratchet_gate(void)
{
    pkt_t p;
    memset(voices, 0, sizeof voices);
    sound_init(&p, 0);
    p.v[PV_LEN] = 4;
    CHECK(voice_trigger_step(&p, 2, 5120u, 4u) == 4u);
    CHECK(voices[0].hold_blocks == 5120u / CTL);        /* a hit's share of 4 steps */
    memset(voices, 0, sizeof voices);
    p.v[PV_LEN] = (int8_t)LEN_HOLD;
    CHECK(voice_trigger_step(&p, 2, 5120u, 4u) == 1u);  /* a held note cannot retrigger: the ratchet takes a step */
    CHECK(voices[0].hold_blocks == 5120u / 4u / CTL && voices[0].key == KEY_NONE);
    memset(voices, 0, sizeof voices);
    CHECK(voice_trigger_step(&p, 2, 5120u, 1u) == 1u);
    CHECK(voices[0].hold_blocks == 0 && voices[0].key == KEY_TRACK(2));
}

/* the amounts of one voice's left and right over many blocks */
static void stereo_measure(pkt_t *p, double *rms_l, double *rms_r, double *correlation)
{
    double ll = 0, rr = 0, lr = 0;
    uint32_t b, i;
    p->v[PV_AMP_S] = 127;
    p->v[PV_SEND_DELAY] = p->v[PV_SEND_REVERB] = 0;
    memset(voices, 0, sizeof voices);
    voice_budget = 0xFFFFFFFFu;
    voice_start(&voices[0], p, KEY_NONE, 0);
    for (b = 0; b < 3000u; b++) {
        int32_t l[CTL] = {0}, r[CTL] = {0}, d[CTL] = {0}, w[CTL] = {0};
        voice_render(&voices[0], l, r, d, w);
        for (i = 0; b > 200u && i < CTL; i++) {
            ll += (double)l[i] * l[i];
            rr += (double)r[i] * r[i];
            lr += (double)l[i] * r[i];
        }
    }
    *rms_l = sqrt(ll);
    *rms_r = sqrt(rr);
    *correlation = ll && rr ? lr / sqrt(ll * rr) : 1.0;
}

static void stereo_packet(pkt_t *p, int engine)
{
    voice_bare_packet(p, (uint32_t)engine);
    p->v[PV_NOTE] = 48;
    p->v[PV_LEVEL] = 104;
    p->v[PV_MOD_S] = 127;                               /* FM's index holds: a carrier other than C follows it */
}

static void test_stereo_engines(void)
{
    static const int ENGINE_LIST[3] = {ENGINE_SWARM, ENGINE_WAVE, ENGINE_FM};
    pkt_t p;
    double l, r, correlation, mono_l, mono_r, l40, r40, correlation40;
    uint32_t i, alg;
    for (i = 0; i < 3u; i++) {
        stereo_packet(&p, ENGINE_LIST[i]);
        p.v[PV_E5] = ENGINE_LIST[i] == ENGINE_SWARM ? 0 : 127;   /* SWARM's sub is in both channels */
        if (ENGINE_LIST[i] == ENGINE_FM) {
            p.v[PV_E4] = 127;
            p.v[PV_E7] = 40;                            /* B1 and B2 share a ratio: without detune they match */
        }
        stereo_measure(&p, &mono_l, &mono_r, &correlation);
        CHECK(correlation > 0.9999 && fabs(mono_l - mono_r) < 1.0);   /* spread 0: one channel */
        p.v[PV_STEREO] = 127;
        stereo_measure(&p, &l, &r, &correlation);
        CHECK(correlation < 0.7);                       /* the channels part */
        CHECK(l > 1000.0 && r > 1000.0);
        CHECK(fabs(20.0 * log10(l / mono_l)) < 2.5);    /* and spread keeps the level */
        CHECK(fabs(20.0 * log10(r / mono_r)) < 2.5);
        p.v[PV_STEREO] = 40;
        stereo_measure(&p, &l40, &r40, &correlation40);
        CHECK(correlation40 > correlation);             /* less spread is less apart */
    }
    stereo_packet(&p, ENGINE_SWARM);
    p.v[PV_E5] = 0;
    p.v[PV_STEREO] = 127;
    stereo_measure(&p, &l, &r, &correlation);
    CHECK(fabs(20.0 * log10(l / r)) < 1.5);             /* SWARM's sides match */
    for (alg = FM_ALG_TWO_CARRIERS; alg < FM_ALGS; alg++) {
        stereo_packet(&p, ENGINE_FM);
        p.v[PV_E0] = (int8_t)alg;
        p.v[PV_E4] = p.v[PV_E5] = 127;
        p.v[PV_E7] = 40;
        p.v[PV_STEREO] = 127;
        stereo_measure(&p, &l, &r, &correlation);
        CHECK(correlation < 0.7);
    }
}

/* what stays in both keeps its level, and a quiet source does not pull the voice to one side */
static void test_stereo_center_and_balance(void)
{
    pkt_t p;
    double l, r, correlation, mono_l, mono_r;
    stereo_packet(&p, ENGINE_SWARM);
    p.v[PV_E4] = 1;                                     /* the center and the sub, and the sides almost silent */
    stereo_measure(&p, &mono_l, &mono_r, &correlation);
    p.v[PV_STEREO] = 127;
    stereo_measure(&p, &l, &r, &correlation);
    CHECK(fabs(20.0 * log10(l / mono_l)) < 0.5);
    CHECK(fabs(20.0 * log10(r / mono_r)) < 0.5);
    stereo_packet(&p, ENGINE_WAVE);
    p.v[PV_E7] = 0;
    p.v[PV_E4] = 0;
    p.v[PV_E5] = 8;
    stereo_measure(&p, &mono_l, &mono_r, &correlation);
    p.v[PV_STEREO] = 127;
    stereo_measure(&p, &l, &r, &correlation);
    CHECK(fabs(20.0 * log10(l / r)) < 1.0);
    CHECK(fabs(20.0 * log10(l / mono_l)) < 0.5);
    stereo_packet(&p, ENGINE_WAVE);
    p.v[PV_E4] = 0;
    p.v[PV_E5] = 1;
    p.v[PV_E7] = 127;                                   /* noise: each side's own, at the level of a mono voice's one */
    stereo_measure(&p, &mono_l, &mono_r, &correlation);
    p.v[PV_STEREO] = 127;
    stereo_measure(&p, &l, &r, &correlation);
    CHECK(fabs(20.0 * log10(l / mono_l)) < 1.0);
    CHECK(correlation < 0.7);
    {
        uint32_t alg;
        for (alg = FM_ALG_TWO_CARRIERS; alg < FM_ALGS; alg++) {
            stereo_packet(&p, ENGINE_FM);
            p.v[PV_E0] = (int8_t)alg;
            p.v[PV_E5] = p.v[PV_E4] = 0;                /* only C sounds */
            stereo_measure(&p, &mono_l, &mono_r, &correlation);
            p.v[PV_STEREO] = 127;
            stereo_measure(&p, &l, &r, &correlation);
            CHECK(correlation > 0.999);
            CHECK(fabs(20.0 * log10(l / mono_l)) < 0.5);
        }
    }
}

/* a voice with one sound to part stays mono, and pays nothing */
static void test_stereo_only_with_two_sources(void)
{
    pkt_t p;
    double l, r, correlation;
    uint32_t alg, op;
    sound_init(&p, 0);
    p.v[PV_STEREO] = 127;
    for (alg = 0; alg < FM_ALGS; alg++) {
        uint32_t carriers = 0;
        for (op = 0; op < FM_OPS; op++)
            carriers += (uint32_t)fm_is_carrier(alg, op);
        p.v[PV_ENGINE] = ENGINE_FM;
        p.v[PV_E0] = (int8_t)alg;
        CHECK(voice_stereo(&p) == (carriers > 1u));
    }
    p.v[PV_STEREO] = 0;
    CHECK(!voice_stereo(&p));
    p.v[PV_STEREO] = 127;
    p.v[PV_ENGINE] = ENGINE_PERC;
    CHECK(!voice_stereo(&p));
    p.v[PV_ENGINE] = ENGINE_SWARM;
    p.v[PV_E4] = 0;
    CHECK(!voice_stereo(&p));                           /* the center oscillator alone */
    p.v[PV_E4] = 1;
    CHECK(voice_stereo(&p));
    p.v[PV_ENGINE] = ENGINE_WAVE;
    p.v[PV_E5] = 0;
    CHECK(!voice_stereo(&p));                           /* no oscillator 2 */
    p.v[PV_E5] = 1;
    CHECK(voice_stereo(&p));
    stereo_packet(&p, ENGINE_FM);
    p.v[PV_E0] = 0;
    p.v[PV_STEREO] = 127;
    stereo_measure(&p, &l, &r, &correlation);
    CHECK(correlation > 0.9999);
    memset(stereo_cost, 0, sizeof stereo_cost);
    memset(cost_effect, 0, sizeof cost_effect);
    stereo_cost[ENGINE_WAVE] = 500;
    voice_cost[ENGINE_WAVE] = 1000;
    cost_effect[COST_FILTER] = 10;
    cost_effect[COST_SHAPER] = 20;
    cost_effect[COST_DRIVE] = 40;
    cost_effect[COST_CRUSH] = 80;
    cost_effect[COST_RATE] = 160;
    sound_init(&p, 0);
    p.v[PV_ENGINE] = ENGINE_WAVE;
    p.v[PV_FLT_TYPE] = FLT_OFF;
    p.v[PV_SHAPER] = p.v[PV_DRIVE] = p.v[PV_CRUSH] = p.v[PV_RATE] = 0;
    for (alg = 0; alg < MOD_SLOTS; alg++)
        p.v[PV_SLOT_SRC0 + alg] = MOD_SRC_OFF;
    p.v[PV_E5] = 10;
    CHECK(voice_charge(&p) == 1000u);
    p.v[PV_STEREO] = 1;
    CHECK(voice_charge(&p) == 1500u);                   /* a wide voice takes more of the budget */
    p.v[PV_FLT_TYPE] = FLT_LP;
    CHECK(voice_charge(&p) == 1510u);                   /* and the second run of each effect it uses */
    p.v[PV_DRIVE] = 1;
    CHECK(voice_charge(&p) == 1550u);
    p.v[PV_DRIVE] = 0;
    p.v[PV_SLOT_SRC0] = MOD_SRC_LFO1;
    p.v[PV_SLOT_DST0] = MOD_DST_RATE;
    p.v[PV_SLOT_AMT0] = 30;
    CHECK(voice_charge(&p) == 1670u);                   /* a route can turn an effect on */
    p.v[PV_SLOT_AMT0] = 0;
    CHECK(voice_charge(&p) == 1510u);
    p.v[PV_E5] = 0;
    CHECK(voice_charge(&p) == 1000u);
    memset(cost_effect, 0, sizeof cost_effect);
    swarm_base_cost = 1000u;
    swarm_edge_cost = 256u;
    stereo_cost[ENGINE_SWARM] = 100u;
    sound_init(&p, 0);
    p.v[PV_ENGINE] = ENGINE_SWARM;
    p.v[PV_E4] = 64;
    {
        uint32_t edges = swarm_edges_q8(&p);
        CHECK(edges > 0u);
        CHECK(voice_charge(&p) == 1000u + edges);
        p.v[PV_STEREO] = 40;
        CHECK(voice_charge(&p) == 1000u + edges + 100u + edges);   /* each edge goes to both channels */
    }
    swarm_base_cost = swarm_edge_cost = 0;
    stereo_cost[ENGINE_SWARM] = 0;
    stereo_cost[ENGINE_WAVE] = 0;
    voice_cost[ENGINE_WAVE] = 0;
}

/* spread 0 at the mix gives what the mono render gives, in both channels */
static void test_stereo_keeps_the_signal(void)
{
    pkt_t p;
    int32_t mono[CTL], a[CTL], b[CTL];
    uint32_t block, i;
    {
        swarm_voice_t one, two;
        stereo_packet(&p, ENGINE_SWARM);
        swarm_start(&one, &p, 1234u);
        swarm_start(&two, &p, 1234u);
        for (block = 0; block < 500u; block++) {
            memset(mono, 0, sizeof mono);
            memset(a, 0, sizeof a);
            memset(b, 0, sizeof b);
            swarm_render(&one, &p, mono, 0, 0, 0);
            swarm_render(&two, &p, a, b, 0, 0);
            for (i = 0; block > 50u && i < CTL; i++) {
                CHECK(abs(mono[i] - a[i]) < 400);
                CHECK(a[i] == b[i]);
            }
        }
    }
    {
        wave_voice_t one, two;
        stereo_packet(&p, ENGINE_WAVE);
        p.v[PV_E7] = 0;
        wave_start(&one, &p, 1234u);
        wave_start(&two, &p, 1234u);
        for (block = 0; block < 200u; block++) {
            memset(mono, 0, sizeof mono);
            memset(a, 0, sizeof a);
            memset(b, 0, sizeof b);
            wave_render(&one, &p, mono, 0, 0, 0);
            wave_render(&two, &p, a, b, 0, 0);
            for (i = 0; i < CTL; i++) {
                CHECK(abs(mono[i] - a[i]) <= 3);
                CHECK(a[i] == b[i]);
            }
        }
    }
    {
        uint32_t alg;
        for (alg = FM_ALG_TWO_CARRIERS; alg < FM_ALGS; alg++) {
            fm_voice_t one, two;
            stereo_packet(&p, ENGINE_FM);
            p.v[PV_E0] = (int8_t)alg;
            fm_start(&one, &p);
            fm_start(&two, &p);
            for (block = 0; block < 200u; block++) {
                memset(mono, 0, sizeof mono);
                memset(a, 0, sizeof a);
                memset(b, 0, sizeof b);
                fm_render(&one, &p, mono, 0, 0, 16384, 0);
                fm_render(&two, &p, a, b, 0, 16384, 0);
                for (i = 0; block > 20u && i < CTL; i++) {
                    CHECK(abs(mono[i] - a[i]) < 8);
                    CHECK(a[i] == b[i]);
                }
            }
        }
    }
}

/* five tracks can claim the budget with waiting notes in one step: a higher trigger still sounds, in place of one */
static void test_voice_higher_trigger_takes_waiting_note(void)
{
    enum { COST = 1000, FIT = 3, HIGH_NOTE = 33, LOW_NOTE = 90, NORM_NOTE = 60 };
    pkt_t p;
    uint32_t saved_cost = voice_cost[ENGINE_FM], saved_budget = voice_budget, saved_dropped = voices_dropped, i, b, high = 0, low = 0, norm = 0, rendering = 0;
    memset(voices, 0, sizeof voices);
    voice_cost[ENGINE_FM] = COST;
    voice_budget = COST * FIT;
    sound_init(&p, 0);
    p.v[PV_ENGINE] = ENGINE_FM;
    p.v[PV_AMP_S] = 127;
    p.v[PV_PRIO] = PRIO_NORMAL;
    p.v[PV_NOTE] = NORM_NOTE;
    for (i = 0; i < FIT; i++)
        voice_trigger(&p, KEY_NONE, 0);
    for (b = 0; b < 300u; b++) {                        /* out of their attack */
        int32_t l[CTL] = {0}, r[CTL] = {0}, d[CTL] = {0}, w[CTL] = {0};
        voices_render(l, r, d, w);
    }
    for (i = 0; i < FIT; i++)
        voice_trigger(&p, KEY_NONE, 0);                 /* every voice now fades with a note waiting */
    CHECK(voices_dropped == saved_dropped);
    p.v[PV_NOTE] = NORM_NOTE + 1;
    voice_trigger(&p, KEY_NONE, 0);                     /* equal priority takes nothing */
    CHECK(voices_dropped == saved_dropped + 1u);
    for (i = 0; i < NVOICE; i++)
        if (voices[i].active && voices[i].has_next)
            CHECK(voices[i].next.v[PV_NOTE] == NORM_NOTE);
    p.v[PV_PRIO] = PRIO_LOW;
    p.v[PV_NOTE] = LOW_NOTE;
    voice_trigger(&p, KEY_NONE, 0);                     /* nor does lower */
    CHECK(voices_dropped == saved_dropped + 2u);
    p.v[PV_PRIO] = PRIO_HIGH;
    p.v[PV_NOTE] = HIGH_NOTE;
    voice_trigger(&p, KEY_NONE, 0);
    CHECK(voices_dropped == saved_dropped + 3u);        /* the note it replaces does not sound */
    for (b = 0; b < 4u; b++) {
        int32_t l[CTL] = {0}, r[CTL] = {0}, d[CTL] = {0}, w[CTL] = {0};
        voices_render(l, r, d, w);
    }
    for (i = 0; i < NVOICE; i++) {
        if (!voices[i].active)
            continue;
        rendering += voices[i].charge;
        high += voices[i].pkt.v[PV_NOTE] == HIGH_NOTE;
        low += voices[i].pkt.v[PV_NOTE] == LOW_NOTE;
        norm += voices[i].pkt.v[PV_NOTE] == NORM_NOTE;
    }
    CHECK(high == 1u && low == 0u && norm == FIT - 1u);
    CHECK(rendering <= voice_budget);
    voice_cost[ENGINE_FM] = saved_cost;
    voice_budget = saved_budget;
    memset(voices, 0, sizeof voices);
}

/* the waiting note with the lowest priority goes first */
static void test_voice_high_trigger_replaces_lowest_waiting(void)
{
    enum { LOW_NOTE = 90, NORM_NOTE = 60, HIGH_NOTE = 33 };
    pkt_t p;
    uint32_t saved_cost = voice_cost[ENGINE_FM], saved_budget = voice_budget, i, b, low = 0, norm = 0, high = 0;
    memset(voices, 0, sizeof voices);
    voice_cost[ENGINE_FM] = 1000;
    voice_budget = 2000;
    sound_init(&p, 0);
    p.v[PV_ENGINE] = ENGINE_FM;
    p.v[PV_AMP_S] = 127;
    p.v[PV_PRIO] = PRIO_LOW;
    for (i = 0; i < 2u; i++)
        voice_trigger(&p, KEY_NONE, 0);
    for (b = 0; b < 300u; b++) {
        int32_t l[CTL] = {0}, r[CTL] = {0}, d[CTL] = {0}, w[CTL] = {0};
        voices_render(l, r, d, w);
    }
    p.v[PV_NOTE] = LOW_NOTE;
    voice_trigger(&p, KEY_NONE, 0);
    p.v[PV_PRIO] = PRIO_NORMAL;
    p.v[PV_NOTE] = NORM_NOTE;
    voice_trigger(&p, KEY_NONE, 0);
    p.v[PV_PRIO] = PRIO_HIGH;
    p.v[PV_NOTE] = HIGH_NOTE;
    voice_trigger(&p, KEY_NONE, 0);
    for (b = 0; b < 4u; b++) {
        int32_t l[CTL] = {0}, r[CTL] = {0}, d[CTL] = {0}, w[CTL] = {0};
        voices_render(l, r, d, w);
    }
    for (i = 0; i < NVOICE; i++)
        if (voices[i].active) {
            low += voices[i].pkt.v[PV_NOTE] == LOW_NOTE;
            norm += voices[i].pkt.v[PV_NOTE] == NORM_NOTE;
            high += voices[i].pkt.v[PV_NOTE] == HIGH_NOTE;
        }
    CHECK(low == 0u && norm == 1u && high == 1u);
    voice_cost[ENGINE_FM] = saved_cost;
    voice_budget = saved_budget;
    memset(voices, 0, sizeof voices);
}

/* a costlier high trigger replaces as many waiting notes as it needs */
static void test_voice_high_trigger_replaces_several(void)
{
    pkt_t p;
    uint32_t saved_fm = voice_cost[ENGINE_FM], saved_wave = voice_cost[ENGINE_WAVE], saved_budget = voice_budget, i, b, total = 0, high = 0;
    memset(voices, 0, sizeof voices);
    voice_cost[ENGINE_FM] = 1000;
    voice_cost[ENGINE_WAVE] = 2000;
    voice_budget = 2000;
    sound_init(&p, 0);
    p.v[PV_ENGINE] = ENGINE_FM;
    p.v[PV_AMP_S] = 127;
    p.v[PV_PRIO] = PRIO_NORMAL;
    for (i = 0; i < 2u; i++)
        voice_trigger(&p, KEY_NONE, 0);
    for (b = 0; b < 300u; b++) {
        int32_t l[CTL] = {0}, r[CTL] = {0}, d[CTL] = {0}, w[CTL] = {0};
        voices_render(l, r, d, w);
    }
    for (i = 0; i < 2u; i++)
        voice_trigger(&p, KEY_NONE, 0);
    p.v[PV_ENGINE] = ENGINE_WAVE;
    p.v[PV_PRIO] = PRIO_HIGH;
    p.v[PV_NOTE] = 33;
    voice_trigger(&p, KEY_NONE, 0);
    for (b = 0; b < 4u; b++) {
        int32_t l[CTL] = {0}, r[CTL] = {0}, d[CTL] = {0}, w[CTL] = {0};
        voices_render(l, r, d, w);
    }
    for (i = 0; i < NVOICE; i++)
        if (voices[i].active) {
            total += voices[i].charge;
            high += voices[i].pkt.v[PV_NOTE] == 33;
        }
    CHECK(high == 1u);
    CHECK(total <= voice_budget);
    voice_cost[ENGINE_FM] = saved_fm;
    voice_cost[ENGINE_WAVE] = saved_wave;
    voice_budget = saved_budget;
    memset(voices, 0, sizeof voices);
}

/* a note that waits for a fade counts at once: every voice rendering at once must fit */
static void test_voice_budget_counts_fades(void)
{
    enum { COST = 1800, FIT = 6 };
    pkt_t p;
    uint32_t round, t, b, i;
    uint32_t saved_cost = voice_cost[ENGINE_WAVE], saved_wide = stereo_cost[ENGINE_WAVE], saved_budget = voice_budget, saved_dropped = voices_dropped;
    memset(voices, 0, sizeof voices);
    voice_cost[ENGINE_WAVE] = COST;
    stereo_cost[ENGINE_WAVE] = 0;
    voice_budget = COST * FIT + 100u;
    sound_init(&p, 0);
    p.v[PV_ENGINE] = ENGINE_WAVE;
    p.v[PV_AMP_S] = 127;
    for (round = 0; round < 40u; round++) {
        for (t = 0; t < FIT; t++) {
            p.v[PV_NOTE] = (int8_t)(40 + t);
            voice_trigger(&p, KEY_NONE, 0);
            {
                uint32_t rendering = 0;
                for (i = 0; i < NVOICE; i++)
                    rendering += voices[i].active ? voices[i].charge : 0u;
                CHECK(rendering <= voice_budget);
            }
        }
        for (b = 0; b < 12u; b++) {
            int32_t l[CTL] = {0}, r[CTL] = {0}, d[CTL] = {0}, w[CTL] = {0};
            uint32_t rendering = 0;
            voices_render(l, r, d, w);
            for (i = 0; i < NVOICE; i++)
                rendering += voices[i].active ? voices[i].charge : 0u;
            CHECK(rendering <= voice_budget);
        }
    }
    CHECK(voices_dropped == saved_dropped);             /* the notes wait for a fade, and none is lost */
    CHECK(voices_active() == FIT);
    voice_cost[ENGINE_WAVE] = saved_cost;
    stereo_cost[ENGINE_WAVE] = saved_wide;
    voice_budget = saved_budget;
    memset(voices, 0, sizeof voices);
}

/* a side has one oscillator from each octave, and its detunes near cancel */
static void test_stereo_swarm_sides(void)
{
    uint32_t side, i, octave_mode = 1u;
    for (side = SWARM_LEFT; side <= SWARM_RIGHT; side++) {
        int32_t detune = 0, count = 0, low = 0, mid = 0, high = 0;
        for (i = 0; i < SWARM_OSCS; i++) {
            if (SWARM_SIDE[i] != side)
                continue;
            detune += SWARM_DETUNE[i];
            count++;
            low += SWARM_OCTAVE_Q8[octave_mode][i] == 128;
            mid += SWARM_OCTAVE_Q8[octave_mode][i] == 256;
            high += SWARM_OCTAVE_Q8[octave_mode][i] == 512;
        }
        CHECK(count == 3 && low == 1 && mid == 1 && high == 1);
        CHECK(abs(detune) < 1500);
    }
    CHECK(SWARM_SIDE[SWARM_CENTER] == SWARM_BOTH);
}

/* the sends hear the mean of the channels, and the follower hears the louder */
static void test_stereo_sends_and_follower(void)
{
    pkt_t p;
    chain_t c;
    int32_t x[CTL], y[CTL], l[CTL] = {0}, r[CTL] = {0}, d[CTL] = {0}, w[CTL] = {0};
    uint32_t b, i;
    sound_init(&p, 0);
    p.v[PV_FLT_TYPE] = FLT_OFF;
    p.v[PV_AMP_A] = 0;
    p.v[PV_AMP_S] = 127;
    p.v[PV_SEND_DELAY] = 127;
    p.v[PV_SEND_REVERB] = 0;
    p.v[PV_SEND_FOLLOW] = 127;
    chain_start(&c, &p);
    for (b = 0; b < 300u; b++) {
        for (i = 0; i < CTL; i++) {
            x[i] = 0;
            y[i] = 12000;
            l[i] = r[i] = d[i] = w[i] = 0;
        }
        follower_input = 0;
        chain_render(&c, &p, x, y, l, r, d, w);
    }
    CHECK(l[CTL - 1u] == 0 && r[CTL - 1u] > 1000);
    CHECK(abs(d[CTL - 1u] - ((r[CTL - 1u] * 254) >> 9)) < 8);   /* the mean of 0 and r, at the send's Q8 */
    CHECK(follower_input > 8000);                               /* the right channel alone drives it */
}

/* each channel has its own filter memory: shared, the two would write over each other */
static void test_stereo_filters_each_channel(void)
{
    pkt_t p;
    chain_t both, one, same;
    int32_t sine_a[CTL], sine_b[CTL], alone[CTL];
    uint32_t b, i, t = 0;
    sound_init(&p, 0);
    p.v[PV_FLT_TYPE] = FLT_LP;
    p.v[PV_FLT_CUT] = 60;
    p.v[PV_AMP_S] = 127;
    p.v[PV_SEND_DELAY] = p.v[PV_SEND_REVERB] = 0;
    chain_start(&both, &p);
    chain_start(&one, &p);
    p.v[PV_DRIVE] = 40;
    p.v[PV_SHAPER] = 40;
    p.v[PV_CRUSH] = 40;
    p.v[PV_RATE] = 40;
    chain_start(&both, &p);
    chain_start(&one, &p);
    chain_start(&same, &p);
    for (b = 0; b < 200u; b++) {
        int32_t l[CTL] = {0}, r[CTL] = {0}, ol[CTL] = {0}, or_[CTL] = {0}, d[CTL] = {0}, w[CTL] = {0}, sl[CTL] = {0}, sr[CTL] = {0}, same_l[CTL], same_r[CTL];
        for (i = 0; i < CTL; i++, t++) {
            sine_a[i] = (int32_t)(8000.0 * sin(t * 0.01));
            sine_b[i] = (int32_t)(8000.0 * sin(t * 0.37));
            alone[i] = sine_a[i];
            same_l[i] = same_r[i] = sine_b[i];
        }
        chain_render(&both, &p, sine_a, sine_b, l, r, d, w);
        chain_render(&one, &p, alone, 0, ol, or_, d, w);
        chain_render(&same, &p, same_l, same_r, sl, sr, d, w);
        for (i = 0; i < CTL; i++) {
            CHECK(l[i] == ol[i]);                       /* left does not hear right */
            CHECK(sl[i] == sr[i]);                      /* and right has every stage left has */
        }
    }
}

/* an impulse's repeats, by the sample each lands on */
static void delay_impulse(delay_t *d, uint32_t samples, int32_t feedback, int32_t width, int32_t *left, int32_t *right, uint32_t count)
{
    uint32_t b, i;
    memset(d, 0, sizeof *d);
    for (b = 0; b * CTL < count; b++) {
        int32_t in[CTL] = {0}, l[CTL] = {0}, r[CTL] = {0};
        in[0] = b ? 0 : 16000;
        delay_block(d, in, l, r, samples, feedback, width);
        for (i = 0; i < CTL; i++) {
            left[b * CTL + i] = l[i];
            right[b * CTL + i] = r[i];
        }
    }
}

static int32_t delay_peak_near(const int32_t *x, uint32_t at)
{
    int32_t peak = 0;
    uint32_t i;
    for (i = at - 4u; i < at + 8u; i++)
        peak = abs(x[i]) > peak ? abs(x[i]) : peak;
    return peak;
}

static void test_delay_ping_pong(void)
{
    enum { HOP = 1000, COUNT = 8000 };
    static delay_t d;
    static int32_t l[COUNT + CTL], r[COUNT + CTL];
    int32_t first, second, third;
    delay_impulse(&d, HOP, 100, 127, l, r, COUNT);
    first = delay_peak_near(l, HOP);
    second = delay_peak_near(r, 2u * HOP);
    third = delay_peak_near(l, 3u * HOP);
    CHECK(first > 20000);                               /* -3 dB would hold the power of a repeat in both channels */
    CHECK(second > first / 2 && second < first);        /* a repeat loses the same each hop */
    CHECK(third > second / 2 && third < second);
    CHECK(delay_peak_near(r, HOP) == 0);                /* each lands on one side only */
    CHECK(delay_peak_near(l, 2u * HOP) == 0);
    CHECK(delay_peak_near(r, 3u * HOP) == 0);
    delay_impulse(&d, HOP, 100, 64, l, r, COUNT);
    CHECK(delay_peak_near(r, HOP) > 1000 && delay_peak_near(r, HOP) < delay_peak_near(l, HOP));
    CHECK(delay_peak_near(l, 2u * HOP) > 1000 && delay_peak_near(l, 2u * HOP) < delay_peak_near(r, 2u * HOP));
    delay_impulse(&d, HOP, 100, 0, l, r, COUNT);
    {
        uint32_t i;
        for (i = 0; i < COUNT; i++)
            CHECK(l[i] == r[i]);
    }
    CHECK(delay_peak_near(l, HOP) == 16000);
    {
        static int32_t long_l[50000 + CTL], long_r[50000 + CTL];
        delay_impulse(&d, 30000, 100, 127, long_l, long_r, 50000);
        CHECK(delay_peak_near(long_l, DELAY_HOP_MAX) > 20000);      /* the hop stops at half of the line */
        CHECK(delay_peak_near(long_r, 2u * DELAY_HOP_MAX) > 1000);
    }
}

/* the repeats of a sound written just before the line wraps land on time */
static void test_delay_wraps(void)
{
    enum { HOP = 1000, BEFORE_WRAP = 200 };
    static delay_t d;
    uint32_t total = 0, b, i, at = DELAY_LEN - BEFORE_WRAP;
    int32_t first = 0, second = 0, stray = 0;
    memset(&d, 0, sizeof d);
    for (b = 0; total < at + 3u * HOP; b++) {
        int32_t in[CTL] = {0}, l[CTL] = {0}, r[CTL] = {0};
        for (i = 0; i < CTL; i++)
            in[i] = total + i == at ? 16000 : 0;
        delay_block(&d, in, l, r, HOP, 100, 127);
        for (i = 0; i < CTL; i++, total++) {
            if (total == at + HOP)
                first = l[i];
            else if (total == at + 2u * HOP)
                second = r[i];
            else if (total < at + 2u * HOP && total != at + HOP)
                stray = abs(l[i]) > stray ? abs(l[i]) : stray;
        }
    }
    CHECK(first > 20000);
    CHECK(second != 0);
    CHECK(stray == 0);
}

/* width 0 is the delay it always was: one line, one read, damping and feedback once per repeat */
static void test_delay_width_0_matches_one_read(void)
{
    enum { HOP = 1500 };
    static delay_t d;
    static int16_t line[DELAY_LEN];
    uint32_t w = 0, b, i;
    int32_t lp = 0, fb = 90 * DELAY_FB_MAX / 127, worst = 0;
    memset(line, 0, sizeof line);
    memset(&d, 0, sizeof d);
    for (b = 0; b < 2000u; b++) {
        int32_t in[CTL], l[CTL] = {0}, r[CTL] = {0};
        for (i = 0; i < CTL; i++)
            in[i] = b < 100u ? (int32_t)(9000.0 * sin((b * CTL + i) * 0.03)) : 0;
        delay_block(&d, in, l, r, HOP, 90, 0);
        for (i = 0; i < CTL; i++) {
            int32_t x = line[(w + DELAY_LEN - HOP) % DELAY_LEN], error;
            lp += ((x - lp) * (32767 - DELAY_DAMP)) >> 15;
            line[w] = (int16_t)clamp(in[i] + ((lp * fb) >> 15), -32768, 32767);
            w = (w + 1u) % DELAY_LEN;
            error = abs(l[i] - x);
            worst = error > worst ? error : worst;
        }
    }
    CHECK(worst <= 8);
}

static void test_stop_and_start(void)
{
    seq_t q;
    track_pat_t *t = &patterns[0].track[0];
    uint32_t i;
    reset();
    seq_init(&q, 120);
    t->steps[0].on = 1;
    t->steps[3].on = 1;
    t->steps[0].chance = t->steps[3].chance = 100;
    seq_block(&q, patterns, q.samples_per_step * 2u + 1u, stop_emit);
    CHECK(q.pos[0] == 2);
    seq_stop(&q);
    CHECK(q.pos[0] == 0 && q.count == 0 && q.sample_acc == 0);
    stop_emits = 0;
    for (i = 0; i < 40u; i++)
        seq_block(&q, patterns, q.samples_per_step, stop_emit);
    CHECK(stop_emits == 0 && q.pos[0] == 0);            /* stopped: no triggers and no movement */
    q.next = 2;
    seq_start(&q);
    CHECK(q.playing == 2);                              /* a pattern chosen while stopped does not wait */
    patterns[2].track[0].steps[0].on = 1;
    seq_block(&q, patterns, 1, stop_emit);
    CHECK(stop_emits == 1 && stop_last_step == 0 && q.pos[0] == 0);   /* step 1 sounds on the first block */
    seq_block(&q, patterns, 1, stop_emit);
    CHECK(stop_emits == 1);                             /* and once */
}

static void test_engine_memory(void)
{
    sound_t s;
    engine_memory_t m;
    memset(&m, 0, sizeof m);
    sound_init(&s, 3);
    s.v[PV_E4] = 99;
    sound_set_engine(&s, &m, ENGINE_FM);
    CHECK(s.v[PV_E4] == 99);                            /* no change of engine, no change of values */
    sound_set_engine(&s, &m, ENGINE_WAVE);
    CHECK(s.v[PV_ENGINE] == ENGINE_WAVE && s.v[PV_E4] == ENGINE_DEFAULTS[ENGINE_WAVE][4] && s.v[PV_E1] == 64);
    s.v[PV_E1] = 10;
    sound_set_engine(&s, &m, ENGINE_PERC);
    CHECK(s.v[PV_E0] == PERC_SKIN);
    sound_set_engine(&s, &m, ENGINE_FM);
    CHECK(s.v[PV_E4] == 99 && s.v[PV_E0] == 3);         /* sweeping back restores */
    sound_set_engine(&s, &m, ENGINE_WAVE);
    CHECK(s.v[PV_E1] == 10);
}

static void test_voices_silence(void)
{
    pkt_t p;
    int32_t l[CTL] = {0}, r[CTL] = {0}, d[CTL] = {0}, rv[CTL] = {0};
    uint32_t i;
    sound_init(&p, 0);
    for (i = 0; i < NVOICE; i++)
        voices[i].active = 0;
    voice_budget = 0xFFFFFFFFu;
    voice_trigger(&p, KEY_NONE, 0);
    CHECK(voices_active() == 1);
    voices_silence();
    for (i = 0; i < 100u; i++)
        voices_render(l, r, d, rv);
    CHECK(voices_active() == 0);
}

static void test_step_copy(void)
{
    track_pat_t *t = &patterns[0].track[0];
    uint32_t i;
    reset();
    t->steps[8].on = 1;
    t->steps[8].note = 70;
    lock_set(t, 8, PV_PAN, 1);
    lock_set(t, 8, PV_FLT_CUT, 2);
    lock_set(t, 8, PV_FLT_RES, 3);
    lock_set(t, 4, PV_DRIVE, 9);                        /* the target's old lock goes */
    CHECK(step_copy(t, 8, 4) == 0);                     /* a target before the source moves the source's locks */
    CHECK(step_lock_count(t, 4) == 3 && step_lock_count(t, 8) == 3);
    CHECK(!step_value_locked(t, 4, PV_DRIVE));
    CHECK(step_value_locked(t, 4, PV_PAN) && step_value_locked(t, 4, PV_FLT_CUT) && step_value_locked(t, 4, PV_FLT_RES));
    CHECK(t->steps[4].on == 1 && t->steps[4].note == 70);
    CHECK(step_copy(t, 8, 12) == 0 && step_lock_count(t, 12) == 3);
    CHECK(step_copy(t, 8, 8) == 0 && step_lock_count(t, 8) == 3);   /* onto itself: no change */
    for (i = t->nlock; i < TRACK_LOCKS; i++)
        (void)lock_set(t, 20u + i / 40u, i % 40u, 1);
    CHECK(t->nlock == TRACK_LOCKS);
    CHECK(step_copy(t, 8, 14) != 0);                    /* no room: the caller is told */
}

static void test_step_clear_locks(void)
{
    track_pat_t *t = &patterns[0].track[0];
    reset();
    t->steps[5].on = 1;
    t->steps[5].note = 40;
    t->steps[5].vel = 40;
    t->steps[5].len = 2;
    t->steps[5].sound = 3;
    t->steps[5].chance = 50;
    lock_set(t, 5, PV_PAN, 1);
    lock_set(t, 6, PV_PAN, 2);
    CHECK(step_value_locked(t, 5, PV_NOTE) && step_value_locked(t, 5, PV_PAN) && !step_value_locked(t, 5, PV_FLT_CUT));
    CHECK(!step_value_locked(t, 4, PV_PAN));
    step_clear_locks(t, 5);
    CHECK(!step_locked(t, 5) && !step_value_locked(t, 5, PV_NOTE) && !step_value_locked(t, 5, PV_PAN));
    CHECK(t->steps[5].on == 1);                         /* the trigger stays */
    CHECK(step_lock_count(t, 6) == 1);                  /* a neighbour's lock stays */
}

static void test_step_locked(void)
{
    track_pat_t *t = &patterns[0].track[0];
    reset();
    CHECK(!step_locked(t, 5));
    lock_set(t, 5, PV_FLT_CUT, 10);
    CHECK(step_locked(t, 5));
    CHECK(!step_locked(t, 4) && !step_locked(t, 6));    /* a neighbour's lock does not count */
    lock_clear_step(t, 5);
    CHECK(!step_locked(t, 5));
    t->steps[5].sound = 3;
    CHECK(step_locked(t, 5));                           /* a sound lock counts */
    CHECK(step_lock_count(t, 5) == 0);
    t->steps[7].note = 61;
    CHECK(step_locked(t, 7));                           /* a changed note counts */
    lock_set(t, 9, PV_FLT_CUT, 10);
    lock_set(t, 9, PV_PAN, 10);
    CHECK(step_lock_count(t, 9) == 2);
    t->steps[11].chance = 50;
    CHECK(step_locked(t, 11));                          /* a changed chance counts */
}

static void test_sound_use_count(void)
{
    static pattern_t pats[2];
    uint32_t p, k;
    for (p = 0; p < 2u; p++)
        for (k = 0; k < NTRK; k++)
            track_init(&pats[p].track[k], 0);
    CHECK(sound_use_count(pats, 2, 0) == 2u * NTRK);
    CHECK(sound_use_count(pats, 2, 17) == 0);
    pats[0].track[1].base = 17;
    pats[1].track[2].steps[9].sound = 17;
    CHECK(sound_use_count(pats, 2, 17) == 2);
    pats[0].track[1].steps[3].sound = 17;               /* the same track twice counts once */
    CHECK(sound_use_count(pats, 2, 17) == 2);
    CHECK(sound_use_count(pats, 2, 0) == 2u * NTRK - 1u);
}

static void test_tracks_free_run(void)
{
    seq_t q;
    uint32_t i;
    reset();
    for (i = 2; i < NTRK; i++)
        patterns[0].track[i].length = 1;
    patterns[0].track[0].length = 4;
    patterns[0].track[1].length = 3;
    seq_init(&q, 120);
    for (i = 0; i < 4; i++)
        seq_advance(&q, patterns, emit_track);
    CHECK(q.count == 0);                                /* the longest track completed */
    CHECK(q.pos[0] == 0 && q.pos[1] == 1);              /* the shorter track did not restart */
    for (i = 0; i < 8; i++)
        seq_advance(&q, patterns, emit_track);
    CHECK(q.pos[0] == 0 && q.pos[1] == 0);              /* 12 steps: the two line up again */
}

static void test_switch_at_longest(void)
{
    seq_t q;
    uint32_t i;
    reset();
    patterns[0].track[0].length = 16;
    patterns[0].track[1].length = 13;
    patterns[1].track[0].steps[0].on = 1;
    patterns[0].track[0].steps[15].on = 1;
    seq_init(&q, 120);
    for (i = 0; i < 5; i++)
        seq_advance(&q, patterns, emit_track);
    q.next = 1;
    for (i = 5; i < 15; i++) {
        seq_advance(&q, patterns, emit_track);
        CHECK(q.playing == 0);
    }
    CHECK(q.pos[0] == 15 && q.pos[1] == 2);             /* the shorter track wrapped and keeps going */
    seq_advance(&q, patterns, emit_track);
    CHECK(q.playing == 1);
    CHECK(q.pos[0] == 0 && q.pos[1] == 0);              /* every track from step 1 */
    CHECK(last_track == &patterns[1].track[0]);         /* the new pattern's first step fires at once */
}

static void test_play_counter(void)
{
    seq_t q;
    uint32_t i;
    reset();
    patterns[0].track[1].length = 13;                   /* a shorter track does not end a play */
    seq_init(&q, 120);
    for (i = 0; i < 15; i++)
        seq_advance(&q, patterns, emit_track);
    CHECK(q.plays == 0 && q.count == 15);
    seq_advance(&q, patterns, emit_track);
    CHECK(q.plays == 1 && q.count == 0);
    for (i = 0; i < 16u * (SEQ_PLAYS - 1u); i++)
        seq_advance(&q, patterns, emit_track);
    CHECK(q.plays == 0);                                /* wrapped */
    for (i = 0; i < 16u * 3u; i++)
        seq_advance(&q, patterns, emit_track);
    CHECK(q.plays == 3);
    q.next = 1;
    for (i = 0; i < 16; i++)
        seq_advance(&q, patterns, emit_track);
    CHECK(q.playing == 1 && q.plays == 0);              /* a committed pattern starts its own count */
}

static void test_played_step_skips_once(void)
{
    seq_t q;
    uint32_t i;
    reset();
    patterns[0].track[0].steps[1].on = 1;
    seq_init(&q, 120);
    q.skip[0] = 2;                                      /* step 1 (index) was played just before the playhead reached it */
    last_track = 0;
    seq_advance(&q, patterns, emit_track);
    CHECK(q.pos[0] == 1 && last_track == 0);
    for (i = 0; i < 16; i++)
        seq_advance(&q, patterns, emit_track);
    CHECK(q.pos[0] == 1 && last_track == &patterns[0].track[0]);      /* only once */
}

static void test_fired_step(void)
{
    seq_t q;
    uint32_t i;
    reset();
    patterns[0].track[0].steps[2].on = 1;
    patterns[1].track[0].steps[5].on = 1;
    seq_init(&q, 120);
    CHECK(q.fired[0] == SEQ_NOT_FIRED);
    for (i = 0; i < 2; i++)
        seq_advance(&q, patterns, emit_track);
    CHECK(q.fired[0] == 2);
    for (i = 0; i < 5; i++)
        seq_advance(&q, patterns, emit_track);
    CHECK(q.pos[0] == 7 && q.fired[0] == 2);            /* steps that are off leave the last trigger */
    q.next = 1;
    while (q.playing == 0)
        seq_advance(&q, patterns, emit_track);
    CHECK(q.fired[0] == SEQ_NOT_FIRED);                 /* the old pattern's step means nothing in the new one */
}

static void test_nearest_step(void)
{
    seq_t q;
    seq_init(&q, 120);
    q.pos[0] = 15;
    q.sample_acc = q.samples_per_step / 2u - 1u;
    CHECK(seq_nearest_step(&q, 0, 16) == 15);
    q.sample_acc = q.samples_per_step / 2u + 1u;
    CHECK(seq_nearest_step(&q, 0, 16) == 0);            /* wraps on the track's length */
    CHECK(seq_nearest_step(&q, 0, 32) == 16);
}

static void test_overdub_knobs(void)
{
    static dub_t d;
    track_pat_t *t = &patterns[0].track[0];
    int32_t v;
    uint32_t i;
    pkt_t p;
    reset();
    memset(&d, 0, sizeof d);
    t->steps[1].on = 1;
    t->steps[2].on = 1;
    dub_set(&d, PV_FLT_CUT, 40, 1000);
    CHECK(dub_value(&d, PV_FLT_CUT, &v) && v == 40);
    dub_write(&d, t, 0);                                /* a step that is off takes no lock */
    CHECK(t->nlock == 0);
    dub_expire(&d, 1100, 150, 0);
    dub_write(&d, t, 1);
    packet_resolve(&p, pool, t, 1);
    CHECK(p.v[PV_FLT_CUT] == 40);
    dub_expire(&d, 1150, 150, 0);                       /* still for the window: it stops writing */
    CHECK(d.n == 0);
    CHECK(!dub_value(&d, PV_FLT_CUT, &v));
    dub_write(&d, t, 2);
    CHECK(step_lock_count(t, 2) == 0);
    dub_set(&d, PV_FLT_RES, 7, 2000);
    dub_expire(&d, 9000, 150, 1);                       /* REC keeps a still knob writing */
    CHECK(d.n == 1);
    dub_write(&d, t, 2);
    CHECK(step_lock_count(t, 2) == 1);
    dub_expire(&d, 9001, 150, 0);                       /* and its release ends that */
    CHECK(d.n == 0);
    dub_set(&d, PV_NOTE, 70, 3000);                     /* note has its field in the step, as a held step's does */
    dub_write(&d, t, 2);
    CHECK(t->steps[2].note == 70 && step_lock_count(t, 2) == 1);
    d.n = 0;
    for (i = 0; i < DUB_KNOBS + 1u; i++)
        dub_set(&d, PV_E0 + i, 1, 4000u + i);           /* the oldest gives way */
    CHECK(d.n == DUB_KNOBS);
    CHECK(!dub_value(&d, PV_E0, &v) && dub_value(&d, PV_E0 + DUB_KNOBS, &v));
    reset();
    t->steps[0].on = 1;
    t->nlock = TRACK_LOCKS;
    d.n = 0;
    d.full = 0;
    dub_set(&d, PV_E0, 1, 0);
    dub_write(&d, t, 0);
    CHECK(d.full);
}

static void test_chance(void)
{
    seq_t q;
    uint32_t i;
    reset();
    memset(nfired, 0, sizeof nfired);
    patterns[0].track[0].steps[0].on = 1;
    patterns[0].track[0].steps[0].chance = 0;
    patterns[0].track[0].length = 1;
    seq_init(&q, 120);
    for (i = 0; i < 100; i++)
        seq_advance(&q, patterns, emit_log);
    CHECK(nfired[0] == 0);
    patterns[0].track[0].steps[0].chance = 100;
    for (i = 0; i < 100; i++)
        seq_advance(&q, patterns, emit_log);
    CHECK(nfired[0] == 100);
}

static void test_tempo(void)
{
    seq_t q;
    uint32_t i;
    reset();
    seq_init(&q, 120);
    CHECK(q.samples_per_step == 5512u);                 /* a 16th at 120 BPM */
    for (i = 0; i < 5512u / CTL; i++)
        seq_block(&q, patterns, CTL, emit_track);
    CHECK(q.count == 0);
    seq_block(&q, patterns, CTL, emit_track);
    CHECK(q.count == 1);
}

static void packet_default(pkt_t *p)
{
    sound_init(p, 0);
}

static int32_t peak(const int32_t *x)
{
    int32_t m = 0;
    uint32_t i;
    for (i = 0; i < CTL; i++)
        if ((x[i] < 0 ? -x[i] : x[i]) > m)
            m = x[i] < 0 ? -x[i] : x[i];
    return m;
}

static int32_t render_tone(chain_t *c, const pkt_t *p, int32_t a, int32_t *l, int32_t *r)
{
    int32_t x[CTL];
    uint32_t i;
    for (i = 0; i < CTL; i++) {
        x[i] = i & 1u ? -a : a;
        l[i] = r[i] = 0;
    }
    {
        int32_t delay[CTL] = {0}, reverb[CTL] = {0};
        return chain_render(c, p, x, 0, l, r, delay, reverb);
    }
}

static void test_envelope(void)
{
    pkt_t p;
    chain_t c;
    int32_t l[CTL], r[CTL], first, later;
    uint32_t n;
    packet_default(&p);
    p.v[PV_AMP_A] = 60;
    chain_start(&c, &p);
    render_tone(&c, &p, 10000, l, r);
    first = peak(l);
    for (n = 0; n < 3000u && c.amp_env.stage == ENV_ATTACK; n++)
        render_tone(&c, &p, 10000, l, r);
    later = peak(l);
    CHECK(c.amp_env.stage != ENV_ATTACK);
    CHECK(first < later / 10);                          /* an attack starts quiet */
    for (n = 0; n < 20000u && c.amp_env.stage == ENV_DECAY; n++)
        render_tone(&c, &p, 10000, l, r);
    CHECK(c.amp_env.stage == ENV_SUSTAIN);
    CHECK(render_tone(&c, &p, 10000, l, r) == 1);
    chain_release(&c);
    for (n = 0; n < 20000u && render_tone(&c, &p, 10000, l, r); n++)
        ;
    CHECK(n < 20000u);                                  /* a release ends */
    CHECK(c.amp_env.stage == ENV_DONE);
    CHECK(c.amp_env.env == 0);
    p.v[PV_AMP_A] = 0;
    p.v[PV_AMP_D] = 0;
    p.v[PV_AMP_S] = 0;
    chain_start(&c, &p);
    for (n = 0; n < 200u; n++)
        render_tone(&c, &p, 10000, l, r);
    CHECK(peak(l) == 0);                                /* sustain 0 decays to silence */
}

static void test_pan_and_level(void)
{
    pkt_t p;
    chain_t c;
    int32_t l[CTL], r[CTL], full;
    uint32_t n;
    packet_default(&p);
    chain_start(&c, &p);
    for (n = 0; n < 8u; n++)
        render_tone(&c, &p, 10000, l, r);
    full = peak(l);
    CHECK(full > 0 && peak(r) == full);
    p.v[PV_PAN] = -64;
    chain_start(&c, &p);
    for (n = 0; n < 8u; n++)
        render_tone(&c, &p, 10000, l, r);
    CHECK(peak(r) == 0 && peak(l) == full);
    p.v[PV_PAN] = 0;
    p.v[PV_LEVEL] = 60;
    chain_start(&c, &p);
    for (n = 0; n < 8u; n++)
        render_tone(&c, &p, 10000, l, r);
    CHECK(peak(l) < full / 2);
}

static void test_filter(void)
{
    pkt_t p;
    chain_t c;
    int32_t l[CTL], r[CTL], open, lp, hp;
    uint32_t n;
    packet_default(&p);
    chain_start(&c, &p);
    for (n = 0; n < 30u; n++)
        render_tone(&c, &p, 10000, l, r);
    open = peak(l);
    p.v[PV_FLT_TYPE] = FLT_LP;
    p.v[PV_FLT_CUT] = 20;
    chain_start(&c, &p);
    for (n = 0; n < 30u; n++)
        render_tone(&c, &p, 10000, l, r);
    lp = peak(l);
    CHECK(lp < open / 20);                              /* the tone is at half the sample rate */
    p.v[PV_FLT_TYPE] = FLT_HP;
    chain_start(&c, &p);
    for (n = 0; n < 30u; n++)
        render_tone(&c, &p, 10000, l, r);
    hp = peak(l);
    CHECK(hp > open / 2);
}

static void test_effects(void)
{
    pkt_t p;
    chain_t c;
    int32_t x[CTL];
    uint32_t i;
    packet_default(&p);
    chain_start(&c, &p);
    p.v[PV_CRUSH] = 127;
    for (i = 0; i < CTL; i++)
        x[i] = (int32_t)i * 100 - 1600;
    chain_effects(&c, &c.path[0], &p, x);
    for (i = 0; i < CTL; i++)
        CHECK((x[i] & 8191) == 0);                      /* 13 low bits gone */
    packet_default(&p);
    p.v[PV_RATE] = 30;
    chain_start(&c, &p);
    for (i = 0; i < CTL; i++)
        x[i] = (int32_t)i;
    chain_effects(&c, &c.path[0], &p, x);
    CHECK(x[0] == 0 && x[10] == 0 && x[11] == 11);      /* held 11 samples */
    packet_default(&p);
    p.v[PV_DRIVE] = 127;
    chain_start(&c, &p);
    for (i = 0; i < CTL; i++)
        x[i] = 30000;
    chain_effects(&c, &c.path[0], &p, x);
    CHECK(x[0] <= TANH_Q15[256]);
    packet_default(&p);
    p.v[PV_SHAPER] = 127;
    chain_start(&c, &p);
    for (i = 0; i < CTL; i++)
        x[i] = i & 1u ? 60000 : -60000;
    chain_effects(&c, &c.path[0], &p, x);
    CHECK(peak(x) <= 32767);
}

#define PLOT_W 150u
#define PLOT_HOLD 30u
static uint8_t plot_y[PLOT_W], plot_x_out[PLOT_W + 4u], plot_y_out[PLOT_W + 4u];

/* draws the envelope and checks what any envelope's picture must show */
static uint32_t plot_env(int32_t a, int32_t d, int32_t sus, int32_t r, env_curve_t *c)
{
    uint32_t m, i, top = 0, flat = 0;
    env_curve(c, plot_y, PLOT_W, a, d, sus, r);
    CHECK(c->n > 0 && c->n <= PLOT_W);
    CHECK(c->rise > 0 && c->rise <= c->n);
    m = env_polyline(c, plot_y, PLOT_W, PLOT_HOLD, plot_x_out, plot_y_out);
    CHECK(m <= c->n + 4u);
    CHECK(plot_x_out[0] == 0 && plot_y_out[0] == 0);
    CHECK(plot_y_out[m - 1u] == 0);                     /* ends at silence */
    CHECK(plot_x_out[m - 1u] <= PLOT_W + PLOT_HOLD);
    for (i = 1; i < m; i++) {
        CHECK(plot_x_out[i] >= plot_x_out[i - 1u]);
        if (plot_y_out[i] == c->sustain && plot_y_out[i - 1u] == c->sustain && plot_x_out[i] - plot_x_out[i - 1u] == PLOT_HOLD)
            flat = 1;
        if (plot_y_out[i] > top)
            top = plot_y_out[i];
    }
    CHECK(flat);                                        /* the sustain shows at its level */
    CHECK(top == 128);                                  /* the peak shows, however short */
    return m;
}

static void test_env_curve(void)
{
    env_curve_t c;
    uint32_t i;
    (void)plot_env(0, 0, 64, 0, &c);                    /* all times at 0 */
    CHECK(c.sustain >= 63 && c.sustain <= 64);
    CHECK(plot_x_out[1] <= 2);                          /* a 1 block attack is near vertical, not stretched */
    (void)plot_env(0, 0, 127, 0, &c);
    (void)plot_env(0, 0, 0, 0, &c);
    (void)plot_env(0, 80, 64, 0, &c);
    (void)plot_env(80, 0, 64, 0, &c);
    (void)plot_env(0, 0, 64, 80, &c);
    (void)plot_env(30, 30, 64, 127, &c);                /* a short peak before the longest release */
    CHECK(c.rise * 10u < c.n);
    (void)plot_env(127, 0, 127, 0, &c);                 /* the longest attack before a 1 block release */
    CHECK(c.rise * 10u > c.n * 9u);
    (void)plot_env(127, 127, 127, 127, &c);             /* the longest times fill the width */
    CHECK(c.span == c.n && c.n > PLOT_W / 2u);
    (void)plot_env(40, 40, 64, 40, &c);
    for (i = c.rise + 1u; i < c.n; i++)
        CHECK(plot_y[i] <= plot_y[i - 1u]);             /* the release only falls */
}

static void test_meter_bar(void)
{
    CHECK(meter_bar(0, 160) == 0);
    CHECK(meter_bar(MASTER_CEILING, 160) == 160);
    CHECK(meter_bar(32767, 160) == 160);
    CHECK(meter_bar(MASTER_CEILING / 2, 160) == 140);   /* 6 dB is 1/8 of the bar */
    CHECK(meter_bar(MASTER_CEILING / 4, 160) == 120);
    CHECK(meter_bar(MASTER_CEILING >> 8, 160) == 0);    /* 48 dB down */
    CHECK(meter_bar(MASTER_CEILING >> 7, 160) == 20);
}

static void test_lfo_rate_doubles(void)
{
    CHECK(lfo_inc(12) == lfo_inc(0) * 2u || lfo_inc(12) == lfo_inc(0) * 2u + 1u);
    CHECK(lfo_inc(36) / lfo_inc(24) == 2u);
    /* rate 0: one cycle in about 20 s */
    CHECK((1u << 31) / lfo_inc(0) * 2u / (FS / CTL) >= 19u && (1u << 31) / lfo_inc(0) * 2u / (FS / CTL) <= 21u);
}

static void test_lfo_shapes(void)
{
    static const int8_t SHAPES[] = {LFO_SINE, LFO_TRI, LFO_SAW, LFO_SQUARE};
    uint32_t s, n;
    for (s = 0; s < 4; s++) {
        lfo_t l;
        int32_t lo = 0, hi = 0;
        int64_t sum = 0;
        lfo_start(&l, SHAPES[s], LFO_RETRIGGER, 40, 0);
        for (n = 0; n < (1u << 31) / l.inc * 2u; n++) {
            int32_t v = lfo_block(&l, 0);
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
            sum += v;
        }
        CHECK(hi > 30000 && hi <= 32767);
        CHECK(lo < -30000 && lo >= -32767);
        CHECK(sum / (int64_t)n > -1500 && sum / (int64_t)n < 1500);    /* centered */
    }
}

static void test_lfo_rises_from_start(void)
{
    lfo_t l;
    lfo_start(&l, LFO_SINE, LFO_RETRIGGER, 60, 0);
    CHECK(lfo_block(&l, 0) > 0);
    lfo_start(&l, LFO_TRI, LFO_RETRIGGER, 60, 0);
    CHECK(lfo_block(&l, 0) > 0);
}

static void test_lfo_modes(void)
{
    lfo_t a, b;
    uint32_t n;
    lfo_start(&a, LFO_SINE, LFO_FREE, 60, 0);
    lfo_start(&b, LFO_SINE, LFO_FREE, 60, 0);
    for (n = 0; n < 100; n++)
        lfo_block(&a, n);
    CHECK(lfo_block(&a, 100) == lfo_block(&b, 100));    /* free: a late voice joins at the same phase */
    lfo_start(&a, LFO_SINE, LFO_RETRIGGER, 60, 0);
    lfo_start(&b, LFO_SINE, LFO_RETRIGGER, 60, 0);
    for (n = 0; n < 100; n++)
        lfo_block(&a, n);
    CHECK(lfo_block(&a, 100) != lfo_block(&b, 100));    /* retrigger: each voice starts its own */
}

static void test_lfo_sample_hold(void)
{
    lfo_t l;
    int32_t first, v;
    uint32_t n, changes = 0, per_cycle;
    lfo_start(&l, LFO_SH, LFO_RETRIGGER, 60, 0);
    per_cycle = (1u << 31) / l.inc * 2u;
    first = lfo_block(&l, 0);
    v = first;
    for (n = 0; n < per_cycle * 10u; n++) {
        int32_t now = lfo_block(&l, 0);
        changes += now != v;
        v = now;
    }
    CHECK(changes >= 9u && changes <= 11u);             /* one new value each cycle */
}

static void test_mod_routes(void)
{
    pkt_t p;
    chain_t c;
    uint32_t n;
    sound_init(&p, 0);
    chain_start(&c, &p);
    chain_mod(&c, &p, 0);
    for (n = 0; n < MOD_DSTS; n++)
        CHECK(n == MOD_DST_INDEX || c.off[n] == 0);     /* the other default routes have amount 0 */
    p.v[PV_MOD_S] = 127;
    p.v[PV_SLOT_AMT0] = 64;
    chain_start(&c, &p);
    for (n = 0; n < 20; n++)
        chain_mod(&c, &p, 0);
    CHECK(c.off[MOD_DST_CUT] > (60 << 8) && c.off[MOD_DST_CUT] < (68 << 8));
    p.v[PV_SLOT_AMT0] = -64;
    chain_mod(&c, &p, 0);
    CHECK(c.off[MOD_DST_CUT] < -(60 << 8));             /* a negative amount inverts */
    p.v[PV_SLOT_AMT0] = 0;
    p.v[PV_SLOT_AMT3] = 100;
    p.v[PV_VEL] = 127;
    chain_mod(&c, &p, 0);
    CHECK(c.off[MOD_DST_CUT] > (95 << 8));
    p.v[PV_VEL] = 0;
    chain_mod(&c, &p, 0);
    CHECK(c.off[MOD_DST_CUT] == 0);
}

static void test_mod_opens_filter(void)
{
    pkt_t p;
    chain_t c;
    int32_t l[CTL], r[CTL], closed = 0, opened = 0;
    uint32_t n, i, a;
    sound_init(&p, 0);
    p.v[PV_FLT_TYPE] = FLT_LP;
    p.v[PV_FLT_CUT] = 20;
    p.v[PV_MOD_S] = 127;
    for (a = 0; a < 2; a++) {
        p.v[PV_SLOT_AMT0] = a ? 127 : 0;
        chain_start(&c, &p);
        for (n = 0; n < 30; n++) {
            int32_t x[CTL];
            for (i = 0; i < CTL; i++) {
                x[i] = i & 4u ? -10000 : 10000;         /* 5.5 kHz: a low-pass of this form always nulls half the rate */
                l[i] = r[i] = 0;
            }
            chain_mod(&c, &p, 0);
            { int32_t delay[CTL] = {0}, reverb[CTL] = {0}; chain_render(&c, &p, x, 0, l, r, delay, reverb); }
        }
        if (a)
            opened = peak(l);
        else
            closed = peak(l);
    }
    CHECK(opened > closed * 5);
}

static void test_level_ramp_has_no_step(void)
{
    pkt_t p;
    chain_t c;
    int32_t l[CTL], r[CTL], prev = 0;
    uint32_t n, i;
    int32_t worst = 0;
    sound_init(&p, 0);
    p.v[PV_SLOT_SRC0] = MOD_SRC_LFO1;
    p.v[PV_SLOT_DST0] = MOD_DST_LEVEL;
    p.v[PV_SLOT_AMT0] = -100;
    p.v[PV_LFO1_SHAPE] = LFO_SQUARE;
    p.v[PV_LFO1_RATE] = 100;
    chain_start(&c, &p);
    for (n = 0; n < 200; n++) {
        chain_mod(&c, &p, n);
        render_tone(&c, &p, 10000, l, r);
        for (i = 0; i < CTL; i += 2) {                  /* even samples: the tone's positive half */
            int32_t d = l[i] - prev;
            worst = d < 0 ? (-d > worst ? -d : worst) : (d > worst ? d : worst);
            prev = l[i];
        }
    }
    CHECK(worst < 1500);                                /* a hard LFO step spreads over a block */
}

static void test_delay_repeats_at_its_time(void)
{
    static delay_t d;
    int32_t in[CTL] = {20000}, zero[CTL] = {0}, l[CTL], r[CTL], first_at = -1;
    uint32_t b, i, total = 0;
    memset(&d, 0, sizeof d);
    for (b = 0; b < 40; b++) {
        memset(l, 0, sizeof l);
        memset(r, 0, sizeof r);
        delay_block(&d, b ? zero : in, l, r, 1000, 80, 0);
        for (i = 0; i < CTL; i++, total++)
            if (first_at < 0 && l[i] != 0)
                first_at = (int32_t)total;
    }
    CHECK(first_at == 1000);
}

static int64_t energy(const int32_t *x)
{
    int64_t e = 0;
    uint32_t i;
    for (i = 0; i < CTL; i++)
        e += (int64_t)x[i] * x[i];
    return e;
}

static void test_delay_decays_at_full_feedback(void)
{
    static delay_t d;
    int32_t in[CTL], zero[CTL] = {0}, l[CTL], r[CTL];
    int64_t early = 0, late = 0;
    uint32_t b, i;
    memset(&d, 0, sizeof d);
    for (i = 0; i < CTL; i++)
        in[i] = 20000;
    for (b = 0; b < 60000u; b++) {
        memset(l, 0, sizeof l);
        memset(r, 0, sizeof r);
        delay_block(&d, b < 30u ? in : zero, l, r, 2000, 127, 0);
        CHECK(l[0] <= 32767 && l[0] >= -32768);
        if (b >= 100u && b < 500u)
            early += energy(l);
        if (b >= 59600u)
            late += energy(l);
    }
    CHECK(late < early / 100);
}

static void test_delay_time_glides(void)
{
    static delay_t d;
    int32_t zero[CTL] = {0}, l[CTL], r[CTL];
    memset(&d, 0, sizeof d);
    delay_block(&d, zero, l, r, 1000, 0, 0);
    CHECK(d.delay_cur == 1000u);
    delay_block(&d, zero, l, r, 2000, 0, 0);
    CHECK(d.delay_cur == 1000u + CTL);
    delay_block(&d, zero, l, r, 100, 0, 0);
    CHECK(d.delay_cur == 1000u);
}

static void test_reverb_tail(void)
{
    static reverb_t v;
    int32_t in[CTL], zero[CTL] = {0}, l[CTL], r[CTL];
    int64_t early = 0, late = 0, diff = 0;
    uint32_t b, i;
    memset(&v, 0, sizeof v);
    for (i = 0; i < CTL; i++)
        in[i] = i & 8u ? -20000 : 20000;
    for (b = 0; b < 30000u; b++) {
        memset(l, 0, sizeof l);
        memset(r, 0, sizeof r);
        reverb_block(&v, b < 8u ? in : zero, l, r, 127, 0);
        if (b >= 100u && b < 300u) {
            early += energy(l);
            for (i = 0; i < CTL; i++)
                diff += (int64_t)(l[i] - r[i]) * (l[i] - r[i]);
        }
        if (b >= 29800u)
            late += energy(l);
    }
    CHECK(early > 0);                                   /* there is a tail */
    CHECK(diff > early / 10);                           /* the sides differ */
    CHECK(late < early / 10000);                        /* largest size, no damping: still decays past -40 dB, with no limit cycle */
}

static void perc_packet(pkt_t *p, int32_t mode, int32_t note, int32_t decay, int32_t spread, int32_t harm, int32_t fold, int32_t bend, int32_t noise, int32_t click)
{
    sound_init(p, 0);
    p->v[PV_ENGINE] = ENGINE_PERC;
    p->v[PV_E0] = (int8_t)mode;
    p->v[PV_NOTE] = (int8_t)note;
    p->v[PV_AMP_D] = (int8_t)decay;
    p->v[PV_AMP_S] = 0;
    p->v[PV_AMP_R] = 127;
    p->v[PV_E1] = 0;
    p->v[PV_E2] = (int8_t)spread;
    p->v[PV_E3] = (int8_t)harm;
    p->v[PV_SHAPER] = (int8_t)fold;
    p->v[PV_E4] = (int8_t)bend;
    p->v[PV_E5] = (int8_t)noise;
    p->v[PV_E6] = (int8_t)click;
    p->v[PV_E7] = 0;
}

/* one block; returns its peak and adds its rising zero crossings to *crossings */
static int32_t perc_block(perc_voice_t *S, const pkt_t *p, int32_t *alive, uint32_t *crossings, int32_t *last)
{
    int32_t out[CTL], peak = 0;
    uint32_t i;
    for (i = 0; i < CTL; i++)
        out[i] = 0;
    *alive = perc_render(S, p, out, 0, 0);
    for (i = 0; i < CTL; i++) {
        if (abs(out[i]) > peak)
            peak = abs(out[i]);
        if (crossings && *last < 0 && out[i] >= 0)
            ++*crossings;
        *last = out[i];
    }
    return peak;
}

static uint32_t perc_crossings(const pkt_t *p, uint32_t skip, uint32_t blocks)
{
    static perc_voice_t S;
    int32_t alive, last = 0;
    uint32_t b, n = 0;
    perc_start(&S, p, 1);
    for (b = 0; b < skip; b++)
        (void)perc_block(&S, p, &alive, 0, &last);
    for (b = 0; b < blocks; b++)
        (void)perc_block(&S, p, &alive, &n, &last);
    return n;
}

static uint32_t perc_life(const pkt_t *p)
{
    static perc_voice_t S;
    int32_t alive = 1, last = 0;
    uint32_t b = 0;
    perc_start(&S, p, 1);
    while (alive && b < 400000u) {
        (void)perc_block(&S, p, &alive, 0, &last);
        b++;
    }
    return b;
}

static void test_perc_pitch(void)
{
    pkt_t p;
    uint32_t n, blocks = (uint32_t)(FS / CTL);          /* about one second */
    perc_packet(&p, PERC_SKIN, 69, 120, 0, 0, 0, 0, 0, 0);
    n = perc_crossings(&p, 0, blocks);
    CHECK(n > 430 && n < 450);                          /* A4 is 440 Hz */
    perc_packet(&p, PERC_SKIN, 81, 120, 0, 0, 0, 0, 0, 0);
    n = perc_crossings(&p, 0, blocks);
    CHECK(n > 865 && n < 895);                          /* an octave up */
    perc_packet(&p, PERC_METAL, 69, 120, 0, 0, 0, 0, 0, 0);
    CHECK(perc_crossings(&p, 0, blocks) > 400);         /* with harm 0 the modulators are silent */
}

static void test_perc_bend(void)
{
    pkt_t p;
    uint32_t early, late;
    perc_packet(&p, PERC_SKIN, 45, 120, 0, 0, 0, 80, 0, 0);
    early = perc_crossings(&p, 0, 30);
    late = perc_crossings(&p, 2000, 30);
    CHECK(early > late * 2u);
    perc_packet(&p, PERC_SKIN, 45, 120, 0, 0, 0, 0, 0, 0);
    CHECK(perc_crossings(&p, 2000, 30) >= late - 2u && perc_crossings(&p, 2000, 30) <= late + 2u);   /* it settles on the note */
}

static void test_perc_spread_and_harm(void)
{
    int32_t level[PERC_PARTIALS];
    uint32_t i;
    for (i = 0; i < PERC_PARTIALS; i++) {
        CHECK(perc_ratio_q8(PERC_BAR, 0, i) == 256);    /* 0 is unison */
        CHECK(perc_ratio_q8(PERC_TONE, 64, i) == 256u * (i + 1u));
        CHECK(perc_ratio_q8(PERC_BAR, 64, i) == PERC_RATIOS[PERC_BAR][i]);
    }
    perc_levels(0, 32767, level);
    CHECK(level[0] > 32000 && level[1] == 0);
    perc_levels(127, 32767, level);
    for (i = 0; i < PERC_PARTIALS; i++)
        CHECK(level[i] == level[0] && level[i] > 5300);
    perc_levels(64, 16000, level);
    CHECK(level[1] < level[0] && level[0] + level[1] + level[2] + level[3] + level[4] + level[5] <= 16000);
}

static void test_perc_decay_ends_the_voice(void)
{
    pkt_t p;
    uint32_t short_life, long_life;
    perc_packet(&p, PERC_SKIN, 45, 60, 0, 0, 0, 0, 0, 0);
    short_life = perc_life(&p);
    CHECK(short_life + 40u > env_blocks(60) && short_life < env_blocks(60) * 2u + 40u);      /* -60 dB at about AMP D's time */
    perc_packet(&p, PERC_SKIN, 45, 100, 0, 0, 0, 0, 0, 0);
    long_life = perc_life(&p);
    CHECK(long_life > short_life * 4u && long_life < 400000u);
    perc_packet(&p, PERC_SKIN, 45, 0, 0, 127, 0, 0, 0, 127);
    CHECK(perc_life(&p) < 100u);                        /* the shortest settings */
    perc_packet(&p, PERC_METAL, 45, 127, 90, 127, 127, 127, 127, 127);
    CHECK(perc_life(&p) < 400000u);                     /* the longest settings */
}

/* harm sets how much upper partials ring: a low harm thins out over the note */
static void test_perc_harm_shortens_upper_partials(void)
{
    static perc_voice_t a, b;
    pkt_t low, high;
    perc_packet(&low, PERC_SKIN, 45, 100, 64, 40, 0, 0, 0, 0);
    perc_packet(&high, PERC_SKIN, 45, 100, 64, 127, 0, 0, 0, 0);
    perc_start(&a, &low, 1);
    perc_start(&b, &high, 1);
    CHECK(a.decay_k[5] < a.decay_k[0]);
    CHECK(b.decay_k[5] == b.decay_k[0]);
}

static void test_perc_level_bound(void)
{
    static const int8_t V[3] = {0, 64, 127};
    static perc_voice_t S;
    uint32_t mode, a, b, c, d, block;
    int32_t worst = 0;
    for (mode = 0; mode < PERC_MODES; mode++)
        for (a = 0; a < 3u; a++)
            for (b = 0; b < 3u; b++)
                for (c = 0; c < 3u; c++)
                    for (d = 0; d < 3u; d++) {
                        pkt_t p;
                        int32_t alive = 1, last = 0;
                        perc_packet(&p, (int32_t)mode, 45, 70, V[a], V[b], V[c], V[d], V[d], V[a]);
                        p.v[PV_E1] = V[b];
                        perc_start(&S, &p, 7);
                        for (block = 0; alive && block < 300u; block++) {
                            int32_t peak = perc_block(&S, &p, &alive, 0, &last);
                            if (peak > worst)
                                worst = peak;
                        }
                    }
    CHECK(worst <= PERC_PEAK * 3 / 2 + 100);
    CHECK(worst >= PERC_PEAK / 2);
}

static int32_t perc_peak_at(const pkt_t *p, uint32_t block_at)
{
    static perc_voice_t S;
    int32_t alive, last = 0, peak = 0;
    uint32_t b;
    perc_start(&S, p, 1);
    for (b = 0; b < block_at + 16u; b++) {
        int32_t v = perc_block(&S, p, &alive, 0, &last);
        if (b >= block_at && v > peak)
            peak = v;
    }
    return peak;
}

static void test_wavefold(void)
{
    int32_t x;
    CHECK(wavefold(0) == 0 && wavefold(32767) == 32767);
    CHECK(abs(wavefold(40000) - (65534 - 40000)) <= 1);
    for (x = -500000; x <= 500000; x += 37) {
        int32_t y = wavefold(x);
        CHECK(y >= -32768 && y <= 32767);
    }
}

static void test_perc_noise_seed(void)
{
    static perc_voice_t a, b;
    pkt_t p;
    int32_t ao[CTL], bo[CTL];
    uint32_t i;
    int differs = 0;
    perc_packet(&p, PERC_SKIN, 45, 80, 0, 0, 0, 0, 90, 0);
    perc_start(&a, &p, 1);
    perc_start(&b, &p, 99);
    for (i = 0; i < CTL; i++)
        ao[i] = bo[i] = 0;
    (void)perc_render(&a, &p, ao, 0, 0);
    (void)perc_render(&b, &p, bo, 0, 0);
    for (i = 0; i < CTL; i++)
        differs |= ao[i] != bo[i];
    CHECK(differs);                                     /* NOISE differs by seed */
    perc_packet(&p, PERC_SKIN, 45, 80, 0, 0, 0, 0, 0, 0);
    perc_start(&a, &p, 1);
    perc_start(&b, &p, 99);
    for (i = 0; i < CTL; i++)
        ao[i] = bo[i] = 0;
    (void)perc_render(&a, &p, ao, 0, 0);
    (void)perc_render(&b, &p, bo, 0, 0);
    CHECK(memcmp(ao, bo, sizeof ao) == 0);              /* with no noise and no click, the seed does nothing */
}

static void test_perc_click(void)
{
    static perc_voice_t a, b;
    pkt_t hard, soft;
    int32_t ao[CTL], bo[CTL];
    uint32_t i;
    perc_packet(&hard, PERC_SKIN, 45, 100, 0, 0, 0, 0, 0, 127);
    perc_packet(&soft, PERC_SKIN, 45, 100, 0, 0, 0, 0, 0, 0);
    CHECK(perc_peak_at(&hard, 0) > perc_peak_at(&soft, 0));
    perc_start(&a, &hard, 1);
    perc_start(&b, &hard, 99);
    for (i = 0; i < CTL; i++)
        ao[i] = bo[i] = 0;
    (void)perc_render(&a, &hard, ao, 0, 0);
    (void)perc_render(&b, &hard, bo, 0, 0);
    CHECK(memcmp(ao, bo, sizeof ao) != 0);              /* the click is noise, with NOISE at 0 */
}

/* power at one frequency over 8192 samples after skip blocks. inc: phase per sample, as a partial's */
static double perc_power_at(const pkt_t *p, uint32_t inc, uint32_t skip)
{
    enum { N = 8192 };
    static perc_voice_t S;
    double re = 0, im = 0;
    uint32_t ph = 0, b, n = 0;
    perc_start(&S, p, 1);
    for (b = 0; b < skip; b++) {
        int32_t out[CTL] = {0};
        (void)perc_render(&S, p, out, 0, 0);
    }
    while (n < N) {
        int32_t out[CTL] = {0};
        uint32_t i;
        (void)perc_render(&S, p, out, 0, 0);
        for (i = 0; i < CTL && n < N; i++, n++) {
            double window = n < N / 2 ? n : N - n;      /* its side lobes fall fast enough to separate the modes' partials */
            re += window * out[i] * SINE[ph >> 22];
            im += window * out[i] * SINE[(ph + 0x40000000u) >> 22];
            ph += inc;
        }
    }
    return re * re + im * im;
}

/* each mode's partials sound at its table's frequencies, and not at another mode's */
static void test_perc_modes_set_the_partials(void)
{
    uint32_t mode, other, base = note_inc(57 * 16);
    for (mode = 0; mode < PERC_MODES; mode++) {
        pkt_t p;
        uint32_t inc = (uint32_t)(((uint64_t)base * PERC_RATIOS[mode][1]) >> 8);
        double own;
        perc_packet(&p, (int32_t)mode, 57, 120, 64, 127, 0, 0, 0, 0);
        own = perc_power_at(&p, inc, 0);
        for (other = 0; other < PERC_MODES; other++)
            if (other != mode) {
                perc_packet(&p, (int32_t)other, 57, 120, 64, 127, 0, 0, 0, 0);
                CHECK(own > perc_power_at(&p, inc, 0) * 100.0);
            }
    }
}

/* FM makes sidebands from a lone partial, falls over half of the decay, and keeps no voice alive */
static void test_perc_fm(void)
{
    pkt_t p, dry;
    uint32_t base = note_inc(57 * 16), inc = base * 3u, late;     /* a carrier at 1 and a modulator at 2 add a sideband at 3 */
    double early;
    perc_packet(&dry, PERC_TONE, 57, 100, 64, 0, 0, 0, 0, 0);
    p = dry;
    p.v[PV_E1] = 90;
    early = perc_power_at(&p, inc, 0);
    CHECK(early > perc_power_at(&dry, inc, 0) * 1000.0);
    late = env_blocks(100) / 2u;                        /* relative to the note, which falls too */
    CHECK(early / perc_power_at(&p, base, 0) > perc_power_at(&p, inc, late) / perc_power_at(&p, base, late) * 100.0);
    CHECK(perc_life(&p) * 10u < perc_life(&dry) * 11u && perc_life(&dry) * 10u < perc_life(&p) * 11u);
}

/* noise falls with the bend, unless the partials end first */
static void test_perc_noise_fall(void)
{
    static perc_voice_t S;
    pkt_t p, dry;
    perc_packet(&p, PERC_SKIN, 45, 110, 0, 0, 0, 0, 90, 0);
    perc_start(&S, &p, 1);
    CHECK(S.noise_k == S.bend_k);
    perc_packet(&p, PERC_SKIN, 45, 50, 0, 0, 0, 0, 90, 0);
    perc_packet(&dry, PERC_SKIN, 45, 50, 0, 0, 0, 0, 0, 0);
    perc_start(&S, &p, 1);
    CHECK(S.noise_k == S.decay_k[0]);
    CHECK(perc_life(&p) * 10u < perc_life(&dry) * 13u);
}

/* noise is a transient: after the bend's time, SUS holds only the partials. The seed changes only the noise */
static void test_perc_noise_ignores_sustain(void)
{
    static perc_voice_t a, b;
    pkt_t p;
    uint32_t n, i, ends = env_blocks(100) / 4u + 235u + 2u;
    int32_t worst = 0;
    perc_packet(&p, PERC_SKIN, 45, 100, 0, 0, 0, 0, 127, 0);
    p.v[PV_AMP_S] = 90;
    perc_start(&a, &p, 1);
    perc_start(&b, &p, 99);
    for (n = 0; n < ends + 32u; n++) {
        int32_t ao[CTL] = {0}, bo[CTL] = {0};
        (void)perc_render(&a, &p, ao, 0, 0);
        (void)perc_render(&b, &p, bo, 0, 0);
        for (i = 0; n >= ends && i < CTL; i++)
            worst = abs(ao[i] - bo[i]) > worst ? abs(ao[i] - bo[i]) : worst;
    }
    CHECK(worst == 0);
}

static void fm_packet(pkt_t *p, int32_t alg, int32_t note, int32_t c, int32_t a, int32_t b, int32_t index_a, int32_t index_b, int32_t feedback, int32_t detune)
{
    sound_init(p, (uint32_t)alg);
    p->v[PV_NOTE] = (int8_t)note;
    p->v[PV_E1] = (int8_t)c;
    p->v[PV_E2] = (int8_t)a;
    p->v[PV_E3] = (int8_t)b;
    p->v[PV_E4] = (int8_t)index_a;
    p->v[PV_E5] = (int8_t)index_b;
    p->v[PV_E6] = (int8_t)feedback;
    p->v[PV_E7] = (int8_t)detune;
}

/* the routes as data, slowly: each algorithm's loop must match this sample for sample */
static void test_fm_loops_match_routes(void)
{
    uint32_t alg, op, j, b, s;
    for (alg = 0; alg < FM_ALGS; alg++) {
        fm_voice_t f;
        pkt_t p;
        uint32_t phase[FM_OPS] = {0, 0, 0, 0}, inc[FM_OPS];
        int32_t h1 = 0, h2 = 0, dc = 0, detune, bad = 0;
        fm_packet(&p, (int32_t)alg, 57, 2, 4, 3, 90, 70, 60, 20);
        fm_start(&f, &p);
        detune = fm_squared(p.v[PV_E7], 16384);
        inc[FM_C] = fm_op_inc(note_inc(57 * 16), 2, 0);
        inc[FM_A] = fm_op_inc(note_inc(57 * 16), 4, detune);
        inc[FM_B1] = fm_op_inc(note_inc(57 * 16), 3, -detune);
        inc[FM_B2] = fm_op_inc(note_inc(57 * 16), 3, detune / 2);
        for (b = 0; b < 40u; b++) {
            int32_t out[CTL] = {0};
            fm_render(&f, &p, out, 0, 0, 32768, 0);
            for (s = 0; s < CTL; s++) {
                int32_t val[FM_OPS], y = 0, x;
                for (op = FM_OPS; op-- > 0;) {
                    int32_t m = op == FM_B2 ? ((h1 + h2) >> 1) * f.feedback >> 15 : 0;
                    for (j = 0; j < FM_OPS; j++)
                        if (FM_ROUTES[alg][op] >> j & 1u)
                            m += (val[j] * (j == FM_A ? fm_squared(p.v[PV_E4], 32767) : fm_squared(p.v[PV_E5], 32767))) >> 15;
                    val[op] = SINE[(phase[op] + (uint32_t)m * (1u << FM_SHIFT)) >> 22];
                    phase[op] += inc[op];
                    if (fm_is_carrier(alg, op))
                        y += op == FM_C ? val[op] : (val[op] * (op == FM_A ? fm_squared(p.v[PV_E4], 32767) : fm_squared(p.v[PV_E5], 32767))) >> 15;
                }
                h2 = h1;
                h1 = val[FM_B2];
                x = y * 256 - dc;
                dc += x >> 10;
                bad += out[s] != (((x >> 8) * f.mix_gain) >> 15);
            }
        }
        CHECK(bad == 0);
    }
}

static void test_fm_routes_run_forward(void)
{
    uint32_t alg, op, j;
    for (alg = 0; alg < FM_ALGS; alg++) {
        CHECK(fm_is_carrier(alg, FM_C));
        for (op = 0; op < FM_OPS; op++)
            for (j = 0; j <= op; j++)
                CHECK(!(FM_ROUTES[alg][op] >> j & 1u));
    }
}

/* power at one frequency over 8192 samples, with the MOD envelope held at mod_q15 */
static double fm_power_at(const pkt_t *p, uint32_t inc, int32_t mod_q15)
{
    enum { N = 8192 };
    fm_voice_t f;
    double re = 0, im = 0;
    uint32_t ph = 0, n = 0;
    fm_start(&f, p);
    while (n < N) {
        int32_t out[CTL] = {0};
        uint32_t i;
        fm_render(&f, p, out, 0, 0, mod_q15, 0);
        for (i = 0; i < CTL && n < N; i++, n++) {
            double window = n < N / 2 ? n : N - n;
            re += window * out[i] * SINE[ph >> 22];
            im += window * out[i] * SINE[(ph + 0x40000000u) >> 22];
            ph += inc;
        }
    }
    return re * re + im * im;
}

static void test_fm_sound(void)
{
    pkt_t p, q;
    uint32_t base = note_inc(57 * 16);
    double power;
    fm_packet(&p, 6, 57, 2, 3, 2, 0, 0, 0, 0);          /* C at 1, A at 2 */
    p.v[PV_E4] = 90;
    power = fm_power_at(&p, base * 3u, 32768);          /* a sideband at 3 */
    q = p;
    q.v[PV_E4] = 0;
    CHECK(power > fm_power_at(&q, base * 3u, 32768) * 1000.0);    /* the index makes it */
    CHECK(power > fm_power_at(&p, base * 3u, 0) * 1000.0);        /* the MOD envelope scales it */
    q = p;
    q.v[PV_E2] = 4;                                     /* A at 3 makes a sideband at 4, and A at 2 makes only odd ones */
    CHECK(fm_power_at(&q, base * 4u, 32768) > fm_power_at(&p, base * 4u, 32768) * 1000.0);
    fm_packet(&p, 7, 57, 2, 2, 4, 0, 127, 0, 0);        /* B at 3, sounding */
    q = p;
    q.v[PV_E6] = 90;
    CHECK(fm_power_at(&q, base * 6u, 32768) > fm_power_at(&p, base * 6u, 32768) * 100.0);    /* feedback adds harmonics of B2 */
    q = p;
    q.v[PV_E7] = 127;
    CHECK(fm_power_at(&q, fm_op_inc(base, 4, -fm_squared(127, 16384)), 32768) > fm_power_at(&p, fm_op_inc(base, 4, -fm_squared(127, 16384)), 32768) * 2.0);    /* detune moves B1 */
    CHECK(fm_op_inc(note_inc(127 * 16), 15, 0) == 0u);  /* over Nyquist it would alias */
}

/* the MOD envelope moves the index on a real voice, and holds it at MOD SUS */
static void test_fm_mod_envelope(void)
{
    int32_t l[CTL], r[CTL], d[CTL], w[CTL];
    pkt_t p;
    uint32_t b, i;
    fm_packet(&p, 0, 57, 2, 3, 3, 90, 90, 0, 0);
    p.v[PV_MOD_D] = 64;
    p.v[PV_MOD_S] = 0;
    memset(voices, 0, sizeof voices);
    voice_trigger(&p, KEY_NONE, 0);
    for (b = 0; b < env_blocks(64) * 2u; b++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        voices_render(l, r, d, w);
    }
    CHECK(voices[0].engine.fm.gain_a < fm_squared(p.v[PV_E4], 32767) / 100);
    p.v[PV_MOD_S] = 127;
    memset(voices, 0, sizeof voices);
    voice_trigger(&p, KEY_NONE, 0);
    for (b = 0; b < env_blocks(64) * 2u; b++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        voices_render(l, r, d, w);
    }
    CHECK(voices[0].engine.fm.gain_a > fm_squared(p.v[PV_E4], 32767) * 9 / 10);
}

static void test_fm_level_bound(void)
{
    static const int8_t V[3] = {0, 64, 127};
    uint32_t alg, a, b, c, block, i;
    int32_t worst = 0;
    for (alg = 0; alg < FM_ALGS; alg++)
        for (a = 0; a < 3u; a++)
            for (b = 0; b < 3u; b++)
                for (c = 0; c < 3u; c++) {
                    fm_voice_t f;
                    pkt_t p;
                    fm_packet(&p, (int32_t)alg, 45, 2, 3, 4, V[a], V[b], V[c], V[a]);
                    fm_start(&f, &p);
                    for (block = 0; block < 300u; block++) {
                        int32_t out[CTL] = {0};
                        fm_render(&f, &p, out, 0, 0, 32768, 0);
                        for (i = 0; i < CTL; i++)
                            worst = abs(out[i]) > worst ? abs(out[i]) : worst;
                    }
                }
    CHECK(worst <= FM_PEAK * 3 / 2);
    CHECK(worst >= FM_PEAK / 2);
}

/* feedback puts an offset in B2's output, and the mean over whole periods shows what is left of it */
static void test_fm_removes_offset(void)
{
    enum { PERIODS = 32 };
    fm_voice_t f;
    pkt_t p;
    uint32_t b, i, n = 0, samples = (uint32_t)(PERIODS * (4294967296.0 / fm_op_inc(note_inc(45 * 16), 2, 0)));
    int64_t sum = 0;
    fm_packet(&p, 7, 45, 2, 2, 2, 0, 127, 127, 0);
    fm_start(&f, &p);
    for (b = 0; n < samples; b++) {
        int32_t out[CTL] = {0};
        fm_render(&f, &p, out, 0, 0, 32768, 0);
        for (i = 0; b >= 400u && i < CTL && n < samples; i++, n++)
            sum += out[i];
    }
    CHECK(llabs(sum / (int64_t)samples) < 12);
}

static void swarm_packet(pkt_t *p, int32_t mode, int32_t note, int32_t spread, int32_t shape, int32_t shift, int32_t mix, int32_t sub, int32_t drift)
{
    sound_init(p, 0);
    p->v[PV_ENGINE] = ENGINE_SWARM;
    p->v[PV_E0] = (int8_t)mode;
    p->v[PV_NOTE] = (int8_t)note;
    p->v[PV_E1] = (int8_t)spread;
    p->v[PV_E2] = (int8_t)shape;
    p->v[PV_E3] = (int8_t)shift;
    p->v[PV_E4] = (int8_t)mix;
    p->v[PV_E5] = (int8_t)sub;
    p->v[PV_E6] = (int8_t)drift;
}

/* fills buf with n samples, after skip blocks */
static void swarm_samples(const pkt_t *p, int32_t *buf, uint32_t n, uint32_t skip, int32_t timbre)
{
    swarm_voice_t S;
    uint32_t b, i = 0;
    swarm_start(&S, p, 12345u);
    for (b = 0; i < n; b++) {
        int32_t out[CTL] = {0};
        uint32_t s;
        swarm_render(&S, p, out, 0, 0, timbre);
        for (s = 0; b >= skip && s < CTL && i < n; s++)
            buf[i++] = out[s];
    }
}

#define SWARM_N 8192u
static int32_t swarm_buf[SWARM_N];

static double buf_power_at(uint32_t inc)
{
    double re = 0, im = 0;
    uint32_t ph = 0, n;
    for (n = 0; n < SWARM_N; n++) {
        double window = n < SWARM_N / 2u ? n : SWARM_N - n;
        re += window * swarm_buf[n] * SINE[ph >> 22];
        im += window * swarm_buf[n] * SINE[(ph + 0x40000000u) >> 22];
        ph += inc;
    }
    return re * re + im * im;
}

static double swarm_power_at(const pkt_t *p, uint32_t inc, int32_t timbre)
{
    swarm_samples(p, swarm_buf, SWARM_N, 8u, timbre);
    return buf_power_at(inc);
}

static double swarm_rms(const pkt_t *p)
{
    double e = 0;
    uint32_t n;
    swarm_samples(p, swarm_buf, SWARM_N, 8u, 0);
    for (n = 0; n < SWARM_N; n++)
        e += (double)swarm_buf[n] * swarm_buf[n];
    return e / SWARM_N;
}

static void test_swarm_pitch_and_shape(void)
{
    pkt_t p, q;
    uint32_t base = note_inc(57 * 16);
    uint32_t n, crossings = 0;
    int32_t last = 0;
    swarm_packet(&p, SWARM_UNISON, 69, 0, 0, 0, 0, 0, 0);       /* the center saw alone: A4 is 440 Hz */
    swarm_samples(&p, swarm_buf, SWARM_N, 8u, 0);
    for (n = 0; n < SWARM_N; n++) {
        crossings += last < 0 && swarm_buf[n] >= 0;
        last = swarm_buf[n];
    }
    CHECK(crossings > 75u && crossings < 88u);
    swarm_packet(&p, SWARM_UNISON, 57, 0, 0, 0, 0, 0, 0);       /* a saw has a 2nd harmonic */
    q = p;
    q.v[PV_E2] = 64;                                            /* a square does not */
    CHECK(swarm_power_at(&p, base * 2u, 0) > swarm_power_at(&q, base * 2u, 0) * 100.0);
    q.v[PV_E2] = 127;                                           /* a narrow pulse does again */
    {
        pkt_t square = q;
        square.v[PV_E2] = 64;
        CHECK(swarm_power_at(&q, base * 2u, 0) > swarm_power_at(&square, base * 2u, 0) * 100.0);
    }
    CHECK(swarm_power_at(&p, base * 2u, 64) * 100.0 < swarm_power_at(&p, base * 2u, 0));   /* TIMBRE moves SHAPE */
}

/* 3 dB over the whole of SHAPE */
static void test_swarm_shape_keeps_loudness(void)
{
    pkt_t p;
    int32_t shape;
    double lo = 1e30, hi = 0;
    for (shape = 0; shape <= 127; shape += 16) {
        double e;
        swarm_packet(&p, SWARM_UNISON, 57, 0, shape, 0, 0, 0, 0);
        e = swarm_rms(&p);
        lo = e < lo ? e : lo;
        hi = e > hi ? e : hi;
    }
    CHECK(hi < lo * 2.0);
}

static void test_swarm_spread_mode_mix_sub(void)
{
    pkt_t p, q;
    uint32_t base = note_inc(57 * 16);
    uint32_t outer = (uint32_t)(((uint64_t)base * (65536u + 7042u)) >> 16);
    swarm_packet(&p, SWARM_UNISON, 57, 127, 0, 0, 127, 0, 0);
    q = p;
    q.v[PV_E1] = 0;
    CHECK(swarm_power_at(&p, outer, 0) > swarm_power_at(&q, outer, 0) * 100.0);     /* SPREAD reaches the outer oscillator */
    q = p;
    q.v[PV_E4] = 0;
    CHECK(swarm_power_at(&p, outer, 0) > swarm_power_at(&q, outer, 0) * 100.0);     /* MIX 0 leaves the center alone */
    swarm_packet(&p, SWARM_OCTAVES, 57, 0, 64, 0, 127, 0, 0);
    q = p;
    q.v[PV_E0] = SWARM_UNISON;
    CHECK(swarm_power_at(&p, base >> 1, 0) > swarm_power_at(&q, base >> 1, 0) * 100.0);
    swarm_packet(&p, SWARM_UNISON, 57, 0, 64, 0, 0, 127, 0);
    q = p;
    q.v[PV_E5] = 0;
    CHECK(swarm_power_at(&p, base >> 1, 0) > swarm_power_at(&q, base >> 1, 0) * 100.0);    /* SUB */
}

/* SUB's level answers to SUB alone, and it leaves the oscillators alone */
static void test_swarm_sub_is_separate(void)
{
    pkt_t p, q;
    uint32_t base = note_inc(57 * 16);
    double alone, with_sides, with_square, oscs;
    swarm_packet(&p, SWARM_UNISON, 57, 0, 0, 0, 0, 127, 0);
    alone = swarm_power_at(&p, base >> 1, 0);
    q = p;
    q.v[PV_E4] = 127;
    with_sides = swarm_power_at(&q, base >> 1, 0);
    q = p;
    q.v[PV_E2] = 64;
    with_square = swarm_power_at(&q, base >> 1, 0);
    printf("sub: sides %.1f dB, square %.1f dB\n", 10.0 * log10(with_sides / alone), 10.0 * log10(with_square / alone));
    CHECK(with_sides > alone * 0.8 && with_sides < alone * 1.25);
    CHECK(with_square > alone * 0.8 && with_square < alone * 1.25);
    oscs = swarm_power_at(&p, base, 0);
    q = p;
    q.v[PV_E5] = 0;
    CHECK(oscs > swarm_power_at(&q, base, 0) * 0.8 && oscs < swarm_power_at(&q, base, 0) * 1.25);
}

/* harmonic 22 of 2093 Hz is past the sample rate and folds to 1946 Hz, which is not a harmonic. Multiplying the phase step by 22 wraps there too */
static void test_swarm_band_limit(void)
{
    pkt_t p;
    uint32_t base = note_inc(96 * 16);
    double fundamental, alias;
    swarm_packet(&p, SWARM_UNISON, 96, 0, 0, 0, 0, 0, 0);
    fundamental = swarm_power_at(&p, base, 0);
    alias = swarm_power_at(&p, base * 22u, 0);
    printf("swarm alias %.1f dB, ", 10.0 * log10(alias / fundamental));
    CHECK(fundamental > alias * 100000.0);
    p.v[PV_E3] = 100;                                   /* 3.3 times the pitch: the 3rd harmonic is the strongest */
    fundamental = swarm_power_at(&p, base * 3u, 0);
    alias = swarm_power_at(&p, base * 22u, 0);
    printf("with sync %.1f dB\n", 10.0 * log10(alias / fundamental));
    CHECK(fundamental > alias * 100000.0);
}

/* SHIFT at 2.875 times the pitch: the 3rd harmonic grows and the note stays */
static void test_swarm_shift(void)
{
    pkt_t p, q;
    uint32_t base = note_inc(45 * 16);
    swarm_packet(&p, SWARM_UNISON, 45, 0, 0, 0, 0, 0, 0);
    q = p;
    q.v[PV_E3] = 80;
    CHECK(swarm_power_at(&q, base * 3u, 0) > swarm_power_at(&p, base * 3u, 0) * 3.0);
    CHECK(swarm_power_at(&q, base, 0) > swarm_power_at(&q, base + (base >> 1), 0) * 100.0);
}

/* lines meet across blocks: the second difference is large only at an edge, a few per cycle */
enum { SHAPE_STILL, SHAPE_SWEEPS, SHAPE_JUMPS };
static uint32_t swarm_kinks(const pkt_t *p, int move, int32_t limit)
{
    static swarm_voice_t S;
    uint32_t n = 0, kinks = 0, b = 0;
    swarm_start(&S, p, 12345u);
    while (n < SWARM_N) {
        int32_t out[CTL] = {0};
        uint32_t s;
        int32_t timbre = move == SHAPE_SWEEPS ? abs((int32_t)(b % 128u) - 64) : move == SHAPE_JUMPS ? (int32_t)((b / 64u) & 1u) * 63 : 0;
        swarm_render(&S, p, out, 0, 0, timbre);
        b++;
        for (s = 0; s < CTL && n < SWARM_N; s++)
            swarm_buf[n++] = out[s];
    }
    for (n = 2000u; n < SWARM_N; n++) {
        int32_t bend = swarm_buf[n] - 2 * swarm_buf[n - 1u] + swarm_buf[n - 2u];
        kinks += abs(bend) > limit;
    }
    return kinks;
}

static void test_swarm_lines_join(void)
{
    pkt_t p;
    uint32_t slow, flat;
    swarm_packet(&p, SWARM_UNISON, 45, 0, 0, 0, 0, 0, 0);       /* 110 Hz: 16 cycles and 4 samples around each edge */
    flat = swarm_kinks(&p, SHAPE_STILL, 4);
    printf("swarm kinks: saw %u, ", flat);
    CHECK(flat <= 16u * 4u);
    p.v[PV_E2] = 64;
    flat = swarm_kinks(&p, SHAPE_STILL, 4);
    printf("square %u, ", flat);
    CHECK(flat <= 32u * 4u);
    swarm_packet(&p, SWARM_UNISON, 45, 40, 0, 0, 127, 0, 90);   /* 7 oscillators */
    flat = swarm_kinks(&p, SHAPE_STILL, 8);
    printf("seven %u, ", flat);
    CHECK(flat <= 7u * 16u * 4u);
    swarm_packet(&p, SWARM_UNISON, 33, 0, 0, 0, 0, 0, 0);       /* SHAPE moving at every block, 8 cycles at 55 Hz */
    slow = swarm_kinks(&p, SHAPE_SWEEPS, 40);
    printf("sweep %u, ", slow);
    CHECK(slow <= 100u);
    p.v[PV_E2] = 64;                                            /* square to narrow pulse in one block */
    slow = swarm_kinks(&p, SHAPE_JUMPS, 300);
    printf("jump %u\n", slow);
    CHECK(slow <= 8u * 2u * 4u);
}

/* an edge between blocks: t is 1/2, so its two samples sit evenly around the middle of the step, one in each block */
static void test_swarm_edge_across_blocks(void)
{
    static swarm_voice_t S;
    pkt_t p;
    int32_t o[CTL * 2u] = {0};
    uint32_t inc = note_inc(45 * 16);
    swarm_packet(&p, SWARM_UNISON, 45, 0, 0, 0, 0, 0, 0);
    swarm_start(&S, &p, 1u);
    S.phase[SWARM_CENTER] = 0u - inc * 31u - inc / 2u;
    swarm_render(&S, &p, o, 0, 0, 0);
    swarm_render(&S, &p, o + CTL, 0, 0, 0);
    CHECK(o[CTL - 1u] > SWARM_PEAK / 3 && o[CTL] < -SWARM_PEAK / 3);
    CHECK(abs(o[CTL - 1u] + o[CTL]) < SWARM_PEAK / 10);   /* a lone sample at the far side of the step would miss by 1/4 of the peak */
}

/* what a note is charged counts its edges: 440 Hz is 0.32 edges a block per saw */
static void test_swarm_charge(void)
{
    pkt_t p;
    uint32_t e;
    swarm_packet(&p, SWARM_UNISON, 69, 0, 0, 0, 127, 0, 0);
    e = swarm_edges_q8(&p);
    CHECK(e > 7u * 2u * 75u && e < 7u * 2u * 88u);
    p.v[PV_E4] = 0;
    CHECK(swarm_edges_q8(&p) == e / 7u);
    swarm_packet(&p, SWARM_UNISON, 69, 0, 0, 64, 127, 0, 0);    /* 2.5 times the pitch, and the master */
    CHECK(swarm_edges_q8(&p) > e * 27u / 10u && swarm_edges_q8(&p) < e * 33u / 10u);
    swarm_base_cost = 1000u;
    swarm_edge_cost = 100u;
    swarm_packet(&p, SWARM_UNISON, 36, 0, 0, 0, 127, 0, 0);
    e = voice_charge(&p);
    p.v[PV_NOTE] = 96;
    CHECK(e > 1000u && voice_charge(&p) > e * 2u);
    memset(voices, 0, sizeof voices);
    voice_budget = voice_charge(&p) * 2u;               /* two high notes fit, and a third steals */
    voice_trigger(&p, KEY_NONE, 0);
    voice_trigger(&p, KEY_NONE, 0);
    CHECK(voices_active() == 2u && !voice_fading(&voices[0]) && !voice_fading(&voices[1]));
    voice_trigger(&p, KEY_NONE, 0);
    CHECK(voice_fading(&voices[0]) && !voices[2].active);
    voice_budget = 0xFFFFFFFFu;
    swarm_base_cost = swarm_edge_cost = 0u;
    memset(voices, 0, sizeof voices);
}

static void test_swarm_drift(void)
{
    static swarm_voice_t S;
    pkt_t p;
    uint32_t b, i;
    int32_t moved = 0, worst = 0;
    swarm_packet(&p, SWARM_UNISON, 45, 0, 0, 0, 127, 0, 127);
    swarm_start(&S, &p, 7);
    for (b = 0; b < 8000u; b++) {
        int32_t out[CTL] = {0};
        swarm_render(&S, &p, out, 0, 0, 0);
        for (i = 0; i < SWARM_OSCS; i++) {
            worst = abs(S.drift[i]) > worst ? abs(S.drift[i]) : worst;
            moved += S.drift[i] != 0;
        }
    }
    CHECK(moved > 0);
    CHECK(worst <= (1008 << 8) && worst > (100 << 8));   /* 1008 is 1.54 % */
    swarm_packet(&p, SWARM_UNISON, 45, 0, 0, 0, 127, 0, 0);
    swarm_start(&S, &p, 7);
    for (b = 0; b < 2000u; b++) {
        int32_t out[CTL] = {0};
        swarm_render(&S, &p, out, 0, 0, 0);
    }
    for (i = 0; i < SWARM_OSCS; i++)
        CHECK(S.drift[i] == 0);
}

static void test_swarm_level_bound(void)
{
    static const int8_t V[3] = {0, 64, 127};
    uint32_t a, b, c, n;
    int32_t worst = 0;
    for (a = 0; a < 3u; a++)
        for (b = 0; b < 3u; b++)
            for (c = 0; c < 3u; c++) {
                pkt_t p;
                swarm_packet(&p, (int32_t)(a & 1u), 45, V[a], V[b], V[c], V[a], V[c], V[b]);
                swarm_samples(&p, swarm_buf, 4096u, 0u, 0);
                for (n = 0; n < 4096u; n++)
                    worst = abs(swarm_buf[n]) > worst ? abs(swarm_buf[n]) : worst;
            }
    CHECK(worst <= SWARM_PEAK * 4);
    CHECK(worst >= SWARM_PEAK / 2);
}

static void wave_packet(pkt_t *p, int32_t table, int32_t note, int32_t wave, int32_t warp, int32_t pitch2, int32_t cross, int32_t osc2, int32_t detune, int32_t noise)
{
    sound_init(p, 0);
    p->v[PV_ENGINE] = ENGINE_WAVE;
    p->v[PV_E0] = (int8_t)table;
    p->v[PV_NOTE] = (int8_t)note;
    p->v[PV_E1] = (int8_t)wave;
    p->v[PV_E2] = (int8_t)warp;
    p->v[PV_E3] = (int8_t)pitch2;
    p->v[PV_E4] = (int8_t)cross;
    p->v[PV_E5] = (int8_t)osc2;
    p->v[PV_E6] = (int8_t)detune;
    p->v[PV_E7] = (int8_t)noise;
}

/* timbre_step: added to the timbre at every block */
static void wave_samples(const pkt_t *p, int32_t timbre_step)
{
    static wave_voice_t W;
    uint32_t i = 0;
    int32_t timbre = 0;
    wave_start(&W, p, 777u);
    while (i < SWARM_N) {
        int32_t out[CTL] = {0};
        uint32_t s;
        wave_render(&W, p, out, 0, 0, timbre);
        timbre += timbre_step;
        for (s = 0; s < CTL && i < SWARM_N; s++)
            swarm_buf[i++] = out[s];
    }
}

static double wave_power_at(const pkt_t *p, uint32_t inc)
{
    wave_samples(p, 0);
    return buf_power_at(inc);
}

#define WAVE_HARM 0
#define WAVE_SYNC 2

static void test_wave_pitch_and_position(void)
{
    pkt_t p, q;
    uint32_t base = note_inc(57 * 16), n, crossings = 0;
    int32_t last = 0;
    wave_packet(&p, WAVE_HARM, 69, 0, 0, 0, 0, 0, 0, 0);    /* a sine: A4 is 440 Hz */
    wave_samples(&p, 0);
    for (n = 0; n < SWARM_N; n++) {
        crossings += last < 0 && swarm_buf[n] >= 0;
        last = swarm_buf[n];
    }
    CHECK(crossings > 75u && crossings < 88u);
    wave_packet(&p, WAVE_HARM, 57, 0, 0, 0, 0, 0, 0, 0);
    q = p;
    q.v[PV_E1] = 127;                                       /* WAVE adds harmonics */
    CHECK(wave_power_at(&q, base * 3u) > wave_power_at(&p, base * 3u) * 100.0);
    q.v[PV_E1] = 4;                                         /* between the first two frames: part of the second */
    CHECK(wave_power_at(&q, base * 3u) > wave_power_at(&p, base * 3u) * 100.0);
    wave_samples(&p, 64);                                   /* TIMBRE moves WAVE */
    CHECK(buf_power_at(base * 3u) > wave_power_at(&p, base * 3u) * 100.0);
}

/* a jump of WAVE, either way, crosses one frame per block and never reads outside the two frames */
static void test_wave_jump(void)
{
    static const int8_t FROM[2] = {0, 127}, TO[2] = {127, 0};
    uint32_t k, b;
    for (k = 0; k < 2u; k++) {
        pkt_t p;
        wave_voice_t W;
        int32_t worst = 0, settled = wave_position(TO[k]);
        wave_packet(&p, WAVE_HARM, 57, 0, 0, 0, 0, 0, 0, 0);
        p.v[PV_E1] = FROM[k];
        wave_start(&W, &p, 1u);
        p.v[PV_E1] = TO[k];
        for (b = 0; b < WAVE_FRAMES; b++) {
            int32_t out[CTL] = {0};
            wave_render(&W, &p, out, 0, 0, 0);
            worst = peak(out) > worst ? peak(out) : worst;
        }
        CHECK(W.position == settled);
        CHECK(worst <= WAVE_PEAK);
    }
}

/* each size's top harmonic stays under Nyquist, and the largest size that does is chosen */
static void test_wave_sizes(void)
{
    static const uint8_t NOTES[] = {12, 36, 60, 72, 84, 96, 108, 120};
    uint32_t i;
    for (i = 0; i < NELEM(NOTES); i++) {
        uint32_t inc = note_inc(NOTES[i] * 16), m = wave_size_index(inc);
        uint64_t top = (uint64_t)inc << (WAVE_TOP_BITS - 2u - m);
        CHECK(m == WAVE_SIZES - 1u || top < 0x80000000u);
        CHECK(m == 0u || (top << 1) >= 0x80000000u);
    }
    CHECK(wave_size(WAVE_DATA[0][0], 1)[0] == WAVE_DATA[0][0][512]);
    CHECK(wave_size(WAVE_DATA[0][0], 6) + 8 == WAVE_DATA[0][0] + sizeof WAVE_DATA[0][0] / sizeof(int16_t));
}

static void test_wave_band_limit(void)
{
    pkt_t p;
    uint32_t base = note_inc(96 * 16), t;
    for (t = 0; t < WAVE_TABLES; t++) {
        double strongest = 0, alias;
        uint32_t h;
        wave_packet(&p, (int32_t)t, 96, t == WAVE_HARM ? 127 : 0, 0, 0, 0, 0, 0, 0);
        wave_samples(&p, 0);
        for (h = 1; h <= 8u; h++) {
            double v = buf_power_at(base * h);
            strongest = v > strongest ? v : strongest;
        }
        alias = buf_power_at(base * 22u);
        printf("wave %u alias %.1f dB ", t, 10.0 * log10(alias / strongest + 1e-12));
        CHECK(strongest > alias * 1000.0);
    }
    printf("\n");
}

/* frames join across blocks while WAVE moves: the wave bends no harder where blocks meet than elsewhere */
static void test_wave_moves_smoothly(void)
{
    pkt_t p;
    uint32_t n;
    double at_joins = 0, elsewhere = 0;
    wave_packet(&p, WAVE_HARM, 33, 0, 0, 0, 0, 0, 0, 0);
    wave_samples(&p, 1);
    for (n = 2u; n < 4000u; n++) {
        double bend = abs(swarm_buf[n] - 2 * swarm_buf[n - 1u] + swarm_buf[n - 2u]);
        if (n % CTL < 2u)
            at_joins += bend / 2.0;
        else
            elsewhere += bend / ((double)CTL - 2.0);
    }
    printf("wave joins %.2f times the rest\n", at_joins / elsewhere);
    CHECK(at_joins < elsewhere * 1.5);
}

static void test_wave_loudness(void)
{
    pkt_t p;
    uint32_t t;
    for (t = 0; t < WAVE_TABLES; t++) {
        int32_t wave;
        double lo = 1e30, hi = 0;
        for (wave = 0; wave <= 127; wave += 17) {
            double e = 0;
            uint32_t n;
            wave_packet(&p, (int32_t)t, 57, wave, 0, 0, 0, 0, 0, 0);
            wave_samples(&p, 0);
            for (n = 0; n < SWARM_N; n++)
                e += (double)swarm_buf[n] * swarm_buf[n];
            lo = e < lo ? e : lo;
            hi = e > hi ? e : hi;
        }
        printf("wave %u spread %.1f dB ", t, 10.0 * log10(hi / lo));
        CHECK(hi < lo * 8.0);
    }
    printf("\n");
}

static void test_wave_warp(void)
{
    pkt_t p, q;
    uint32_t base = note_inc(57 * 16);
    wave_packet(&p, WAVE_HARM, 57, 0, 0, 0, 0, 0, 0, 0);
    q = p;
    q.v[PV_E2] = 100;
    CHECK(wave_power_at(&q, base * 2u) > wave_power_at(&p, base * 2u) * 100.0);
    CHECK(wave_power_at(&q, base) > wave_power_at(&q, base + (base >> 1)) * 100.0);   /* the same note */
}

static void test_wave_second_oscillator(void)
{
    pkt_t p, q;
    uint32_t base = note_inc(57 * 16), fifth = note_inc(64 * 16);
    wave_packet(&p, WAVE_HARM, 57, 0, 0, 7, 0, 127, 0, 0);
    q = p;
    q.v[PV_E5] = 0;
    CHECK(wave_power_at(&p, fifth) > wave_power_at(&q, fifth) * 100.0);          /* PITCH2 and OSC2 */
    CHECK(wave_power_at(&p, base) > wave_power_at(&p, base + (base >> 2)) * 100.0);
    q = p;
    q.v[PV_E6] = 127;                                                            /* DETUNE: up to 1 semitone */
    CHECK(wave_power_at(&q, note_inc(65 * 16)) > wave_power_at(&p, note_inc(65 * 16)) * 100.0);
    q = p;
    q.v[PV_E5] = 0;
    q.v[PV_E4] = -WAVE_CROSS_MAX;                                                /* ring: the sum and difference, no note */
    CHECK(wave_power_at(&q, base + fifth) > wave_power_at(&q, base) * 100.0);
    q.v[PV_E4] = WAVE_CROSS_MAX;                                                 /* FM: sidebands, and OSC2 stays silent */
    CHECK(wave_power_at(&q, base + fifth) > wave_power_at(&p, base + fifth) * 100.0);
    CHECK(wave_power_at(&p, base + fifth) * 100.0 < wave_power_at(&p, base));
    q = p;
    q.v[PV_E5] = 0;
    q.v[PV_E7] = 127;                                                            /* NOISE */
    p.v[PV_E5] = 0;
    CHECK(wave_power_at(&q, base * 7u + (base >> 1)) > wave_power_at(&p, base * 7u + (base >> 1)) * 100.0);
}

static void test_wave_level_bound(void)
{
    static const int8_t V[2] = {0, 127};
    uint32_t t, a, b, n;
    int32_t worst = 0;
    for (t = 0; t < WAVE_TABLES; t++)
        for (a = 0; a < 2u; a++)
            for (b = 0; b < 2u; b++) {
                pkt_t p;
                wave_packet(&p, (int32_t)t, 45, V[a], V[b], 0, a ? WAVE_CROSS_MAX : -WAVE_CROSS_MAX, 127, 127, V[b]);
                wave_samples(&p, 0);
                for (n = 0; n < SWARM_N; n++)
                    worst = abs(swarm_buf[n]) > worst ? abs(swarm_buf[n]) : worst;
            }
    printf("wave peak %d\n", worst);
    CHECK(worst <= WAVE_PEAK * 3);
    CHECK(worst >= WAVE_PEAK / 2);
}

/* a slot reaches each engine's main value */
static void test_timbre_destination(void)
{
    pkt_t p;
    int32_t l[CTL], r[CTL], d[CTL], w[CTL];
    uint32_t b, i;
    fm_packet(&p, 0, 45, 2, 3, 3, 0, 0, 0, 0);
    p.v[PV_MOD_S] = 127;
    p.v[PV_SLOT_SRC3] = MOD_SRC_VEL;
    p.v[PV_SLOT_DST3] = MOD_DST_TIMBRE;
    p.v[PV_SLOT_AMT3] = 127;
    memset(voices, 0, sizeof voices);
    voice_trigger(&p, KEY_NONE, 0);
    for (b = 0; b < 8u; b++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        voices_render(l, r, d, w);
    }
    CHECK(voices[0].chain.off[MOD_DST_TIMBRE] >> 8 > 40);
    CHECK(voices[0].engine.fm.gain_a > 1000);
    perc_packet(&p, PERC_SKIN, 45, 110, 64, 0, 0, 0, 0, 0);
    p.v[PV_SLOT_SRC3] = MOD_SRC_VEL;
    p.v[PV_SLOT_DST3] = MOD_DST_TIMBRE;
    p.v[PV_SLOT_AMT3] = 127;
    memset(voices, 0, sizeof voices);
    voice_trigger(&p, KEY_NONE, 0);
    for (b = 0; b < 8u; b++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        voices_render(l, r, d, w);
    }
    CHECK(voices[0].engine.perc.fm_gain > 1000);
    wave_packet(&p, WAVE_HARM, 45, 0, 0, 0, 0, 0, 0, 0);
    p.v[PV_SLOT_SRC3] = MOD_SRC_VEL;
    p.v[PV_SLOT_DST3] = MOD_DST_TIMBRE;
    p.v[PV_SLOT_AMT3] = 127;
    memset(voices, 0, sizeof voices);
    voice_trigger(&p, KEY_NONE, 0);
    for (b = 0; b < 8u; b++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        voices_render(l, r, d, w);
    }
    CHECK(voices[0].engine.wave.position > 0);
}

static uint32_t count_active_key(uint32_t key)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += voices[i].active && voices[i].key == key && voices[i].chain.amp_env.stage < ENV_RELEASE;
    return n;
}

static void test_hold(void)
{
    static pattern_t pat;
    pkt_t p, other;
    int32_t l[CTL], r[CTL], d[CTL], w[CTL];
    uint32_t b, i, k;
    swarm_packet(&p, SWARM_UNISON, 36, 40, 0, 0, 80, 0, 0);
    p.v[PV_LEN] = (int8_t)LEN_HOLD;
    other = p;
    other.v[PV_NOTE] = 48;
    memset(voices, 0, sizeof voices);
    voice_trigger_step(&p, 2, 5000u, 1u);
    for (b = 0; b < 3000u; b++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        voices_render(l, r, d, w);
    }
    CHECK(count_active_key(KEY_TRACK(2)) == 1u);        /* far past any step count */
    voice_trigger_step(&p, 2, 5000u, 1u);
    CHECK(voices_active() == 1u);                       /* the same note keeps sounding */
    voice_trigger_step(&p, 3, 5000u, 1u);
    CHECK(count_active_key(KEY_TRACK(2)) == 1u && count_active_key(KEY_TRACK(3)) == 1u);   /* another track's note leaves it */
    voice_trigger_step(&other, 2, 5000u, 1u);
    CHECK(count_active_key(KEY_TRACK(2)) == 1u && voices_active() == 3u);    /* a new note: the old one is in release */
    p.v[PV_LEN] = 1;
    voice_trigger_step(&p, 2, 5000u, 1u);
    CHECK(count_active_key(KEY_TRACK(2)) == 0u);        /* so does a note of a set length */
    for (k = 0; k < NTRK; k++)
        track_init(&pat.track[k], 0);
    pat.track[3].steps[0].on = 1;
    for (b = 0; b < HOLD_CHECK_BLOCKS * (NTRK + 1u); b++)
        voices_hold_service(&pat);
    CHECK(count_active_key(KEY_TRACK(3)) == 1u);        /* its track has a step on */
    pat.track[3].steps[0].on = 0;
    for (b = 0; b < HOLD_CHECK_BLOCKS * (NTRK + 1u); b++)
        voices_hold_service(&pat);
    CHECK(count_active_key(KEY_TRACK(3)) == 0u);        /* now nothing else would end it */
}

/* the peak of the voice's output over 16 blocks, from block at */
static int32_t voice_peak_at(const pkt_t *p, uint32_t at)
{
    int32_t l[CTL], r[CTL], d[CTL], w[CTL], peak = 0;
    uint32_t b, i;
    memset(voices, 0, sizeof voices);
    voice_trigger(p, KEY_NONE, 0);
    for (b = 0; b < at + 16u; b++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        voices_render(l, r, d, w);
        for (i = 0; b >= at && i < CTL; i++)
            peak = abs(l[i]) > peak ? abs(l[i]) : peak;
    }
    return peak;
}

static uint32_t perc_voice_life(int32_t decay, int32_t sustain, int32_t release, uint32_t hold)
{
    int32_t l[CTL], r[CTL], d[CTL], w[CTL];
    pkt_t p;
    uint32_t b = 0, i;
    perc_packet(&p, PERC_SKIN, 45, decay, 0, 0, 0, 0, 0, 0);
    p.v[PV_AMP_S] = (int8_t)sustain;
    p.v[PV_AMP_R] = (int8_t)release;
    memset(voices, 0, sizeof voices);
    voice_trigger(&p, KEY_NONE, hold);
    while (voices_active() && b < 400000u) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        voices_render(l, r, d, w);
        b++;
    }
    return b;
}

/* rising zero crossings of the voice's output over the given blocks */
static uint32_t voice_crossings_at(const pkt_t *p, uint32_t at, uint32_t blocks)
{
    int32_t l[CTL], r[CTL], d[CTL], w[CTL], last = 0;
    uint32_t b, i, n = 0;
    memset(voices, 0, sizeof voices);
    voice_trigger(p, KEY_NONE, 0);
    for (b = 0; b < at + blocks; b++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        voices_render(l, r, d, w);
        for (i = 0; b >= at && i < CTL; i++) {
            n += last < 0 && l[i] >= 0;
            last = l[i];
        }
    }
    return n;
}

/* SHAPE folds at low values and keeps the level and the decay, on any engine. Without the follower a fold stays at full
 * scale until the driven signal falls under it */
static void test_shape_keeps_punch(void)
{
    pkt_t dry, folded, fm_dry, fm_folded;
    uint32_t mid = env_blocks(100) / 4u;
    int32_t dry_peak, fold_peak;
    perc_packet(&dry, PERC_SKIN, 45, 100, 0, 0, 0, 0, 0, 0);
    folded = dry;
    folded.v[PV_SHAPER] = 127;
    dry_peak = voice_peak_at(&dry, mid);
    fold_peak = voice_peak_at(&folded, mid);
    CHECK(voice_peak_at(&folded, 60u) > fold_peak * 2);     /* folded output still falls */
    CHECK(fold_peak < dry_peak * 3 / 2 && fold_peak > dry_peak / 3);
    CHECK(voice_crossings_at(&folded, 60u, 60u) > voice_crossings_at(&dry, 60u, 60u) * 2u);   /* and folding adds harmonics */
    mid = env_blocks(100) * 4u / 5u;                        /* near the end they meet again */
    fold_peak = voice_peak_at(&folded, mid);
    dry_peak = voice_peak_at(&dry, mid);
    CHECK(fold_peak < dry_peak * 2 + 20 && fold_peak * 2 + 20 > dry_peak);
    {
        chain_t c;
        int32_t x[CTL];
        uint32_t i, b;
        folded = dry;
        folded.v[PV_SHAPER] = 30;                           /* 1.8 times a full engine signal folds back toward 0 */
        chain_start(&c, &folded);
        for (b = 0; b < 2u; b++) {
            for (i = 0; i < CTL; i++)
                x[i] = 8000;
            chain_effects(&c, &c.path[0], &folded, x);
        }
        CHECK(abs(x[CTL - 1u]) < 4000);
    }
    fm_packet(&fm_dry, 7, 45, 2, 2, 2, 0, 0, 0, 0);
    fm_folded = fm_dry;
    fm_folded.v[PV_SHAPER] = 127;
    CHECK(voice_crossings_at(&fm_folded, 60u, 60u) > voice_crossings_at(&fm_dry, 60u, 60u) * 2u);
    CHECK(voice_peak_at(&fm_folded, 60u) < voice_peak_at(&fm_dry, 60u) * 3 / 2);
}

/* a Perc voice takes the amp envelope as any engine does */
static void test_perc_amp_envelope(void)
{
    pkt_t p;
    int32_t fast, slow, early, mid;
    uint32_t full = env_blocks(100), once = perc_voice_life(100, 0, 127, 8u), held = perc_voice_life(100, 0, 127, 50u);
    CHECK(once * 10u > full * 7u && once * 10u < full * 13u);       /* SUS 0 and a long REL: a one-shot */
    CHECK(held * 10u > once * 9u && held * 10u < once * 11u);       /* whatever the note length */
    CHECK(perc_voice_life(100, 0, 20, 50u) < 50u + env_blocks(20) * 2u + 8u);       /* a short REL gates it */
    CHECK(perc_voice_life(100, 0, 70, 50u) < perc_voice_life(100, 0, 90, 50u));     /* REL sets the note's end */
    CHECK(perc_voice_life(40, 64, 40, 5000u) >= 5000u);             /* SUS holds until the note ends */
    CHECK(perc_voice_life(40, 64, 40, 5000u) < 5000u + env_blocks(40) * 2u + 8u);
    perc_packet(&p, PERC_SKIN, 45, 100, 0, 0, 0, 0, 0, 0);
    fast = voice_peak_at(&p, 8u);
    p.v[PV_AMP_A] = 90;
    slow = voice_peak_at(&p, 8u);
    CHECK(slow * 4 < fast);                             /* ATK rises the voice */
    p.v[PV_AMP_A] = 0;
    early = voice_peak_at(&p, 8u);
    mid = voice_peak_at(&p, full / 2u);
    CHECK(mid * 100 > early && mid * 8 < early);        /* -30 dB at half of DEC: the fall applies once */
}

/* blocks from trigger until the voice ends, with a note of hold blocks */
static uint32_t voice_life(int32_t engine, int32_t model, int32_t release, uint32_t hold)
{
    int32_t l[CTL], r[CTL], d[CTL], w[CTL];
    pkt_t p;
    uint32_t b = 0, i;
    memset(voices, 0, sizeof voices);
    sound_init(&p, 0);
    p.v[PV_ENGINE] = (int8_t)engine;
    p.v[PV_E0] = (int8_t)model;
    p.v[PV_NOTE] = 48;
    p.v[PV_AMP_S] = 127;
    p.v[PV_AMP_R] = (int8_t)release;
    voice_trigger(&p, KEY_NONE, hold);
    while (voices_active() && b < 200000u) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        voices_render(l, r, d, w);
        b++;
    }
    return b;
}

/* velocity sets the amp envelope's peak on each engine, apart from level */
static void test_velocity_scales_peak(void)
{
    pkt_t p;
    int32_t full, half, quiet;
    fm_packet(&p, 0, 45, 2, 3, 3, 0, 0, 0, 0);
    p.v[PV_VEL] = 127;
    full = voice_peak_at(&p, 20u);
    p.v[PV_VEL] = 64;
    half = voice_peak_at(&p, 20u);
    p.v[PV_VEL] = 0;
    quiet = voice_peak_at(&p, 20u);
    CHECK(half * 45 > full * 10 && half * 35 < full * 10);
    CHECK(quiet == 0);
    p.v[PV_VEL] = 127;
    p.v[PV_LEVEL] = 64;
    CHECK(voice_peak_at(&p, 20u) < full / 4);           /* level still attenuates */
    swarm_packet(&p, SWARM_UNISON, 45, 0, 0, 0, 0, 0, 0);
    p.v[PV_VEL] = 127;
    full = voice_peak_at(&p, 20u);
    p.v[PV_VEL] = 64;
    half = voice_peak_at(&p, 20u);
    CHECK(half * 45 > full * 10 && half * 35 < full * 10);
    perc_packet(&p, PERC_SKIN, 45, 110, 64, 0, 0, 0, 0, 0);
    p.v[PV_VEL] = 127;
    full = voice_peak_at(&p, 20u);
    p.v[PV_VEL] = 64;
    half = voice_peak_at(&p, 20u);
    CHECK(half * 45 > full * 10 && half * 35 < full * 10);
}

/* a note ends by its amp release, on every engine that takes note length */
static void test_release_sets_note_end(void)
{
    static const int8_t ENGINE[][2] = {{ENGINE_FM, 0}, {ENGINE_FM, 3}, {ENGINE_FM, 7}, {ENGINE_SWARM, 0}, {ENGINE_WAVE, 0}};
    uint32_t e;
    const uint32_t hold = 100u;
    for (e = 0; e < NELEM(ENGINE); e++) {
        uint32_t quick = voice_life(ENGINE[e][0], ENGINE[e][1], 20, hold), slow = voice_life(ENGINE[e][0], ENGINE[e][1], 100, hold);
        CHECK(quick >= hold && quick < hold + env_blocks(20) * 2u + 8u);
        CHECK(slow >= hold + env_blocks(100) / 2u && slow < hold + env_blocks(100) * 2u);
    }
}

/* the time table means -60 dB for decay and release, and the whole rise for attack */
static void test_envelope_times(void)
{
    static const int8_t V[] = {40, 64, 80, 100, 127};
    uint32_t i;
    for (i = 0; i < NELEM(V); i++) {
        env_t e;
        uint32_t n = 0, want = env_blocks(V[i]);
        env_start(&e, 0, V[i], 0, 0);
        while (e.stage == ENV_ATTACK)
            env_block(&e);
        while (e.stage == ENV_DECAY) {
            env_block(&e);
            n++;
        }
        CHECK(n * 10u > want * 7u && n * 10u < want * 13u);
        env_start(&e, 0, 0, 127, V[i]);
        while (e.stage != ENV_SUSTAIN)
            env_block(&e);
        env_release(&e);
        n = 0;
        while (e.stage != ENV_DONE) {
            env_block(&e);
            n++;
        }
        CHECK(n * 10u > want * 7u && n * 10u < want * 13u);
    }
}

/* with every voice taken, a trigger takes one of lower priority, and never one of higher priority */
static void test_priority_across_voices(void)
{
    pkt_t low, high;
    uint32_t i, n_low = 0, n_high = 0, dropped;
    int32_t l[CTL], r[CTL], d[CTL], w[CTL];
    perc_packet(&low, PERC_SKIN, 45, 110, 0, 0, 0, 0, 0, 0);
    low.v[PV_PRIO] = PRIO_LOW;
    high = low;
    high.v[PV_PRIO] = PRIO_HIGH;
    high.v[PV_NOTE] = 60;
    memset(voices, 0, sizeof voices);
    for (i = 0; i < NVOICE; i++)
        voice_trigger(&low, KEY_NONE, 0);
    for (i = 0; i < CTL; i++)
        l[i] = r[i] = d[i] = w[i] = 0;
    voices_render(l, r, d, w);
    voice_trigger(&high, KEY_NONE, 0);
    voices_render(l, r, d, w);
    voices_render(l, r, d, w);
    for (i = 0; i < NVOICE; i++) {
        n_low += voices[i].active && voices[i].pkt.v[PV_PRIO] == PRIO_LOW;
        n_high += voices[i].active && voices[i].pkt.v[PV_PRIO] == PRIO_HIGH;
    }
    CHECK(n_high == 1u && n_low == NVOICE - 1u);
    memset(voices, 0, sizeof voices);
    for (i = 0; i < NVOICE; i++)
        voice_trigger(&high, KEY_NONE, 0);
    voices_render(l, r, d, w);
    dropped = voices_dropped;
    voice_trigger(&low, KEY_NONE, 0);
    CHECK(voices_dropped == dropped + 1u);
}

/* a lower priority never costs a higher one a trigger, even one that has not sounded yet */
static void test_low_priority_keeps_out(void)
{
    static const uint32_t KEEP[2] = {PRIO_NORMAL, PRIO_HIGH};
    pkt_t p;
    uint32_t i, b, dropped;
    int32_t l[CTL], r[CTL], d[CTL], w[CTL];
    voice_cost[0] = 100u;
    voice_budget = 200u;
    fm_packet(&p, 0, 45, 2, 3, 3, 0, 0, 0, 0);
    memset(voices, 0, sizeof voices);
    for (i = 0; i < 2u; i++) {
        p.v[PV_PRIO] = (int8_t)KEEP[i];
        voice_trigger(&p, KEY_NONE, 0);
    }
    p.v[PV_PRIO] = PRIO_XLOW;
    dropped = voices_dropped;
    voice_trigger(&p, KEY_NONE, 0);                     /* in the same block: neither has rendered */
    CHECK(!voice_fading(&voices[0]) && !voice_fading(&voices[1]));
    CHECK(voices_dropped == dropped + 1u);              /* and the lost trigger is counted */
    p.v[PV_AMP_A] = 100;                                /* a slow attack starts quiet too */
    memset(voices, 0, sizeof voices);
    p.v[PV_PRIO] = PRIO_NORMAL;
    voice_trigger(&p, KEY_NONE, 0);
    voice_trigger(&p, KEY_NONE, 0);
    for (b = 0; b < 4u; b++) {
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        voices_render(l, r, d, w);
    }
    p.v[PV_PRIO] = PRIO_XLOW;
    voice_trigger(&p, KEY_NONE, 0);
    CHECK(!voice_fading(&voices[0]) && !voice_fading(&voices[1]));
    p.v[PV_PRIO] = PRIO_HIGH;                           /* a higher one still gets in, and that lost note is counted */
    dropped = voices_stolen;
    voice_trigger(&p, KEY_NONE, 0);
    CHECK(voice_fading(&voices[0]) && voices_stolen == dropped + 1u);
    voice_budget = 0xFFFFFFFFu;
    voice_cost[0] = 0u;
    memset(voices, 0, sizeof voices);
}

static void test_steal_order(void)
{
    steal_t v[3];
    memset(v, 0, sizeof v);
    v[0] = (steal_t){1, PRIO_NORMAL, 20000, 1, 0};
    v[1] = (steal_t){1, PRIO_NORMAL, 10000, 2, 0};
    v[2] = (steal_t){1, PRIO_NORMAL, 30000, 3, 0};
    CHECK(voice_victim(v, 3, PRIO_NORMAL) == 0);        /* equal priority: oldest, not quietest */
    CHECK(voice_victim(v, 3, PRIO_LOW) == -1);          /* never from a higher priority */
    CHECK(voice_victim(v, 3, PRIO_HIGH) == 1);          /* lower priority: quietest */
    v[2].prio = PRIO_LOW;
    CHECK(voice_victim(v, 3, PRIO_HIGH) == 2);          /* the lowest priority goes first */
    CHECK(voice_victim(v, 3, PRIO_NORMAL) == 2);
    v[1] = (steal_t){1, PRIO_HIGH, 100, 2, 0};
    CHECK(voice_victim(v, 3, PRIO_LOW) == 2);           /* nearly silent is still safe from a lower priority */
    CHECK(voice_victim(v, 3, PRIO_HIGH) == 1);          /* it goes first at its own priority */
    v[1].rising = 1;
    CHECK(voice_victim(v, 3, PRIO_XXHIGH) == 2);        /* quiet in its attack has not ended: lower priorities go by rank */
    v[1].rising = 0;
    v[1].stealable = 0;
    CHECK(voice_victim(v, 3, PRIO_LOW) == 2);           /* a voice already fading cannot be taken twice */
}

static void test_master_bound(void)
{
    master_t m = MASTER_INIT;
    uint32_t i;
    int32_t after = 0;
    for (i = 0; i < 2000u; i++) {
        int32_t l = i & 1u ? 500000 : -500000, r = l;
        master_sample(&m, &l, &r);
        CHECK(l <= MASTER_CEILING && l >= -MASTER_CEILING);
        if (i > 100u && (l < 0 ? -l : l) > after)
            after = l < 0 ? -l : l;
    }
    CHECK(after <= MASTER_LIMIT);                       /* the limiter holds a sustained overload */
    {
        master_t q = MASTER_INIT;
        int32_t l = 1000, r = -1000;
        master_sample(&q, &l, &r);
        CHECK(l > 900 && l <= 1000 && r == -l);         /* quiet signals pass nearly unchanged */
    }
}

static int8_t arp_heard[64];
static uint32_t arp_heard_block[64], arp_heard_hold[64], arp_heard_n, arp_now;
static int8_t arp_heard_cut[64];

static void arp_capture(const pkt_t *p, uint32_t hold_blocks)
{
    arp_heard[arp_heard_n] = p->v[PV_NOTE];
    arp_heard_cut[arp_heard_n] = p->v[PV_FLT_CUT];
    arp_heard_block[arp_heard_n] = arp_now;
    arp_heard_hold[arp_heard_n++] = hold_blocks;
}

static void arp_run(arp_t *a, uint32_t blocks, uint32_t samples_per_step)
{
    uint32_t i;
    for (i = 0; i < blocks; i++, arp_now++)
        arp_block(a, CTL, samples_per_step, arp_capture);
}

static void arp_start(arp_t *a, pkt_t *p, int32_t mode, int32_t range)
{
    memset(a, 0, sizeof *a);
    arp_heard_n = 0;
    arp_now = 0;
    sound_init(p, 0);
    p->v[PV_ARP_MODE] = (int8_t)mode;
    p->v[PV_ARP_RANGE] = (int8_t)range;
}

static void arp_add(arp_t *a, pkt_t *p, int32_t note)
{
    p->v[PV_NOTE] = (int8_t)note;
    arp_note_on(a, p, ARP_OWNER_STEP, 3200u * 40u, 0);
}

#define ARP_SPS 3200u   /* 100 blocks */

static void test_arp_modes(void)
{
    static const struct { int32_t mode, range; int8_t want[9]; } CASES[] = {
        {ARP_UP, 1, {60, 64, 67, 60, 64, 67, 60, 64, 67}},
        {ARP_DOWN, 1, {67, 64, 60, 67, 64, 60, 67, 64, 60}},
        {ARP_UPDOWN, 1, {60, 64, 67, 64, 60, 64, 67, 64, 60}},
        {ARP_PLAYED, 1, {64, 60, 67, 64, 60, 67, 64, 60, 67}},
        {ARP_UP, 2, {60, 64, 67, 72, 76, 79, 60, 64, 67}},
        {ARP_DOWN, 2, {79, 76, 72, 67, 64, 60, 79, 76, 72}},
        {ARP_UPDOWN, 2, {60, 64, 67, 72, 76, 79, 76, 72, 67}},
    };
    arp_t a;
    pkt_t p;
    uint32_t c, i;
    for (c = 0; c < NELEM(CASES); c++) {
        arp_start(&a, &p, CASES[c].mode, CASES[c].range);
        arp_add(&a, &p, 64);
        arp_add(&a, &p, 60);
        arp_add(&a, &p, 67);
        arp_run(&a, 9u * ARP_SPS / CTL, ARP_SPS);
        CHECK(arp_heard_n >= 9u);
        for (i = 0; i < 9u; i++)
            CHECK(arp_heard[i] == CASES[c].want[i]);
    }
}

static void test_arp_random_stays_in_the_run(void)
{
    arp_t a;
    pkt_t p;
    uint32_t i, seen = 0;
    arp_start(&a, &p, ARP_RANDOM, 2);
    arp_add(&a, &p, 60);
    arp_add(&a, &p, 64);
    arp_run(&a, 30u * ARP_SPS / CTL, ARP_SPS);
    for (i = 0; i < arp_heard_n; i++) {
        CHECK(arp_heard[i] == 60 || arp_heard[i] == 64 || arp_heard[i] == 72 || arp_heard[i] == 76);
        seen |= 1u << ((arp_heard[i] - 60) / 4);
    }
    CHECK(seen == (1u << 0 | 1u << 1 | 1u << 3 | 1u << 4));    /* all four come up */
}

static void test_arp_rate_and_gate(void)
{
    arp_t a;
    pkt_t p;
    arp_start(&a, &p, ARP_UP, 1);
    p.v[PV_ARP_RATE] = ARP_RATE_1_8T;
    p.v[PV_ARP_GATE] = 127;
    arp_add(&a, &p, 60);
    arp_run(&a, 3u * ARP_SPS * 4u / CTL, ARP_SPS);
    CHECK(arp_heard_block[0] == 0u);                    /* at once */
    CHECK(arp_heard_n == 9u);                           /* 12 steps of 1/8T: 3 hits in 4 steps, with no drift */
    CHECK(arp_heard_block[3] == 400u);
    CHECK(arp_heard_block[6] == 800u);
    CHECK(arp_heard_hold[0] == ARP_SPS * 8u / 6u / CTL);
    arp_start(&a, &p, ARP_UP, 1);
    p.v[PV_ARP_GATE] = 0;
    arp_add(&a, &p, 60);
    arp_run(&a, 1u, ARP_SPS);
    CHECK(arp_heard_hold[0] == 1u);                     /* 0 would mean no end */
}

static void test_arp_note_length(void)
{
    arp_t a;
    pkt_t p;
    arp_start(&a, &p, ARP_UP, 1);
    p.v[PV_NOTE] = 60;
    arp_note_on(&a, &p, ARP_OWNER_STEP, ARP_SPS * 4u, 0);
    arp_run(&a, 10u * ARP_SPS / CTL, ARP_SPS);
    CHECK(arp_heard_n == 4u);                           /* LEN 4 at 1/16 */
    CHECK(a.n == 0u);
}

/* a tempo with a step that is not a whole number of blocks: each note on a LEN 1 step must find the arp empty and start from the bottom */
static void test_arp_one_step_notes_restart(void)
{
    arp_t a;
    pkt_t p;
    uint32_t sps, acc = 0, b, i;
    for (sps = 2205u; sps < 22050u; sps += 397u) {
        arp_start(&a, &p, ARP_UP, 2);
        p.v[PV_NOTE] = 60;
        acc = 0;
        for (b = 0; b < 40u * sps / CTL && arp_heard_n < 32u; b++, arp_now++) {
            if (acc < CTL) {
                arp_note_on(&a, &p, ARP_OWNER_STEP, sps, 0);
                acc += sps;
            }
            acc -= CTL;
            arp_block(&a, CTL, sps, arp_capture);
        }
        CHECK(arp_heard_n >= 16u);
        for (i = 0; i < arp_heard_n; i++)
            CHECK(arp_heard[i] == 60);
    }
}

static void test_arp_overlap_and_limit(void)
{
    arp_t a;
    pkt_t p;
    uint32_t i;
    arp_start(&a, &p, ARP_UP, 1);
    for (i = 0; i < 9u; i++)
        arp_add(&a, &p, 60 + (int32_t)i);
    CHECK(a.n == ARP_NOTES);
    CHECK(a.note[0].pkt.v[PV_NOTE] == 61);              /* the oldest went */
    arp_add(&a, &p, 64);
    CHECK(a.n == ARP_NOTES);                            /* the same pitch does not double */
    CHECK(a.note[ARP_NOTES - 1u].pkt.v[PV_NOTE] == 64);
}

static void test_arp_hits_keep_their_locks(void)
{
    arp_t a;
    pkt_t p;
    arp_start(&a, &p, ARP_UP, 1);
    p.v[PV_FLT_CUT] = 11;
    arp_add(&a, &p, 60);
    p.v[PV_FLT_CUT] = 22;
    arp_add(&a, &p, 67);
    arp_run(&a, 2u * ARP_SPS / CTL, ARP_SPS);
    CHECK(arp_heard[0] == 60 && arp_heard_cut[0] == 11);
    CHECK(arp_heard[1] == 67 && arp_heard_cut[1] == 22);
}

static void test_arp_pitch_stops_at_127(void)
{
    arp_t a;
    pkt_t p;
    arp_start(&a, &p, ARP_UP, 4);
    arp_add(&a, &p, 120);
    arp_run(&a, 4u * ARP_SPS / CTL, ARP_SPS);
    CHECK(arp_heard[3] == 127);
}

static void test_arp_keys_and_latch(void)
{
    arp_t a;
    pkt_t p;
    arp_start(&a, &p, ARP_UP, 1);
    p.v[PV_NOTE] = 60;
    arp_note_on(&a, &p, 3, 0, 0);
    arp_key_off(&a, 3);
    CHECK(a.n == 0u);                                   /* no latch: the key ends it */
    p.v[PV_ARP_LATCH] = 1;
    arp_note_on(&a, &p, 3, 0, 0);
    p.v[PV_NOTE] = 64;
    arp_note_on(&a, &p, 5, 0, 0);
    arp_key_off(&a, 3);
    arp_key_off(&a, 5);
    CHECK(a.n == 2u);                                   /* latched */
    p.v[PV_NOTE] = 67;
    arp_note_on(&a, &p, 7, 0, 0);
    CHECK(a.n == 1u && a.note[0].pkt.v[PV_NOTE] == 67); /* a press with no key down replaces them */
    p.v[PV_NOTE] = 69;
    arp_note_on(&a, &p, 8, 0, 0);
    CHECK(a.n == 2u);                                   /* a key joining one that is down adds to it */
    arp_key_off(&a, 7);
    arp_key_off(&a, 8);
    arp_remove_released(&a);
    CHECK(a.n == 0u);                                   /* latch turned off */
}

static void test_arp_held_note(void)
{
    arp_t a;
    pkt_t p;
    arp_start(&a, &p, ARP_UP, 1);
    p.v[PV_NOTE] = 60;
    arp_note_on(&a, &p, ARP_OWNER_STEP, 0, 0);
    arp_run(&a, 50u, ARP_SPS);
    p.v[PV_NOTE] = 60;
    arp_note_on(&a, &p, ARP_OWNER_STEP, 0, 0);
    CHECK(a.n == 1u);
    arp_run(&a, 51u, ARP_SPS);
    CHECK(arp_heard_n == 2u);                           /* the same note looping keeps the arp's clock */
    CHECK(arp_heard_block[1] == 100u);
    p.v[PV_NOTE] = 67;
    arp_note_on(&a, &p, ARP_OWNER_STEP, 0, 0);
    CHECK(a.n == 1u && a.note[0].pkt.v[PV_NOTE] == 67); /* the next note ends a held one */
    arp_end_held(&a);
    CHECK(a.n == 0u);
}

/* the path from a step's trigger to the voices */
static void test_arp_voice_path(void)
{
    pkt_t p;
    memset(voices, 0, sizeof voices);
    memset(arps, 0, sizeof arps);
    sound_init(&p, 0);
    p.v[PV_LEN] = 4;
    p.v[PV_ARP_MODE] = ARP_UP;
    CHECK(voice_trigger_step(&p, 2, 3200u, 1u) == 4u);
    CHECK(voices_active() == 0u && arps[2].n == 1u);    /* the arp's hit starts the voice, not the step */
    arp_block(&arps[2], CTL, 3200u, voice_arp_hit);
    CHECK(voices_active() == 1u);
    CHECK(voices[0].key == KEY_NONE && voices[0].hold_blocks == 3200u * 64u / 127u / CTL);
    voices_silence();
    CHECK(arps[2].n == 0u);
    p.v[PV_LEN] = (int8_t)LEN_HOLD;
    voice_trigger_step(&p, 2, 3200u, 1u);
    CHECK(arps[2].n == 1u);
    p.v[PV_ARP_MODE] = ARP_OFF;
    memset(voices, 0, sizeof voices);
    voice_trigger_step(&p, 2, 3200u, 1u);
    CHECK(arps[2].n == 0u);                             /* a step with the arp off is the track's next note */
    CHECK(voices[0].key == KEY_TRACK(2));
}

int main(void)
{
    test_resolve_layers();
    test_locks_stay_sorted();
    test_lock_capacity();
    test_polymeter();
    test_step_inherits_from_sound();
    test_step_locked();
    test_sound_use_count();
    test_tracks_free_run();
    test_switch_at_longest();
    test_play_counter();
    test_played_step_skips_once();
    test_fired_step();
    test_nearest_step();
    test_overdub_knobs();
    test_chance();
    test_tempo();
    test_envelope();
    test_pan_and_level();
    test_filter();
    test_effects();
    test_env_curve();
    test_meter_bar();
    test_lfo_rate_doubles();
    test_lfo_shapes();
    test_lfo_rises_from_start();
    test_lfo_modes();
    test_lfo_sample_hold();
    test_mod_routes();
    test_mod_opens_filter();
    test_level_ramp_has_no_step();
    test_delay_repeats_at_its_time();
    test_delay_decays_at_full_feedback();
    test_delay_time_glides();
    test_reverb_tail();
    test_perc_pitch();
    test_perc_bend();
    test_perc_spread_and_harm();
    test_perc_decay_ends_the_voice();
    test_perc_harm_shortens_upper_partials();
    test_perc_level_bound();
    test_wavefold();
    test_perc_noise_seed();
    test_perc_click();
    test_perc_modes_set_the_partials();
    test_perc_fm();
    test_perc_noise_fall();
    test_perc_noise_ignores_sustain();
    test_fm_routes_run_forward();
    test_fm_loops_match_routes();
    test_fm_sound();
    test_fm_mod_envelope();
    test_fm_level_bound();
    test_fm_removes_offset();
    test_shape_keeps_punch();
    test_swarm_pitch_and_shape();
    test_swarm_shape_keeps_loudness();
    test_swarm_spread_mode_mix_sub();
    test_swarm_sub_is_separate();
    test_swarm_band_limit();
    test_swarm_shift();
    test_swarm_lines_join();
    test_swarm_edge_across_blocks();
    test_swarm_charge();
    test_swarm_drift();
    test_swarm_level_bound();
    test_wave_pitch_and_position();
    test_wave_jump();
    test_wave_sizes();
    test_wave_band_limit();
    test_wave_moves_smoothly();
    test_wave_loudness();
    test_wave_warp();
    test_wave_second_oscillator();
    test_wave_level_bound();
    test_timbre_destination();
    test_hold();
    test_perc_amp_envelope();
    test_velocity_scales_peak();
    test_release_sets_note_end();
    test_envelope_times();
    test_low_priority_keeps_out();
    test_steal_order();
    test_priority_across_voices();
    test_master_bound();
    test_master_log_exp();
    test_master_neutral();
    test_master_compressor_static();
    test_master_compressor_times();
    test_master_compressor_off();
    test_master_tilt();
    test_master_width();
    test_master_drive();
    test_master_extremes_bound();
    test_master_volume_last();
    test_mod_random_source();
    test_mod_lfo_rate_route();
    test_fm_index_route_keeps_sound();
    test_fm_index_routes();
    test_lfo_sync();
    test_lfo_shared_sample_hold();
    test_follower_send();
    test_follower_ducks_any_voice();
    test_follower_times();
    test_mod_each_lfo_is_its_own_source();
    test_mod_route_slot();
    test_mod_dst_of_value();
    test_seq_late_step();
    test_seq_early_step();
    test_seq_early_step_edits();
    test_seq_early_step_pattern_change();
    test_seq_ratchet();
    test_step_timing_clears();
    test_seq_swing();
    test_seq_human();
    test_seq_offset_limit();
    test_voice_ratchet_gate();
    test_delay_ping_pong();
    test_delay_wraps();
    test_delay_width_0_matches_one_read();
    test_voice_budget_counts_fades();
    test_voice_higher_trigger_takes_waiting_note();
    test_voice_high_trigger_replaces_lowest_waiting();
    test_voice_high_trigger_replaces_several();
    test_stereo_engines();
    test_stereo_swarm_sides();
    test_stereo_sends_and_follower();
    test_stereo_center_and_balance();
    test_stereo_only_with_two_sources();
    test_stereo_keeps_the_signal();
    test_stereo_filters_each_channel();
    test_stop_and_start();
    test_engine_memory();
    test_voices_silence();
    test_arp_modes();
    test_arp_random_stays_in_the_run();
    test_arp_rate_and_gate();
    test_arp_note_length();
    test_arp_one_step_notes_restart();
    test_arp_overlap_and_limit();
    test_arp_hits_keep_their_locks();
    test_arp_pitch_stops_at_127();
    test_arp_keys_and_latch();
    test_arp_held_note();
    test_arp_voice_path();
    test_step_copy();
    test_step_clear_locks();
    printf("sizeof(pattern_t) %zu, 27 patterns %zu KB, pool %zu B\n", sizeof(pattern_t),
           NPATTERN * sizeof(pattern_t) / 1024, sizeof pool);
    printf(fails ? "FAILED: %d\n" : "ok\n", fails);
    return fails != 0;
}
