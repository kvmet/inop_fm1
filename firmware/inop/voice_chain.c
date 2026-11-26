/* SPDX-License-Identifier: GPL-3.0-only */
/* What each voice does after its engine: effects, filter, amp envelope, level and pan. All values come from the packet.
 * No hardware here: tests/inop_test.c runs this on the host. */
#define ENV_FULL (1 << 24)
#define ENV_END (1 << 14)        /* -60 dB: the last block fades from here to 0 */
#define CTL_BITS 5
_Static_assert((1 << CTL_BITS) == CTL, "the amp ramp divides by a shift");
#define SHAPE_FULL 8192          /* an engine's full scale; SHAPE folds relative to this */

enum { FLT_OFF, FLT_LP, FLT_BP, FLT_HP, FLT_TYPES };
enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_DONE };
enum { LFO_SINE, LFO_TRI, LFO_SAW, LFO_SQUARE, LFO_SH, LFO_SHAPES };
enum { LFO_RETRIGGER, LFO_FREE, LFO_SYNC, LFO_MODES };
#define MOD_SLOTS 8u
_Static_assert(PV_SLOT_DST0 - PV_SLOT_SRC0 == MOD_SLOTS, "the packet holds each slot");
#define LFO_BASE_INC 155833u     /* 0.05 Hz at one step per render block */

typedef struct {
    int32_t env, inc, decay_k, release_k, sustain;
    uint8_t stage;
} env_t;

typedef struct {
    uint32_t phase, inc, start;
    uint32_t cycle;              /* a synced LFO's cycle, in LFO_SYNC_UNITS_PER_STEP */
    int32_t held;                /* the S&H shape's value */
    uint8_t shape, mode;
} lfo_t;

/* what a channel of the chain remembers. A wide voice runs two */
typedef struct {
    int32_t ic1, ic2;
    int32_t hold;
    uint32_t hold_left;
    int32_t shape_level, shape_gain;   /* Q15 */
} chain_path_t;

typedef struct {
    env_t amp_env, mod_env, mod2_env;
    lfo_t lfo[2];
    int32_t off[MOD_DSTS];       /* this block's modulation, in packet units Q8 */
    uint8_t index_routed;        /* a slot routes to the FM index */
    int32_t random;              /* the random source: one value for the trigger's whole life */
    int32_t lfo_rate_off[2];     /* the rate offset each LFO's inc holds */
    int32_t gain_l, gain_r;      /* Q15: the level the last block ended at */
    int32_t cut_q8, res_q8;      /* the values a1..a3 were computed from */
    int32_t a1, a2, a3, damp;
    chain_path_t path[2];
} chain_t;

static uint32_t mod_rng = 0x2F6E2B1u;
static int32_t follower_input;   /* Q15: the loudest send of this block */
static int32_t follower_level;   /* Q15: what MOD_SRC_FOLLOW reads, so every voice in a block reads the same value */

static AUDIO_INLINE int32_t mod_random(void)
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

/* per block k of an exponential fall that reaches -60 dB in the table's time: 6.91 time constants */
static int32_t env_fall_k(int32_t time_id)
{
    uint32_t k = 452853u / env_blocks(time_id);
    return k > 65536u ? 65536 : (int32_t)k;
}

static void env_start(env_t *e, int32_t a, int32_t d, int32_t sus, int32_t r)
{
    e->env = 0;
    e->inc = ENV_FULL / (int32_t)env_blocks(a);
    e->decay_k = env_fall_k(d);
    e->release_k = env_fall_k(r);
    e->sustain = (ENV_FULL / 127) * clamp(sus, 0, 127);
    e->stage = ENV_ATTACK;
}

static AUDIO_INLINE void env_release(env_t *e)
{
    if (e->stage < ENV_RELEASE)
        e->stage = ENV_RELEASE;
}

static AUDIO_INLINE int32_t env_toward(int32_t env, int32_t target, int32_t k)
{
    int32_t move = (((target - env) >> 12) * k) >> 4;    /* k reaches 65536: a wider product overflows */
    if (!move)
        move = target > env ? 1 : target < env ? -1 : 0;
    return env + move;
}

static AUDIO_INLINE void env_block(env_t *e)
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
#define NOTE0_INC 49765u         /* phase per sample at note 0 (8.18 Hz), >> 4 */

/* phase per sample. pitch: 1/16 semitones above note 0 */
static AUDIO_INLINE uint32_t note_inc(int32_t pitch)
{
    uint32_t q = (uint32_t)clamp(pitch, 0, 127 * 16 + 15), semi = q >> 4;
    uint32_t v = ((NOTE0_INC * SEMI_Q12[semi % 12u]) >> 8) << (semi / 12u);
    return v + (v >> 8) * (q & 15u);                 /* one 1/16 semitone is 0.36 %, and 1/256 is 0.39 % */
}

static AUDIO_INLINE uint32_t lfo_inc(int32_t rate)
{
    uint32_t r = (uint32_t)clamp(rate, 0, 127);
    return ((LFO_BASE_INC * SEMI_Q12[r % 12u]) >> 12) << (r / 12u);
}

/* in steps: 1/32, 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2T, 1/2, then 1 to 16 bars */
static const uint32_t LFO_SYNC_CYCLE[] = {384, 768, 1024, 1536, 2048, 3072, 4096, 6144, 12288, 24576, 49152, 98304, 196608};
#define LFO_SYNC_RATES NELEM(LFO_SYNC_CYCLE)
static uint32_t lfo_sync_pos;    /* the sequencer's place: all synced LFOs read it */

static uint32_t lfo_sync_rate_index(int32_t rate) { return (uint32_t)clamp(rate, 0, 127) * LFO_SYNC_RATES / 128u; }

static void lfo_start(lfo_t *l, int32_t shape, int32_t mode, int32_t rate, int32_t start)
{
    l->shape = (uint8_t)clamp(shape, 0, LFO_SHAPES - 1);
    l->mode = (uint8_t)clamp(mode, 0, LFO_MODES - 1);
    l->cycle = LFO_SYNC_CYCLE[lfo_sync_rate_index(rate)];
    l->inc = l->mode == LFO_SYNC ? 0xFFFFFFFFu / l->cycle : lfo_inc(rate);
    l->start = (uint32_t)clamp(start, 0, 127) << 25;
    l->phase = l->start;
    l->held = mod_random();
}

/* the value of one numbered cycle, so voices that share a clock share it. They need not have been sounding when it began */
static AUDIO_INLINE int32_t lfo_shared_random(uint32_t cycle_number)
{
    uint32_t x = (cycle_number + 1u) * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    return clamp((int32_t)(x >> 16) - 32768, -32767, 32767);
}

/* -32767..32767. clock counts render blocks: a free-running LFO reads its phase from it, so all its voices agree */
static AUDIO_INLINE int32_t lfo_block(lfo_t *l, uint32_t clock)
{
    uint32_t prev = l->phase, q;
    int32_t x;
    if (l->mode == LFO_SYNC)
        l->phase = (lfo_sync_pos % l->cycle) * l->inc + l->start;
    else if (l->mode == LFO_FREE)
        l->phase = clock * l->inc + l->start;
    else
        l->phase = prev + l->inc;
    if (l->mode == LFO_RETRIGGER && l->phase < prev)
        l->held = mod_random();
    if (l->shape == LFO_SH && l->mode == LFO_SYNC)
        l->held = lfo_shared_random((lfo_sync_pos + (l->start >> 25) * l->cycle / 128u) / l->cycle);
    if (l->shape == LFO_SH && l->mode == LFO_FREE)
        l->held = lfo_shared_random((uint32_t)(((uint64_t)clock * l->inc + l->start) >> 32));
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

static AUDIO_INLINE void chain_filter_coef(chain_t *c, int32_t cut_q8, int32_t res_q8)
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
static AUDIO_INLINE void chain_filter_update(chain_t *c, const pkt_t *p)
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
    env_start(&c->mod2_env, p->v[PV_MOD2_A], p->v[PV_MOD2_D], p->v[PV_MOD2_S], p->v[PV_MOD2_R]);
    lfo_start(&c->lfo[0], p->v[PV_LFO1_SHAPE], p->v[PV_LFO1_PHASE], p->v[PV_LFO1_RATE], p->v[PV_LFO1_START]);
    lfo_start(&c->lfo[1], p->v[PV_LFO2_SHAPE], p->v[PV_LFO2_PHASE], p->v[PV_LFO2_RATE], p->v[PV_LFO2_START]);
    for (i = 0; i < MOD_DSTS; i++)
        c->off[i] = 0;
    c->random = clamp(mod_random(), -32767, 32767);
    c->index_routed = 0;
    for (i = 0; i < MOD_SLOTS; i++)
        c->index_routed |= p->v[PV_SLOT_DST0 + i] == MOD_DST_INDEX && p->v[PV_SLOT_SRC0 + i] != MOD_SRC_OFF;
    c->lfo_rate_off[0] = c->lfo_rate_off[1] = 0;
    c->gain_l = c->gain_r = 0;
    c->cut_q8 = c->res_q8 = -1;
    chain_filter_update(c, p);
    for (i = 0; i < 2u; i++) {
        c->path[i].ic1 = c->path[i].ic2 = 0;
        c->path[i].hold = 0;
        c->path[i].hold_left = 0;
        c->path[i].shape_level = c->path[i].shape_gain = 0;
    }
}

static AUDIO_INLINE void chain_release(chain_t *c)
{
    env_release(&c->amp_env);
    env_release(&c->mod_env);
    env_release(&c->mod2_env);
}

/* a free LFO's phase comes from its rate and the clock, so a moving rate would throw the phase around */
static AUDIO_INLINE void chain_lfo_rate(chain_t *c, uint32_t k, int32_t rate)
{
    int32_t offset = c->off[MOD_DST_LFO1_RATE + k] >> 8;
    if (offset != c->lfo_rate_off[k] && c->lfo[k].mode == LFO_RETRIGGER) {
        c->lfo_rate_off[k] = offset;
        c->lfo[k].inc = lfo_inc(rate + offset);
    }
}

/* runs the sources and the slots for one block; the amount at 127 and a full source move a value by 127 */
static AUDIO_INLINE void chain_mod(chain_t *c, const pkt_t *p, uint32_t clock)
{
    int32_t src[MOD_SRCS];
    uint32_t i;
    env_block(&c->mod_env);
    env_block(&c->mod2_env);
    src[MOD_SRC_OFF] = 0;
    src[MOD_SRC_ENV] = c->mod_env.env >> 9;
    src[MOD_SRC_ENV2] = c->mod2_env.env >> 9;
    src[MOD_SRC_LFO1] = lfo_block(&c->lfo[0], clock);
    src[MOD_SRC_LFO2] = lfo_block(&c->lfo[1], clock);
    src[MOD_SRC_VEL] = clamp(p->v[PV_VEL], 0, 127) * 258;
    src[MOD_SRC_RANDOM] = c->random;
    src[MOD_SRC_FOLLOW] = follower_level;
    for (i = 0; i < MOD_DSTS; i++)
        c->off[i] = 0;
    for (i = 0; i < MOD_SLOTS; i++) {
        int32_t from = clamp(p->v[PV_SLOT_SRC0 + i], 0, MOD_SRCS - 1), to = clamp(p->v[PV_SLOT_DST0 + i], 0, MOD_DSTS - 1);
        c->off[to] += (src[from] * p->v[PV_SLOT_AMT0 + i]) >> 7;
    }
    chain_lfo_rate(c, 0, p->v[PV_LFO1_RATE]);
    chain_lfo_rate(c, 1, p->v[PV_LFO2_RATE]);
}

/* Q15. Routes to the index scale it, so an envelope at 127 opens it fully at the envelope's peak. With no route it stays at its knob */
static AUDIO_INLINE int32_t chain_fm_index_q15(const chain_t *c)
{
    return c->index_routed ? clamp((c->off[MOD_DST_INDEX] * 129) >> 7, 0, 32768) : 32768;
}

/* the destination that feeds a knob's value, or MOD_DST_OFF. Each engine's TIMBRE offset moves one of its knobs */
static uint32_t mod_dst_of_value(uint32_t id, int32_t engine)
{
    switch (id) {
    case PV_NOTE: return MOD_DST_PITCH;
    case PV_FLT_CUT: return MOD_DST_CUT;
    case PV_FLT_RES: return MOD_DST_RES;
    case PV_LEVEL: return MOD_DST_LEVEL;
    case PV_PAN: return MOD_DST_PAN;
    case PV_DRIVE: return MOD_DST_DRIVE;
    case PV_SHAPER: return MOD_DST_SHAPER;
    case PV_CRUSH: return MOD_DST_CRUSH;
    case PV_RATE: return MOD_DST_RATE;
    case PV_LFO1_RATE: return MOD_DST_LFO1_RATE;
    case PV_LFO2_RATE: return MOD_DST_LFO2_RATE;
    case PV_E1: return engine == ENGINE_PERC || engine == ENGINE_WAVE ? MOD_DST_TIMBRE : MOD_DST_OFF;
    case PV_E2: return engine == ENGINE_SWARM ? MOD_DST_TIMBRE : MOD_DST_OFF;
    case PV_E4:
    case PV_E5: return engine == ENGINE_FM ? MOD_DST_INDEX : MOD_DST_OFF;
    default: return MOD_DST_OFF;
    }
}

/* the slot that holds this route, or else the first that does nothing, or -1. A route to the index at amount 0 still closes the index */
static int32_t mod_route_slot(const pkt_t *p, int32_t src, int32_t dst)
{
    uint32_t i;
    for (i = 0; i < MOD_SLOTS; i++)
        if (p->v[PV_SLOT_SRC0 + i] == src && p->v[PV_SLOT_DST0 + i] == dst)
            return (int32_t)i;
    for (i = 0; i < MOD_SLOTS; i++) {
        int32_t from = p->v[PV_SLOT_SRC0 + i], to = p->v[PV_SLOT_DST0 + i];
        if (from == MOD_SRC_OFF || to == MOD_DST_OFF || (p->v[PV_SLOT_AMT0 + i] == 0 && to != MOD_DST_INDEX))
            return (int32_t)i;
    }
    return -1;
}

/* Q12 gains of a source on its own side and the other. Near rises 3 dB as far falls, which holds the power of uncorrelated sources */
#define STEREO_UNITY_Q12 4096
#define STEREO_NEAR_RISE_Q12 1697
#define STEREO_SOURCE_Q15 23170  /* -3 dB: two uncorrelated sources where a mono voice has one */
static AUDIO_INLINE void stereo_gains(int32_t spread, int32_t *near, int32_t *far)
{
    int32_t amount = clamp(spread, 0, 127);
    *near = STEREO_UNITY_Q12 + STEREO_NEAR_RISE_Q12 * amount / 127;
    *far = STEREO_UNITY_Q12 - STEREO_UNITY_Q12 * amount / 127;
}

/* a wide voice's left and right sources to left and right, in place, plus what stays in both */
static AUDIO_INLINE void stereo_mix(int32_t *left, int32_t *right, const int32_t *both, int32_t spread)
{
    int32_t near, far;
    uint32_t i;
    stereo_gains(spread, &near, &far);
    for (i = 0; i < CTL; i++) {
        int32_t x = left[i], y = right[i];
        left[i] = ((x * near + y * far) >> 12) + both[i];
        right[i] = ((y * near + x * far) >> 12) + both[i];
    }
}

static AUDIO_INLINE int32_t softclip(int32_t x)
{
    int32_t a = x < 0 ? -x : x, i = a >> 8, y;
    if (i >= 256)
        y = TANH_Q15[256];
    else
        y = TANH_Q15[i] + (((TANH_Q15[i + 1] - TANH_Q15[i]) * (a & 255)) >> 8);
    return x < 0 ? -y : y;
}

/* folds any value into -32768..32767 in one step */
static AUDIO_INLINE int32_t wavefold(int32_t x)
{
    uint32_t u = ((uint32_t)x + 32768u) & 0x1FFFFu;
    return u < 65536u ? (int32_t)u - 32768 : 98303 - (int32_t)u;
}

/* folding alone would hold the level at full scale until the driven signal falls under it. This gain follows the input's
 * level until then. Below that point the folded signal falls by itself, and 1 / drive scales it back to the input's level.
 * The follower's 47 ms is long enough that a low note's own cycle does not move the gain */
static AUDIO_INLINE void chain_shape(chain_path_t *c, int32_t *x, int32_t shape)
{
    int32_t drive_q4 = 16 + shape * shape * 240 / 16129;     /* 127 drives by 16 */
    int32_t floor = 32768 * 16 / drive_q4, peak = 0, target, ramp = c->shape_gain * (1 << CTL_BITS);
    uint32_t i;
    for (i = 0; i < CTL; i++) {
        int32_t a = x[i] < 0 ? -x[i] : x[i];
        peak = a > peak ? a : peak;
    }
    peak = clamp(peak, 0, SHAPE_FULL) * (32768 / SHAPE_FULL);
    c->shape_level -= c->shape_level >> 6;
    c->shape_level = peak > c->shape_level ? peak : c->shape_level;
    target = c->shape_level > floor ? c->shape_level : floor;
    for (i = 0; i < CTL; i++) {
        ramp += target - c->shape_gain;
        x[i] = ((wavefold((clamp(x[i], -60000, 60000) * drive_q4) >> 2) >> 2) * (ramp >> CTL_BITS)) >> 15;
    }
    c->shape_gain = target;
}

/* cheap effects in place; each is skipped at 0 */
static AUDIO_INLINE void chain_effects(const chain_t *c, chain_path_t *path, const pkt_t *p, int32_t *x)
{
    int32_t drive = clamp(p->v[PV_DRIVE] + (c->off[MOD_DST_DRIVE] >> 8), 0, 127);
    int32_t shape = clamp(p->v[PV_SHAPER] + (c->off[MOD_DST_SHAPER] >> 8), 0, 127);
    int32_t crush = clamp(p->v[PV_CRUSH] + (c->off[MOD_DST_CRUSH] >> 8), 0, 127);
    int32_t rate = clamp(p->v[PV_RATE] + (c->off[MOD_DST_RATE] >> 8), 0, 127);
    uint32_t i;
    if (shape)
        chain_shape(path, x, shape);
    if (drive)
        for (i = 0; i < CTL; i++)
            x[i] = softclip(clamp(x[i], -60000, 60000) * (8 + drive) >> 3);
    if (crush) {
        int32_t mask = ~((1 << (1 + crush * 12 / 127)) - 1);
        for (i = 0; i < CTL; i++)
            x[i] &= mask;
    }
    if (rate) {
        uint32_t every = 1u + (uint32_t)rate / 3u;
        for (i = 0; i < CTL; i++) {
            if (!path->hold_left) {
                path->hold = x[i];
                path->hold_left = every;
            }
            path->hold_left--;
            x[i] = path->hold;
        }
    }
}

static AUDIO_INLINE void chain_filter(const chain_t *c, chain_path_t *path, uint32_t type, int32_t *x)
{
    uint32_t i;
    if (type == FLT_OFF)
        return;
    for (i = 0; i < CTL; i++) {
        int32_t in = x[i], v3 = in - path->ic2;
        int32_t v1 = (c->a1 * path->ic1 + c->a2 * v3) >> 13;
        int32_t v2 = path->ic2 + ((c->a2 * path->ic1 + c->a3 * v3) >> 13);
        path->ic1 = clamp(2 * v1 - path->ic1, -150000, 150000);
        path->ic2 = clamp(2 * v2 - path->ic2, -150000, 150000);
        x[i] = type == FLT_LP ? v2 : type == FLT_BP ? v1 : in - ((c->damp * v1) >> 12) - v2;
    }
}

/* after the amp envelope and before LEVEL, so a silent track can still drive it */
static AUDIO_INLINE void chain_follow_send(const chain_t *c, const int32_t *x, int32_t send)
{
    int32_t peak = 0, level;
    uint32_t i;
    for (i = 0; i < CTL; i++) {
        int32_t a = x[i] < 0 ? -x[i] : x[i];
        peak = a > peak ? a : peak;
    }
    level = clamp(peak, 0, SHAPE_FULL) * (32768 / SHAPE_FULL);
    level = ((level * (c->amp_env.env >> 9)) >> 15) * send >> 7;
    if (level > follower_input)
        follower_input = level;
}

/* a block from each channel to l and r. Inlined with one channel for both, a mono voice pays for one clamp */
static AUDIO_INLINE void chain_output(chain_t *c, int32_t *x, const int32_t *x_right, int32_t *l, int32_t *r, int32_t target_l, int32_t target_r)
{
    int32_t ramp_l = c->gain_l * (1 << CTL_BITS), ramp_r = c->gain_r * (1 << CTL_BITS);
    uint32_t i;
    for (i = 0; i < CTL; i++) {
        int32_t y_l = clamp(x[i], -60000, 60000), y_r = clamp(x_right[i], -60000, 60000);
        ramp_l += target_l - c->gain_l;
        ramp_r += target_r - c->gain_r;
        y_l = (y_l * (ramp_l >> (CTL_BITS + 4))) >> 11;
        y_r = (y_r * (ramp_r >> (CTL_BITS + 4))) >> 11;
        l[i] += y_l;
        r[i] += y_r;
        x[i] = (y_l + y_r) >> 1;
    }
}

/* adds one block to l, r and the send buses; x is the engine's block and is overwritten. x_right: a wide voice's right channel, or 0. 0 when the envelope has ended */
static AUDIO_INLINE int chain_render(chain_t *c, const pkt_t *p, int32_t *x, int32_t *x_right, int32_t *l, int32_t *r, int32_t *delay_bus, int32_t *reverb_bus)
{
    int32_t vel = clamp(p->v[PV_VEL], 0, 127);
    int32_t lvl = LEVEL_Q12[clamp(p->v[PV_LEVEL] + (c->off[MOD_DST_LEVEL] >> 8), 0, 127)] * vel * vel / (127 * 127);  /* velocity: 64 is -12 dB */
    int32_t pan = clamp(p->v[PV_PAN] + (c->off[MOD_DST_PAN] >> 8), -63, 63);
    int32_t gl = 4096 - (pan > 0 ? pan * 4096 / 63 : 0), gr = 4096 + (pan < 0 ? pan * 4096 / 63 : 0);
    int32_t delay_q8 = clamp(p->v[PV_SEND_DELAY], 0, 127) * 2, reverb_q8 = clamp(p->v[PV_SEND_REVERB], 0, 127) * 2;
    int32_t amp, target_l, target_r;
    uint32_t i, type = (uint32_t)clamp(p->v[PV_FLT_TYPE], 0, FLT_TYPES - 1);
    chain_effects(c, &c->path[0], p, x);
    if (x_right)
        chain_effects(c, &c->path[1], p, x_right);
    if (type != FLT_OFF)
        chain_filter_update(c, p);
    chain_filter(c, &c->path[0], type, x);
    if (x_right)
        chain_filter(c, &c->path[1], type, x_right);
    env_block(&c->amp_env);
    if (p->v[PV_SEND_FOLLOW] > 0) {
        chain_follow_send(c, x, p->v[PV_SEND_FOLLOW]);
        if (x_right)
            chain_follow_send(c, x_right, p->v[PV_SEND_FOLLOW]);
    }
    amp = ((c->amp_env.env >> 9) * lvl) >> 12;
    target_l = (amp * gl) >> 12;
    target_r = (amp * gr) >> 12;
    if (x_right)
        chain_output(c, x, x_right, l, r, target_l, target_r);
    else
        chain_output(c, x, x, l, r, target_l, target_r);
    if (delay_q8)
        for (i = 0; i < CTL; i++)
            delay_bus[i] += (x[i] * delay_q8) >> 8;
    if (reverb_q8)
        for (i = 0; i < CTL; i++)
            reverb_bus[i] += (x[i] * reverb_q8) >> 8;
    c->gain_l = target_l;
    c->gain_r = target_r;
    return c->amp_env.stage != ENV_DONE;
}
