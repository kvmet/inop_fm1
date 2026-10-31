/* SPDX-License-Identifier: GPL-3.0-only */
/* Host tests of the INOP model and sequencer.
 *   cc -std=c11 -Wall -Wextra -Wno-unused-function -Ifirmware/inop -Ifirmware/src -Ibuild/gen tests/inop_test.c -o /tmp/inop_test && /tmp/inop_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"
#include "model.c"
#include "seq.c"
#include "overdub.c"
#include "fm6_core.c"
#include "fm_chart.c"
#include "voice_chain.c"
#include "perc_engine.c"
#include "fx_sends.c"
#include "phys_engine.c"
#include "voice_steal.c"

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
static void emit_log(uint32_t k, uint32_t step, const track_pat_t *t)
{
    (void)t;
    fired[k][nfired[k]++ & 255u] = step;
}
static const track_pat_t *last_track;
static void emit_track(uint32_t k, uint32_t step, const track_pat_t *t)
{
    (void)k;
    (void)step;
    last_track = t;
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
    return chain_render(c, p, x, l, r);
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
    chain_effects(&c, &p, x);
    for (i = 0; i < CTL; i++)
        CHECK((x[i] & 8191) == 0);                      /* 13 low bits gone */
    packet_default(&p);
    p.v[PV_RATE] = 30;
    chain_start(&c, &p);
    for (i = 0; i < CTL; i++)
        x[i] = (int32_t)i;
    chain_effects(&c, &p, x);
    CHECK(x[0] == 0 && x[10] == 0 && x[11] == 11);      /* held 11 samples */
    packet_default(&p);
    p.v[PV_DRIVE] = 127;
    chain_start(&c, &p);
    for (i = 0; i < CTL; i++)
        x[i] = 30000;
    chain_effects(&c, &p, x);
    CHECK(x[0] <= TANH_Q15[256]);
    packet_default(&p);
    p.v[PV_SHAPER] = 127;
    chain_start(&c, &p);
    for (i = 0; i < CTL; i++)
        x[i] = i & 1u ? 60000 : -60000;
    chain_effects(&c, &p, x);
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

/* the chart's places hold what the renderer runs */
static void test_fm_chart(void)
{
    uint32_t a, i, j;
    uint8_t m[6];
    static const uint8_t ALG1[6] = {1u << 1, 0, 1u << 3, 1u << 4, 1u << 5, 0};     /* 2>1, 6>5>4>3 */
    fm_routes(0, m);
    CHECK(memcmp(m, ALG1, 6) == 0);
    CHECK(fm_feedback_op(0) == 5);
    CHECK(fm_is_carrier(0, 0) && !fm_is_carrier(0, 1) && fm_is_carrier(0, 2));
    fm_routes(31, m);
    for (i = 0; i < 6u; i++)
        CHECK(m[i] == 0 && fm_is_carrier(31, i));      /* algorithm 32: six carriers */
    for (a = 0; a < 32u; a++) {
        fm_routes(a, m);
        for (i = 0; i < 6u; i++) {
            CHECK(((FM_CELL[a][i] >> 4) == 0) == (fm_is_carrier(a, i) != 0));
            for (j = 0; j < 6u; j++) {
                if (i != j)
                    CHECK(FM_CELL[a][i] != FM_CELL[a][j]);
                if (m[i] >> j & 1u)
                    CHECK((FM_CELL[a][j] >> 4) == (FM_CELL[a][i] >> 4) + 1u);
            }
        }
    }
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
        CHECK(c.off[n] == 0);                           /* the default routes have amount 0 */
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
    CHECK(c.off[MOD_DST_LEVEL] > (95 << 8));
    p.v[PV_VEL] = 0;
    chain_mod(&c, &p, 0);
    CHECK(c.off[MOD_DST_LEVEL] == 0);
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
            chain_render(&c, &p, x, l, r);
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
        delay_block(&d, b ? zero : in, l, r, 1000, 80);
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
        delay_block(&d, b < 30u ? in : zero, l, r, 2000, 127);
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
    delay_block(&d, zero, l, r, 1000, 0);
    CHECK(d.delay_cur == 1000u);
    delay_block(&d, zero, l, r, 2000, 0);
    CHECK(d.delay_cur == 1000u + CTL);
    delay_block(&d, zero, l, r, 100, 0);
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
        in[i] = i & 1u ? -20000 : 20000;
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
    CHECK(late < early / 100);                          /* largest size, no damping: still decays */
}

static void phys_packet(pkt_t *p, int32_t model, int32_t note)
{
    sound_init(p, 0);
    p->v[PV_ENGINE] = ENGINE_PHYS;
    p->v[PV_E0] = (int8_t)model;
    p->v[PV_NOTE] = (int8_t)note;
    p->v[PV_VEL] = 127;
}

static void test_phys_rings_and_decays(void)
{
    static phys_voice_t v;
    uint32_t model;
    for (model = 0; model < PM_COUNT; model++) {
        pkt_t p;
        int32_t x[CTL], early = 0, late = 0;
        uint32_t b, i;
        phys_packet(&p, (int32_t)model, 48);
        p.v[PV_E3] = 20;
        phys_start(&v, &p, 12345);
        for (b = 0; b < 3000u; b++) {
            memset(x, 0, sizeof x);
            phys_render(&v, &p, x, 0);
            for (i = 0; i < CTL; i++) {
                int32_t a = x[i] < 0 ? -x[i] : x[i];
                CHECK(a <= 60000);                      /* stays inside what the chain accepts */
                if (b < 100u && a > early)
                    early = a;
                if (b >= 2950u && a > late)
                    late = a;
            }
        }
        CHECK(early > 500);                             /* a strike is heard */
        CHECK(late < early / 10);                       /* and dies away */
    }
}

/* the string's period in samples: the lag where it best matches itself */
static uint32_t phys_period(int32_t note, int32_t pitch)
{
    enum { SKIP = 100, BLOCKS = 300, MAX_LAG = 900 };
    static phys_voice_t v;
    static int32_t wave[BLOCKS * CTL];
    pkt_t p;
    int64_t best = 0;
    uint32_t b, i, lag, best_lag = 0;
    phys_packet(&p, PM_STRING, note);
    p.v[PV_E1] = 32;
    p.v[PV_E2] = 20;
    p.v[PV_E3] = 110;
    p.v[PV_E4] = 0;
    p.v[PV_E7] = 0;
    phys_start(&v, &p, 7);
    for (b = 0; b < SKIP + BLOCKS; b++) {
        int32_t x[CTL];
        memset(x, 0, sizeof x);
        phys_render(&v, &p, x, pitch);
        if (b >= SKIP)
            memcpy(&wave[(b - SKIP) * CTL], x, sizeof x);
    }
    for (lag = 40; lag < MAX_LAG; lag++) {
        int64_t sum = 0;
        for (i = 0; i + lag < BLOCKS * CTL; i += 3)
            sum += (int64_t)wave[i] * wave[i + lag];
        /* the fundamental is the first lag near the best match: its multiples match as well */
        if (sum > best + best / 20) {
            best = sum;
            best_lag = lag;
        }
    }
    return best_lag;
}

static void test_phys_pitch(void)
{
    uint32_t a4 = phys_period(69, 0), a3 = phys_period(57, 0), offset = phys_period(57, 12 * 16);
    CHECK(a4 >= 98u && a4 <= 103u);                     /* note 69 is 440 Hz: 100 samples, as FM's note numbers */
    CHECK(a3 >= 197u && a3 <= 204u);
    CHECK(offset >= 98u && offset <= 103u);             /* the modulation offset moves pitch too */
}

static void perc_packet(pkt_t *p, int32_t mode, int32_t note, int32_t decay, int32_t spread, int32_t harm, int32_t fold, int32_t bend, int32_t noise, int32_t attack)
{
    sound_init(p, 0);
    p->v[PV_ENGINE] = ENGINE_PERC;
    p->v[PV_E0] = (int8_t)mode;
    p->v[PV_NOTE] = (int8_t)note;
    p->v[PV_E1] = (int8_t)decay;
    p->v[PV_E2] = (int8_t)spread;
    p->v[PV_E3] = (int8_t)harm;
    p->v[PV_E4] = (int8_t)fold;
    p->v[PV_E5] = (int8_t)bend;
    p->v[PV_E6] = (int8_t)noise;
    p->v[PV_E7] = (int8_t)attack;
}

/* one block; returns its peak and adds its rising zero crossings to *crossings */
static int32_t perc_block(perc_voice_t *S, const pkt_t *p, int32_t *alive, uint32_t *crossings, int32_t *last)
{
    int32_t out[CTL], peak = 0;
    uint32_t i;
    for (i = 0; i < CTL; i++)
        out[i] = 0;
    *alive = perc_render(S, p, out, 0);
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
    perc_packet(&p, PERC_SKIN, 69, 120, 0, 0, 0, 0, 0, 127);
    n = perc_crossings(&p, 0, blocks);
    CHECK(n > 430 && n < 450);                          /* A4 is 440 Hz */
    perc_packet(&p, PERC_SKIN, 81, 120, 0, 0, 0, 0, 0, 127);
    n = perc_crossings(&p, 0, blocks);
    CHECK(n > 865 && n < 895);                          /* an octave up */
    perc_packet(&p, PERC_METAL, 69, 120, 0, 0, 0, 0, 0, 127);
    CHECK(perc_crossings(&p, 0, blocks) > 400);         /* with harm 0 the modulators are silent */
}

static void test_perc_bend(void)
{
    pkt_t p;
    uint32_t early, late;
    perc_packet(&p, PERC_SKIN, 45, 120, 0, 0, 0, 80, 0, 127);
    early = perc_crossings(&p, 0, 30);
    late = perc_crossings(&p, 2000, 30);
    CHECK(early > late * 2u);
    perc_packet(&p, PERC_SKIN, 45, 120, 0, 0, 0, 0, 0, 127);
    CHECK(perc_crossings(&p, 2000, 30) >= late - 2u && perc_crossings(&p, 2000, 30) <= late + 2u);   /* it settles on the note */
}

static void test_perc_spread_and_harm(void)
{
    int32_t level[PERC_PARTIALS];
    uint32_t i;
    for (i = 0; i < PERC_PARTIALS; i++) {
        CHECK(perc_ratio_q8(0, i) == 256);              /* 0 is unison */
        CHECK(perc_ratio_q8(64, i) == 256u * (i + 1u)); /* 64 is the harmonic series */
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
    perc_packet(&p, PERC_SKIN, 45, 60, 0, 0, 0, 0, 0, 127);
    short_life = perc_life(&p);
    CHECK(short_life + 40u > env_blocks(60) && short_life < env_blocks(60) * 2u + 40u);      /* -60 dB at about E1's time */
    perc_packet(&p, PERC_SKIN, 45, 100, 0, 0, 0, 0, 0, 127);
    long_life = perc_life(&p);
    CHECK(long_life > short_life * 4u && long_life < 400000u);
    perc_packet(&p, PERC_SKIN, 45, 0, 0, 127, 0, 0, 0, 0);
    CHECK(perc_life(&p) < 100u);                        /* the shortest settings */
    perc_packet(&p, PERC_METAL, 45, 127, 90, 127, 127, 127, 127, 0);
    CHECK(perc_life(&p) < 400000u);                     /* the longest settings */
}

/* harm sets how much upper partials ring: a low harm thins out over the note */
static void test_perc_harm_shortens_upper_partials(void)
{
    static perc_voice_t a, b;
    pkt_t low, high;
    perc_packet(&low, PERC_SKIN, 45, 100, 64, 40, 0, 0, 0, 127);
    perc_packet(&high, PERC_SKIN, 45, 100, 64, 127, 0, 0, 0, 127);
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

/* without the punch gain a folded sum stays at full scale until the driven sum falls under it */
static void test_perc_fold_keeps_punch(void)
{
    pkt_t dry, folded;
    uint32_t mid = env_blocks(100) / 4u;
    int32_t dry_peak, fold_peak;
    perc_packet(&dry, PERC_SKIN, 45, 100, 0, 0, 0, 0, 0, 127);
    perc_packet(&folded, PERC_SKIN, 45, 100, 0, 0, 127, 0, 0, 127);
    dry_peak = perc_peak_at(&dry, mid);
    fold_peak = perc_peak_at(&folded, mid);
    CHECK(perc_peak_at(&folded, 60) > fold_peak * 2);   /* folded output still falls */
    CHECK(fold_peak < dry_peak * 3 / 2 && fold_peak > dry_peak / 3);
    CHECK(perc_crossings(&folded, 60, 60) > perc_crossings(&dry, 60, 60));    /* and folding adds harmonics */
    mid = env_blocks(100) * 4u / 5u;                    /* near the end they meet again */
    fold_peak = perc_peak_at(&folded, mid);
    dry_peak = perc_peak_at(&dry, mid);
    CHECK(fold_peak < dry_peak * 2 + 20 && fold_peak * 2 + 20 > dry_peak);
}

static void test_perc_fold_function(void)
{
    int32_t x;
    CHECK(perc_fold(0) == 0 && perc_fold(32767) == 32767);
    CHECK(abs(perc_fold(40000) - (65534 - 40000)) <= 1);
    for (x = -500000; x <= 500000; x += 37) {
        int32_t y = perc_fold(x);
        CHECK(y >= -32768 && y <= 32767);
        CHECK(abs(y - wavefold(x)) <= 40);              /* the chain's fold, in one step: their periods differ by 1 in 32768 */
    }
}

static void test_perc_metal_and_noise(void)
{
    static perc_voice_t a, b;
    pkt_t p;
    int32_t ao[CTL], bo[CTL];
    uint32_t i;
    int differs = 0;
    perc_packet(&p, PERC_SKIN, 45, 80, 60, 100, 0, 0, 0, 127);
    perc_start(&a, &p, 1);
    perc_packet(&p, PERC_METAL, 45, 80, 60, 100, 0, 0, 0, 127);
    perc_start(&b, &p, 1);
    for (i = 0; i < CTL; i++)
        ao[i] = bo[i] = 0;
    for (i = 0; i < 8u; i++) {
        pkt_t skin;
        perc_packet(&skin, PERC_SKIN, 45, 80, 60, 100, 0, 0, 0, 127);
        (void)perc_render(&a, &skin, ao, 0);
        (void)perc_render(&b, &p, bo, 0);
    }
    for (i = 0; i < CTL; i++)
        differs |= ao[i] != bo[i];
    CHECK(differs);                                     /* METAL modulates */
    perc_packet(&p, PERC_SKIN, 45, 80, 0, 0, 0, 0, 90, 127);
    perc_start(&a, &p, 1);
    perc_start(&b, &p, 99);
    for (i = 0; i < CTL; i++)
        ao[i] = bo[i] = 0;
    (void)perc_render(&a, &p, ao, 0);
    (void)perc_render(&b, &p, bo, 0);
    differs = 0;
    for (i = 0; i < CTL; i++)
        differs |= ao[i] != bo[i];
    CHECK(differs);                                     /* NOISE differs by seed */
    perc_packet(&p, PERC_SKIN, 45, 80, 0, 0, 0, 0, 0, 127);
    perc_start(&a, &p, 1);
    perc_start(&b, &p, 99);
    for (i = 0; i < CTL; i++)
        ao[i] = bo[i] = 0;
    (void)perc_render(&a, &p, ao, 0);
    (void)perc_render(&b, &p, bo, 0);
    CHECK(memcmp(ao, bo, sizeof ao) == 0);              /* with no noise and no click, the seed does nothing */
}

static void test_perc_attack(void)
{
    pkt_t hard, soft;
    perc_packet(&hard, PERC_SKIN, 45, 100, 0, 0, 0, 0, 0, 0);
    perc_packet(&soft, PERC_SKIN, 45, 100, 0, 0, 0, 0, 0, 127);
    CHECK(perc_peak_at(&hard, 0) > perc_peak_at(&soft, 0));
    {
        static perc_voice_t a, b;
        int32_t ao[CTL], bo[CTL];
        uint32_t i;
        perc_start(&a, &hard, 1);
        perc_start(&b, &hard, 99);
        for (i = 0; i < CTL; i++)
            ao[i] = bo[i] = 0;
        (void)perc_render(&a, &hard, ao, 0);
        (void)perc_render(&b, &hard, bo, 0);
        CHECK(memcmp(ao, bo, sizeof ao) != 0);          /* a low attack adds the noise click, with NOISE at 0 */
    }
    {
        static perc_voice_t a;
        int32_t alive, last = 0, first;
        perc_start(&a, &soft, 1);
        first = perc_block(&a, &soft, &alive, 0, &last);
        CHECK(first < PERC_PEAK / 8);                   /* the longest onset starts quietly */
    }
}

static void test_steal_order(void)
{
    steal_t v[3];
    memset(v, 0, sizeof v);
    v[0] = (steal_t){1, PRIO_NORMAL, 20000, 1};
    v[1] = (steal_t){1, PRIO_NORMAL, 10000, 2};
    v[2] = (steal_t){1, PRIO_NORMAL, 30000, 3};
    CHECK(voice_victim(v, 3, PRIO_NORMAL) == 0);        /* equal priority: oldest, not quietest */
    CHECK(voice_victim(v, 3, PRIO_LOW) == -1);          /* never from a higher priority */
    CHECK(voice_victim(v, 3, PRIO_HIGH) == 1);          /* lower priority: quietest */
    v[2].prio = PRIO_LOW;
    CHECK(voice_victim(v, 3, PRIO_HIGH) == 2);          /* the lowest priority goes first */
    CHECK(voice_victim(v, 3, PRIO_NORMAL) == 2);
    v[1] = (steal_t){1, PRIO_HIGH, 100, 2};
    CHECK(voice_victim(v, 3, PRIO_LOW) == 1);           /* nearly silent goes first, whatever its priority */
    v[1].stealable = 0;
    CHECK(voice_victim(v, 3, PRIO_LOW) == 2);           /* a voice already fading cannot be taken twice */
}

static void test_master_bound(void)
{
    master_t m = {MASTER_LIMIT};
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
        master_t q = {MASTER_LIMIT};
        int32_t l = 1000, r = -1000;
        master_sample(&q, &l, &r);
        CHECK(l > 900 && l <= 1000 && r == -l);         /* quiet signals pass nearly unchanged */
    }
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
    test_fm_chart();
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
    test_phys_rings_and_decays();
    test_phys_pitch();
    test_perc_pitch();
    test_perc_bend();
    test_perc_spread_and_harm();
    test_perc_decay_ends_the_voice();
    test_perc_harm_shortens_upper_partials();
    test_perc_level_bound();
    test_perc_fold_keeps_punch();
    test_perc_fold_function();
    test_perc_metal_and_noise();
    test_perc_attack();
    test_steal_order();
    test_master_bound();
    printf("sizeof(pattern_t) %zu, 27 patterns %zu KB, pool %zu B\n", sizeof(pattern_t),
           NPATTERN * sizeof(pattern_t) / 1024, sizeof pool);
    printf(fails ? "FAILED: %d\n" : "ok\n", fails);
    return fails != 0;
}
