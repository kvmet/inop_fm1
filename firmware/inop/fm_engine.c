/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM engine: four sine operators in one of 8 algorithms. The chain's MOD envelope scales every operator but C.
 * No hardware here: tests/inop_test.c checks each algorithm's loop against FM_ROUTES. */
enum { FM_C, FM_A, FM_B1, FM_B2, FM_OPS };
#define FM_ALGS 8u
#define FM_RATIOS 16u
#define FM_PEAK 8000             /* the mix's full scale at the output */
#define FM_SHIFT 18              /* a full-scale modulator moves phase by 2 cycles */

/* FM_ROUTES[alg][op] bit j: operator j modulates op. Only a later operator modulates an earlier one, so B2 runs first */
static const uint8_t FM_ROUTES[FM_ALGS][FM_OPS] = {
    {1u << FM_A, 1u << FM_B1, 1u << FM_B2, 0},
    {1u << FM_A | 1u << FM_B1, 0, 1u << FM_B2, 0},
    {1u << FM_A | 1u << FM_B1 | 1u << FM_B2, 0, 0, 0},
    {1u << FM_A, 1u << FM_B1 | 1u << FM_B2, 0, 0},
    {1u << FM_A, 0, 1u << FM_B2, 0},
    {1u << FM_B2, 1u << FM_B2, 1u << FM_B2, 0},
    {1u << FM_A, 0, 0, 0},
    {0, 0, 0, 0},
};

static const uint16_t FM_RATIO_Q8[FM_RATIOS] = {64, 128, 256, 512, 768, 1024, 1280, 1536, 1792, 2048, 2304, 2560, 2816, 3072, 3584, 4096};

typedef struct {
    uint32_t phase[FM_OPS];
    int32_t gain_a, gain_b;      /* Q15: where the last block's ramp ended */
    int32_t feedback;            /* Q15 */
    int32_t held[2];             /* B2's last two outputs */
    int32_t mix_gain;            /* FM_PEAK shared by the carriers */
    int32_t dc;                  /* Q8 */
} fm_voice_t;

static uint32_t fm_alg(int32_t v) { return (uint32_t)clamp(v, 0, FM_ALGS - 1); }

static int fm_is_carrier(uint32_t alg, uint32_t op)
{
    uint32_t i;
    for (i = 0; i < FM_OPS; i++)
        if (FM_ROUTES[alg][i] >> op & 1u)
            return 0;
    return 1;
}

/* a knob's fine control sits at its low end */
static int32_t fm_squared(int32_t v, int32_t full)
{
    int32_t c = clamp(v, 0, 127);
    return c * c * full / 16129;
}

static void fm_start(fm_voice_t *f, const pkt_t *p)
{
    uint32_t alg = fm_alg(p->v[PV_E0]), i, carriers = 0;
    for (i = 0; i < FM_OPS; i++) {
        f->phase[i] = 0;
        carriers += (uint32_t)fm_is_carrier(alg, i);
    }
    f->gain_a = fm_squared(p->v[PV_E4], 32767);
    f->gain_b = fm_squared(p->v[PV_E5], 32767);
    f->feedback = fm_squared(p->v[PV_E6], 16384);    /* B2 moves its own phase by 1 cycle at most */
    f->held[0] = f->held[1] = 0;
    f->mix_gain = FM_PEAK / (int32_t)carriers;
    f->dc = 0;
}

/* phase per sample, or 0 over Nyquist. ratio: a knob value. detune: Q16 */
static uint32_t fm_op_inc(uint32_t base, int32_t ratio, int32_t detune)
{
    uint64_t r = ((uint64_t)base * FM_RATIO_Q8[clamp(ratio, 0, FM_RATIOS - 1)]) >> 8;
    r = (r * (uint32_t)(65536 + detune)) >> 16;
    return r >= 0x80000000u ? 0u : (uint32_t)r;
}

#define FM_SIN(ph, m) SINE[((ph) + (uint32_t)(m) * (1u << FM_SHIFT)) >> 22]
#define FM_M(x, d) (((x) * (d)) >> 15)
/* one loop per algorithm: a routing lookup per operator per sample would cost more than the operators */
#define FM_LOOP(...) \
    for (s = 0; s < CTL; s++) { \
        int32_t da = depth_a[s], db = depth_b[s], b2 = FM_SIN(pb2, FM_M((h1 + h2) >> 1, fb)); \
        h2 = h1; \
        h1 = b2; \
        __VA_ARGS__ \
        pc += ic; \
        pa += ia; \
        pb1 += ib1; \
        pb2 += ib2; \
    }

/* adds one block to out. pitch: an offset in 1/16 semitones. mod_q15: the MOD envelope. timbre: an offset to both indexes */
static void fm_render(fm_voice_t *f, const pkt_t *p, int32_t *out, int32_t pitch, int32_t mod_q15, int32_t timbre)
{
    int32_t y[CTL], depth_a[CTL], depth_b[CTL];
    int32_t target_a = (fm_squared(p->v[PV_E4] + timbre, 32767) * mod_q15) >> 15, target_b = (fm_squared(p->v[PV_E5] + timbre, 32767) * mod_q15) >> 15;
    int32_t ramp_a = f->gain_a * (1 << CTL_BITS), ramp_b = f->gain_b * (1 << CTL_BITS);
    int32_t h1 = f->held[0], h2 = f->held[1], fb = f->feedback, detune = fm_squared(p->v[PV_E7], 16384);   /* 1/4 is 1 semitone */
    uint32_t base = note_inc(p->v[PV_NOTE] * 16 + pitch), s;
    uint32_t ic = fm_op_inc(base, p->v[PV_E1], 0), ia = fm_op_inc(base, p->v[PV_E2], detune);
    uint32_t ib1 = fm_op_inc(base, p->v[PV_E3], -detune), ib2 = fm_op_inc(base, p->v[PV_E3], detune / 2);
    uint32_t pc = f->phase[FM_C], pa = f->phase[FM_A], pb1 = f->phase[FM_B1], pb2 = f->phase[FM_B2];
    for (s = 0; s < CTL; s++) {
        ramp_a += target_a - f->gain_a;
        ramp_b += target_b - f->gain_b;
        depth_a[s] = ramp_a >> CTL_BITS;
        depth_b[s] = ramp_b >> CTL_BITS;
    }
    f->gain_a = target_a;
    f->gain_b = target_b;
    switch (fm_alg(p->v[PV_E0])) {
    case 0:
        FM_LOOP(int32_t b1 = FM_SIN(pb1, FM_M(b2, db)), a = FM_SIN(pa, FM_M(b1, db));
                y[s] = FM_SIN(pc, FM_M(a, da));)
        break;
    case 1:
        FM_LOOP(int32_t b1 = FM_SIN(pb1, FM_M(b2, db)), a = FM_SIN(pa, 0);
                y[s] = FM_SIN(pc, FM_M(a, da) + FM_M(b1, db));)
        break;
    case 2:
        FM_LOOP(int32_t b1 = FM_SIN(pb1, 0), a = FM_SIN(pa, 0);
                y[s] = FM_SIN(pc, FM_M(a, da) + FM_M(b1, db) + FM_M(b2, db));)
        break;
    case 3:
        FM_LOOP(int32_t b1 = FM_SIN(pb1, 0), a = FM_SIN(pa, FM_M(b1, db) + FM_M(b2, db));
                y[s] = FM_SIN(pc, FM_M(a, da));)
        break;
    case 4:
        FM_LOOP(int32_t b1 = FM_SIN(pb1, FM_M(b2, db)), a = FM_SIN(pa, 0);
                y[s] = FM_SIN(pc, FM_M(a, da)) + FM_M(b1, db);)
        break;
    case 5:
        FM_LOOP(int32_t m = FM_M(b2, db), b1 = FM_SIN(pb1, m), a = FM_SIN(pa, m);
                y[s] = FM_SIN(pc, m) + FM_M(a, da) + FM_M(b1, db);)
        break;
    case 6:
        FM_LOOP(int32_t b1 = FM_SIN(pb1, 0), a = FM_SIN(pa, 0);
                y[s] = FM_SIN(pc, FM_M(a, da)) + FM_M(b1, db) + FM_M(b2, db);)
        break;
    default:
        FM_LOOP(int32_t b1 = FM_SIN(pb1, 0), a = FM_SIN(pa, 0);
                y[s] = FM_SIN(pc, 0) + FM_M(a, da) + FM_M(b1, db) + FM_M(b2, db);)
        break;
    }
    f->phase[FM_C] = pc;
    f->phase[FM_A] = pa;
    f->phase[FM_B1] = pb1;
    f->phase[FM_B2] = pb2;
    f->held[0] = h1;
    f->held[1] = h2;
    for (s = 0; s < CTL; s++) {
        int32_t x = y[s] * 256 - f->dc;
        f->dc += x >> 10;                                /* a one-pole high-pass, ~7 Hz: feedback puts a sideband at 0 Hz */
        out[s] += ((x >> 8) * f->mix_gain) >> 15;
    }
}
