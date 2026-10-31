/* SPDX-License-Identifier: GPL-3.0-only */
/* What each voice does after its engine: effects, filter, amp envelope, level and pan. All values come from the packet.
 * Also the master limiter. No hardware here: tests/inop_test.c runs this on the host. */
#define ENV_FULL (1 << 24)
#define ENV_END (1 << 14)        /* -60 dB: the last block fades from here to 0 */
#define CTL_BITS 5
_Static_assert((1 << CTL_BITS) == CTL, "the amp ramp divides by a shift");
#define MASTER_LIMIT 12000
#define MASTER_CEILING 14000     /* the limiter's attack lets a few samples past MASTER_LIMIT */

enum { FLT_OFF, FLT_LP, FLT_BP, FLT_HP, FLT_TYPES };
enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_DONE };
enum { MOD_SRC_OFF, MOD_SRC_ENV, MOD_SRC_LFO1, MOD_SRC_LFO2, MOD_SRC_VEL, MOD_SRCS };
enum { MOD_DST_OFF, MOD_DST_PITCH, MOD_DST_CUT, MOD_DST_RES, MOD_DST_LEVEL, MOD_DST_PAN, MOD_DST_DRIVE, MOD_DST_SHAPER, MOD_DST_CRUSH, MOD_DST_RATE, MOD_DSTS };
enum { LFO_SINE, LFO_TRI, LFO_SAW, LFO_SQUARE, LFO_SH, LFO_SHAPES };
enum { LFO_RETRIGGER, LFO_FREE, LFO_MODES };
#define MOD_SLOTS 4u
#define LFO_BASE_INC 155833u     /* 0.05 Hz at one step per render block */

typedef struct {
    int32_t env, inc, decay_k, release_k, sustain;
    uint8_t stage;
} env_t;

typedef struct {
    uint32_t phase, inc, start;
    int32_t held;                /* the S&H shape's value */
    uint8_t shape, mode;
} lfo_t;

typedef struct {
    env_t amp_env, mod_env;
    lfo_t lfo[2];
    int32_t off[MOD_DSTS];       /* this block's modulation, in packet units Q8 */
    int32_t gain_l, gain_r;      /* Q15: the level the last block ended at */
    int32_t cut_q8, res_q8;      /* the values a1..a3 were computed from */
    int32_t a1, a2, a3, damp, ic1, ic2;
    int32_t hold;
    uint32_t hold_left;
} chain_t;

static uint32_t mod_rng = 0x2F6E2B1u;

static int32_t mod_random(void)
{
    mod_rng ^= mod_rng << 13;
    mod_rng ^= mod_rng >> 17;
    mod_rng ^= mod_rng << 5;
    return (int32_t)(mod_rng >> 16) - 32768;
}

static uint32_t env_blocks(int32_t time_id)
{
    uint32_t blocks = TIME_MS_X10[clamp(time_id, 0, 127)] * 441u / 100u / CTL;
    return blocks ? blocks : 1u;
}

static void env_start(env_t *e, int32_t a, int32_t d, int32_t sus, int32_t r)
{
    e->env = 0;
    e->inc = ENV_FULL / (int32_t)env_blocks(a);
    e->decay_k = 65536 / (int32_t)env_blocks(d);
    e->release_k = 65536 / (int32_t)env_blocks(r);
    e->sustain = (ENV_FULL / 127) * clamp(sus, 0, 127);
    e->stage = ENV_ATTACK;
}

static void env_release(env_t *e)
{
    if (e->stage < ENV_RELEASE)
        e->stage = ENV_RELEASE;
}

static int32_t env_toward(int32_t env, int32_t target, int32_t k)
{
    int32_t move = (((target - env) >> 12) * k) >> 4;    /* k reaches 65536: a wider product overflows */
    if (!move)
        move = target > env ? 1 : target < env ? -1 : 0;
    return env + move;
}

static void env_block(env_t *e)
{
    switch (e->stage) {
    case ENV_ATTACK:
        e->env += e->inc;
        if (e->env >= ENV_FULL) {
            e->env = ENV_FULL;
            e->stage = ENV_DECAY;
        }
        break;
    case ENV_DECAY:
        e->env = env_toward(e->env, e->sustain, e->decay_k);
        if (e->env - e->sustain < ENV_END) {
            e->env = e->sustain;
            e->stage = ENV_SUSTAIN;
        }
        break;
    case ENV_RELEASE:
        e->env = env_toward(e->env, 0, e->release_k);
        if (e->env < ENV_END) {
            e->env = 0;
            e->stage = ENV_DONE;
        }
        break;
    default:
        break;
    }
}

typedef struct { uint32_t n, rise, span, sustain; } env_curve_t;

/* the envelope as it plays, released as soon as it reaches sustain. y[0..n) is 0..128, and the first rise points run up to
 * sustain. Each point is the highest of its blocks, so a short peak survives a long release. One point is one block until
 * cap points are full, so span is cap for a short envelope and n for a long one that fills the width */
static void env_curve(env_curve_t *c, uint8_t *y, uint32_t cap, int32_t a, int32_t d, int32_t sus, int32_t r)
{
    env_t e;
    uint32_t stride = 1, tick = 0, rise_ticks = 0, i;
    env_start(&e, a, d, sus, r);
    c->n = 0;
    c->sustain = (uint32_t)e.sustain >> 17;
    while (e.stage != ENV_DONE) {
        uint32_t at;
        uint8_t v;
        env_block(&e);
        if (e.stage == ENV_SUSTAIN) {
            rise_ticks = tick;
            env_release(&e);
            continue;
        }
        v = (uint8_t)(e.env >> 17);
        if (tick / stride >= cap) {
            for (i = 0; i < (cap + 1u) / 2u; i++) {
                uint8_t second = i * 2u + 1u < cap ? y[i * 2u + 1u] : 0u;
                y[i] = y[i * 2u] > second ? y[i * 2u] : second;
            }
            c->n = (cap + 1u) / 2u;
            stride *= 2u;
        }
        at = tick / stride;
        if (at >= c->n) {
            y[at] = v;
            c->n = at + 1u;
        } else if (v > y[at]) {
            y[at] = v;
        }
        tick++;
    }
    c->rise = (rise_ticks + stride - 1u) / stride;
    c->span = stride > 1u ? c->n : cap;
}

/* the line to draw: from silence, up to sustain, hold px wide at sustain, down to silence. Writes at most c->n + 4 points */
static uint32_t env_polyline(const env_curve_t *c, const uint8_t *y, uint32_t w, uint32_t hold, uint8_t *px, uint8_t *py)
{
    uint32_t i, m = 0, hold_x = c->rise * w / c->span;
    px[m] = 0;
    py[m++] = 0;
    for (i = 0; i < c->rise; i++) {
        px[m] = (uint8_t)((i + 1u) * w / c->span);
        py[m++] = y[i];
    }
    px[m] = (uint8_t)hold_x;
    py[m++] = (uint8_t)c->sustain;
    px[m] = (uint8_t)(hold_x + hold);
    py[m++] = (uint8_t)c->sustain;
    for (i = c->rise; i < c->n; i++) {
        px[m] = (uint8_t)((i + 1u) * w / c->span + hold);
        py[m++] = y[i];
    }
    px[m] = (uint8_t)(c->n * w / c->span + hold);
    py[m++] = 0;
    return m;
}

static const uint16_t SEMI_Q12[12] = {4096, 4340, 4598, 4871, 5161, 5468, 5793, 6137, 6502, 6889, 7298, 7732};

static uint32_t lfo_inc(int32_t rate)
{
    uint32_t r = (uint32_t)clamp(rate, 0, 127);
    return ((LFO_BASE_INC * SEMI_Q12[r % 12u]) >> 12) << (r / 12u);
}

static void lfo_start(lfo_t *l, int32_t shape, int32_t mode, int32_t rate, int32_t start)
{
    l->shape = (uint8_t)clamp(shape, 0, LFO_SHAPES - 1);
    l->mode = (uint8_t)clamp(mode, 0, LFO_MODES - 1);
    l->inc = lfo_inc(rate);
    l->start = (uint32_t)clamp(start, 0, 127) << 25;
    l->phase = l->start;
    l->held = mod_random();
}

/* -32767..32767. clock counts render blocks: a free-running LFO reads its phase from it, so all its voices agree */
static int32_t lfo_block(lfo_t *l, uint32_t clock)
{
    uint32_t prev = l->phase, q;
    int32_t x;
    l->phase = l->mode == LFO_FREE ? clock * l->inc + l->start : prev + l->inc;
    if (l->phase < prev)
        l->held = mod_random();
    switch (l->shape) {
    case LFO_SAW:
        return (int32_t)(l->phase >> 16) - 32768 + (l->phase >> 16 == 0u);
    case LFO_SQUARE:
        return l->phase >> 31 ? -32767 : 32767;
    case LFO_SH:
        return clamp(l->held, -32767, 32767);
    default:
        break;
    }
    q = (l->phase + 0x40000000u) >> 16;          /* a quarter on, so phase 0 is the rising zero */
    x = (int32_t)(q < 32768u ? q : 65535u - q) * 2 - 32767;
    if (l->shape == LFO_SINE)
        x = clamp(((x >> 1) * (98304 - ((x * x) >> 15))) >> 15, -32767, 32767);
    return x;
}

static void chain_filter_coef(chain_t *c, int32_t cut_q8, int32_t res_q8)
{
    int32_t i, g, den;
    cut_q8 = clamp(cut_q8, 0, 127 << 8);
    res_q8 = clamp(res_q8, 0, 127 << 8);
    if (cut_q8 == c->cut_q8 && res_q8 == c->res_q8)
        return;
    c->cut_q8 = cut_q8;
    c->res_q8 = res_q8;
    i = cut_q8 >> 8;
    g = SVF_G[i];
    if (i < 127)
        g += ((SVF_G[i + 1] - g) * (cut_q8 & 255)) >> 8;
    c->damp = 8192 - (res_q8 >> 4) * 7600 / (127 << 4);
    den = 4096 + ((g * (g + c->damp)) >> 12);
    c->a1 = (int32_t)((4096u << 13) / (uint32_t)den);
    c->a2 = (c->a1 * g) >> 12;
    c->a3 = (c->a2 * g) >> 12;
}

/* the note is the packet's: key tracking follows the pitch the trigger asked for */
static void chain_filter_update(chain_t *c, const pkt_t *p)
{
    int32_t key = clamp(p->v[PV_FLT_KEY], 0, 127) * (p->v[PV_NOTE] - 60) * 2;
    chain_filter_coef(c, (clamp(p->v[PV_FLT_CUT], 0, 127) << 8) + key + c->off[MOD_DST_CUT],
                      (clamp(p->v[PV_FLT_RES], 0, 127) << 8) + c->off[MOD_DST_RES]);
}

static void chain_start(chain_t *c, const pkt_t *p)
{
    uint32_t i;
    env_start(&c->amp_env, p->v[PV_AMP_A], p->v[PV_AMP_D], p->v[PV_AMP_S], p->v[PV_AMP_R]);
    env_start(&c->mod_env, p->v[PV_MOD_A], p->v[PV_MOD_D], p->v[PV_MOD_S], p->v[PV_MOD_R]);
    lfo_start(&c->lfo[0], p->v[PV_LFO1_SHAPE], p->v[PV_LFO1_PHASE], p->v[PV_LFO1_RATE], p->v[PV_LFO1_START]);
    lfo_start(&c->lfo[1], p->v[PV_LFO2_SHAPE], p->v[PV_LFO2_PHASE], p->v[PV_LFO2_RATE], p->v[PV_LFO2_START]);
    for (i = 0; i < MOD_DSTS; i++)
        c->off[i] = 0;
    c->gain_l = c->gain_r = 0;
    c->cut_q8 = c->res_q8 = -1;
    chain_filter_update(c, p);
    c->ic1 = c->ic2 = 0;
    c->hold = 0;
    c->hold_left = 0;
}

static void chain_release(chain_t *c)
{
    env_release(&c->amp_env);
    env_release(&c->mod_env);
}

/* runs the sources and the slots for one block; the amount at 127 and a full source move a value by 127 */
static void chain_mod(chain_t *c, const pkt_t *p, uint32_t clock)
{
    int32_t src[MOD_SRCS];
    uint32_t i;
    env_block(&c->mod_env);
    src[MOD_SRC_OFF] = 0;
    src[MOD_SRC_ENV] = c->mod_env.env >> 9;
    src[MOD_SRC_LFO1] = lfo_block(&c->lfo[0], clock);
    src[MOD_SRC_LFO2] = lfo_block(&c->lfo[1], clock);
    src[MOD_SRC_VEL] = clamp(p->v[PV_VEL], 0, 127) * 258;
    for (i = 0; i < MOD_DSTS; i++)
        c->off[i] = 0;
    for (i = 0; i < MOD_SLOTS; i++) {
        int32_t from = clamp(p->v[PV_SLOT_SRC0 + i], 0, MOD_SRCS - 1), to = clamp(p->v[PV_SLOT_DST0 + i], 0, MOD_DSTS - 1);
        c->off[to] += (src[from] * p->v[PV_SLOT_AMT0 + i]) >> 7;
    }
}

static inline int32_t softclip(int32_t x)
{
    int32_t a = x < 0 ? -x : x, i = a >> 8, y;
    if (i >= 256)
        y = TANH_Q15[256];
    else
        y = TANH_Q15[i] + (((TANH_Q15[i + 1] - TANH_Q15[i]) * (a & 255)) >> 8);
    return x < 0 ? -y : y;
}

static inline int32_t wavefold(int32_t x)
{
    while (x > 32767 || x < -32767)
        x = x > 0 ? 65534 - x : -65534 - x;
    return x;
}

/* cheap effects in place; each is skipped at 0 */
static void chain_effects(chain_t *c, const pkt_t *p, int32_t *x)
{
    int32_t drive = clamp(p->v[PV_DRIVE] + (c->off[MOD_DST_DRIVE] >> 8), 0, 127);
    int32_t shape = clamp(p->v[PV_SHAPER] + (c->off[MOD_DST_SHAPER] >> 8), 0, 127);
    int32_t crush = clamp(p->v[PV_CRUSH] + (c->off[MOD_DST_CRUSH] >> 8), 0, 127);
    int32_t rate = clamp(p->v[PV_RATE] + (c->off[MOD_DST_RATE] >> 8), 0, 127);
    uint32_t i;
    if (drive)
        for (i = 0; i < CTL; i++)
            x[i] = softclip(clamp(x[i], -60000, 60000) * (8 + drive) >> 3);
    if (shape)
        for (i = 0; i < CTL; i++)
            x[i] = wavefold(clamp(x[i], -60000, 60000) * (16 + shape) >> 4);
    if (crush) {
        int32_t mask = ~((1 << (1 + crush * 12 / 127)) - 1);
        for (i = 0; i < CTL; i++)
            x[i] &= mask;
    }
    if (rate) {
        uint32_t every = 1u + (uint32_t)rate / 3u;
        for (i = 0; i < CTL; i++) {
            if (!c->hold_left) {
                c->hold = x[i];
                c->hold_left = every;
            }
            c->hold_left--;
            x[i] = c->hold;
        }
    }
}

static void chain_filter(chain_t *c, uint32_t type, int32_t *x)
{
    uint32_t i;
    if (type == FLT_OFF)
        return;
    for (i = 0; i < CTL; i++) {
        int32_t in = clamp(x[i], -60000, 60000), v3 = in - c->ic2;
        int32_t v1 = (c->a1 * c->ic1 + c->a2 * v3) >> 13;
        int32_t v2 = c->ic2 + ((c->a2 * c->ic1 + c->a3 * v3) >> 13);
        c->ic1 = clamp(2 * v1 - c->ic1, -150000, 150000);
        c->ic2 = clamp(2 * v2 - c->ic2, -150000, 150000);
        x[i] = type == FLT_LP ? v2 : type == FLT_BP ? v1 : in - ((c->damp * v1) >> 12) - v2;
    }
}

/* adds one block to l and r; x is the engine's block and is overwritten. 0 when the envelope has ended */
static int chain_render(chain_t *c, const pkt_t *p, int32_t *x, int32_t *l, int32_t *r)
{
    int32_t lvl = LEVEL_Q12[clamp(p->v[PV_LEVEL] + (c->off[MOD_DST_LEVEL] >> 8), 0, 127)];
    int32_t pan = clamp(p->v[PV_PAN] + (c->off[MOD_DST_PAN] >> 8), -63, 63);
    int32_t gl = 4096 - (pan > 0 ? pan * 4096 / 63 : 0), gr = 4096 + (pan < 0 ? pan * 4096 / 63 : 0);
    int32_t amp, target_l, target_r, ramp_l, ramp_r;
    uint32_t i;
    chain_effects(c, p, x);
    if (clamp(p->v[PV_FLT_TYPE], 0, FLT_TYPES - 1) != FLT_OFF)
        chain_filter_update(c, p);
    chain_filter(c, (uint32_t)clamp(p->v[PV_FLT_TYPE], 0, FLT_TYPES - 1), x);
    env_block(&c->amp_env);
    amp = ((c->amp_env.env >> 9) * lvl) >> 12;
    target_l = (amp * gl) >> 12;
    target_r = (amp * gr) >> 12;
    ramp_l = c->gain_l * (1 << CTL_BITS);
    ramp_r = c->gain_r * (1 << CTL_BITS);
    for (i = 0; i < CTL; i++) {
        int32_t y = clamp(x[i], -60000, 60000);
        ramp_l += target_l - c->gain_l;
        ramp_r += target_r - c->gain_r;
        l[i] += (y * (ramp_l >> (CTL_BITS + 4))) >> 11;
        r[i] += (y * (ramp_r >> (CTL_BITS + 4))) >> 11;
    }
    c->gain_l = target_l;
    c->gain_r = target_r;
    return c->amp_env.stage != ENV_DONE;
}

/* the master: limiter, then soft clip, then the ceiling */
typedef struct { int32_t env; } master_t;

static void master_sample(master_t *m, int32_t *l, int32_t *r)
{
    int32_t al = *l < 0 ? -*l : *l, ar = *r < 0 ? -*r : *r, a = al > ar ? al : ar;
    if (a > m->env)
        m->env += (a - m->env) >> 2;
    else if (m->env > MASTER_LIMIT)
        m->env -= ((m->env - MASTER_LIMIT) >> 12) + 1;
    if (m->env > MASTER_LIMIT) {
        int32_t g = (int32_t)(((uint32_t)MASTER_LIMIT << 15) / (uint32_t)m->env);
        *l = ((*l >> 4) * g) >> 11;
        *r = ((*r >> 4) * g) >> 11;
    }
    *l = clamp(softclip(*l), -MASTER_CEILING, MASTER_CEILING);
    *r = clamp(softclip(*r), -MASTER_CEILING, MASTER_CEILING);
}

/* a peak as a bar of 0..w px: 6 dB per w/8 px, full at MASTER_CEILING */
#define METER_OCTAVES 8
static uint32_t meter_bar(int32_t peak, uint32_t w)
{
    static const int32_t FULL_Q3 = 109;          /* log2(MASTER_CEILING) in 1/8 octaves */
    uint32_t v = (uint32_t)peak;
    int32_t e = 0;
    if (peak < 8)
        return 0;
    while (v >> (e + 1))
        e++;
    return (uint32_t)clamp(e * 8 + (int32_t)((v >> (e - 3)) & 7u) - FULL_Q3 + METER_OCTAVES * 8, 0, METER_OCTAVES * 8) * w / (METER_OCTAVES * 8);
}
