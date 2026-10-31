/* SPDX-License-Identifier: GPL-3.0-only */
/* The SWARM engine: seven detuned oscillators and a sub, each morphing from saw through square to a narrow pulse, with
 * optional hard sync. Between edges each oscillator is a line and their sum is one line, so an oscillator costs
 * a little per block and more only per edge. No hardware here: tests/inop_test.c runs this on the host. */
enum { SWARM_UNISON, SWARM_OCTAVES, SWARM_MODES };
#define SWARM_OSCS 7u
#define SWARM_CENTER 3u
#define SWARM_PEAK 8000
#define SWARM_DRIFT_BLOCKS 128u  /* one oscillator takes a new drift target this often: 650 ms for all seven */
#define SWARM_NARROW 0x0CCCCCCCu /* the narrowest pulse: 5 % */

/* relative detune, Q16: Roland JP-8000 values as measured by A. Szabo, "How to Emulate the Super Saw" (2010) */
static const int16_t SWARM_DETUNE[SWARM_OSCS] = {-7210, -4121, -1279, 0, 1305, 4074, 7042};
static const uint16_t SWARM_OCTAVE_Q8[SWARM_MODES][SWARM_OSCS] = {
    {256, 256, 256, 256, 256, 256, 256},
    {128, 128, 256, 256, 256, 512, 512},
};

typedef struct {
    uint32_t phase[SWARM_OSCS], master[SWARM_OSCS], sub_phase;
    uint32_t offset[SWARM_OSCS];     /* the second saw's phase over the first's */
    int32_t drift[SWARM_OSCS], drift_to[SWARM_OSCS];   /* Q24 of the frequency */
    int32_t level[SWARM_OSCS], sub_level;              /* Q15 */
    int32_t morph;                   /* Q15: where the last block's ramp ended */
    int32_t carry;                   /* Q8: the smoothing of an edge at the last block's end, for this block's first sample */
    int32_t shape_gain, dc;
    uint32_t rng, clock;
} swarm_voice_t;

static uint32_t swarm_mode(int32_t v) { return (uint32_t)clamp(v, 0, SWARM_MODES - 1); }

/* SHAPE: 0 is a saw, 64 is a square, and 127 is a narrow pulse */
static void swarm_shape(int32_t shape, int32_t *morph, uint32_t *width)
{
    int32_t v = clamp(shape, 0, 127);
    *morph = v >= 64 ? 32767 : v * 512;
    *width = v <= 64 ? 0x80000000u : 0x80000000u - (uint32_t)(v - 64) * ((0x80000000u - SWARM_NARROW) / 63u);
}

/* Q15, so that SHAPE changes the tone and not the loudness: a square is louder than a saw, and a narrow pulse quieter */
static int32_t swarm_shape_gain(int32_t shape)
{
    int32_t v = clamp(shape, 0, 127);
    return v <= 64 ? 32767 - v * 215 : 19007 + (v - 64) * 340;
}

/* Q8. A saw minus a second saw: their ramps cancel in part, and their means stay 0 at every setting */
static inline int32_t swarm_line(uint32_t a, uint32_t b, int32_t morph)
{
    return ((int32_t)(a >> 8) - (1 << 23)) - ((morph * ((int32_t)(b >> 16) - 32768)) >> 7);
}

static inline int32_t swarm_wave(uint32_t ph, int32_t morph, uint32_t width) { return swarm_line(ph, ph + width, morph) >> 8; }

/* Q8, into d, the first difference of the block: a step of jump between samples e and e + 1, band-limited by polyBLEP.
 * t: Q15, how far past the edge sample e + 1 is */
static inline void swarm_edge(int32_t *d, int32_t *carry, uint32_t e, int32_t jump, int32_t t)
{
    int32_t x = t - 32768, j = jump >> 8;
    int32_t before = (j * (((x * x) >> 15) + x + x + 32768)) >> 8, after = (j * (t + t - ((t * t) >> 15) - 32768)) >> 8;
    d[e] += before;
    d[e + 1] -= before;
    if (e + 1u < CTL) {
        d[e + 1] += jump + after;
        d[e + 2] -= after;
    } else {
        *carry += after;
    }
}

static inline int32_t swarm_past_edge(uint32_t ph, uint32_t inc)
{
    uint32_t t = ph / ((inc >> 15) | 1u);
    return t > 32768u ? 32768 : (int32_t)t;
}

/* the wraps of a saw at ph on sample from, up to and including one onto sample to */
static inline void swarm_wraps(int32_t *d, int32_t *carry, uint32_t ph, uint32_t inc, uint32_t from, uint32_t to, int32_t jump)
{
    uint32_t left = 0u - ph - 1u;
    while ((uint64_t)inc * (to - from) > left) {
        uint32_t k = left / inc;
        ph += (k + 1u) * inc;
        swarm_edge(d, carry, from + k, jump, swarm_past_edge(ph, inc));
        from += k + 1u;
        left = 0u - ph - 1u;
    }
}

/* Q15, before the sum is kept in range. SIDES trades the center oscillator for the six around it */
static int32_t swarm_level(int32_t sides, uint32_t osc) { return osc == SWARM_CENTER ? 32767 - sides * 142 : sides * 258; }

static void swarm_start(swarm_voice_t *S, const pkt_t *p, uint32_t seed)
{
    int32_t mix = clamp(p->v[PV_E4], 0, 127), sub = clamp(p->v[PV_E5], 0, 127), center = swarm_level(mix, SWARM_CENTER), side = swarm_level(mix, 0);
    int32_t sum = center + 6 * side, mix_gain = sum < 32768 ? 2048 : 0x8000000 / sum;     /* Q11: keeps the oscillators' sum in range */
    uint32_t r = seed | 1u, width, i;
    swarm_shape(p->v[PV_E2], &S->morph, &width);
    for (i = 0; i < SWARM_OSCS; i++) {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        S->phase[i] = S->master[i] = r;  /* a random start: aligned saws would all jump at once */
        S->offset[i] = width;
        S->drift[i] = S->drift_to[i] = 0;
        S->level[i] = ((i == SWARM_CENTER ? center : side) * mix_gain) >> 11;
    }
    S->sub_phase = 0;
    S->sub_level = sub * 258;
    S->carry = 0;
    S->shape_gain = swarm_shape_gain(p->v[PV_E2]);
    S->dc = 0;
    S->rng = r;
    S->clock = 0;
}

static void swarm_drift(swarm_voice_t *S, int32_t depth)
{
    uint32_t i;
    if (++S->clock % SWARM_DRIFT_BLOCKS == 0u) {
        uint32_t r = S->rng;
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        S->rng = r;
        S->drift_to[(S->clock / SWARM_DRIFT_BLOCKS) % SWARM_OSCS] = (((int32_t)r >> 16) * depth) >> 11;
    }
    for (i = 0; i < SWARM_OSCS; i++)
        S->drift[i] += (S->drift_to[i] - S->drift[i]) >> 9;
}

/* edges per block, Q8, that a note makes: its cost beyond the fixed work. A second saw counts even at SHAPE 0, since TIMBRE can move it */
static uint32_t swarm_edges_q8(const pkt_t *p)
{
    int32_t shift = clamp(p->v[PV_E3], 0, 127);
    uint32_t base = note_inc(p->v[PV_NOTE] * 16), mode = swarm_mode(p->v[PV_E0]), sum = 0, i;
    for (i = 0; i < SWARM_OSCS; i++) {
        uint32_t inc = (uint32_t)(((uint64_t)base * SWARM_OCTAVE_Q8[mode][i]) >> 8);
        uint64_t sync = ((uint64_t)inc * (uint32_t)(256 + shift * 6)) >> 8;
        if (sync < 0x80000000u && (i == SWARM_CENTER || p->v[PV_E4]))
            sum += (uint32_t)(sync >> 18) + (shift ? inc >> 19 : 0u);    /* 2 saws, at 2^-19 edges per block for each unit of inc */
    }
    return sum;
}

/* adds one block to out. pitch: an offset in 1/16 semitones. timbre: an offset to SHAPE */
static void swarm_render(swarm_voice_t *S, const pkt_t *p, int32_t *out, int32_t pitch, int32_t timbre)
{
    int32_t d[CTL + 2u], sub[CTL];
    int32_t shape = clamp(p->v[PV_E2] + timbre, 0, 127), spread = clamp(p->v[PV_E1], 0, 127), shift = clamp(p->v[PV_E3], 0, 127);
    int32_t amount = spread * spread * 32767 / 16129, drift = clamp(p->v[PV_E6], 0, 127), morph_to, d_morph;
    int32_t gain_to = swarm_shape_gain(shape), ramp, line = 0, slope = 0, morph = S->morph;
    uint32_t base = note_inc(p->v[PV_NOTE] * 16 + pitch), width, mode = swarm_mode(p->v[PV_E0]), i, s;
    swarm_shape(shape, &morph_to, &width);
    d_morph = (morph_to - morph) / CTL;
    swarm_drift(S, drift * drift);       /* 127 moves each oscillator by up to 1.5 % */
    for (s = 0; s < CTL; s++)
        d[s] = sub[s] = 0;
    d[CTL] = d[CTL + 1u] = 0;
    d[0] = S->carry;
    d[1] = -S->carry;
    S->carry = 0;
    for (i = 0; i < SWARM_OSCS; i++) {
        int64_t ratio = 65536 + ((SWARM_DETUNE[i] * amount) >> 15) + (S->drift[i] >> 8);
        uint64_t inc64 = (((uint64_t)base * SWARM_OCTAVE_Q8[mode][i]) >> 8) * (uint64_t)ratio >> 16;
        uint64_t sync64 = (inc64 * (uint32_t)(256 + shift * 6)) >> 8;   /* 127 is nearly 4 times the oscillator's pitch */
        int32_t level = S->level[i], jump_b, moved;
        uint32_t a = S->phase[i], b, inc_m = (uint32_t)inc64, m = S->master[i], inc = (uint32_t)sync64, inc_b;
        int second = morph || morph_to;
        if (sync64 >= 0x80000000u || !level)    /* over Nyquist it would alias */
            continue;
        moved = (int32_t)((width + (uint32_t)(S->drift[i] >> 8) * (1u << 16)) - S->offset[i]) / (int32_t)CTL;
        inc_b = inc + (uint32_t)moved;
        b = a + S->offset[i];
        line += (int32_t)(((int64_t)swarm_line(a, b, morph) * level) >> 16);
        slope += (int32_t)((((((int64_t)inc - (((int64_t)inc_b * morph) >> 15)) >> 8) - ((d_morph * ((int32_t)(b >> 16) - 32768)) >> 7)) * level) >> 16);
        jump_b = ((morph + d_morph * (int32_t)(CTL / 2u)) * level) >> 7;
        for (s = 0; s < CTL;) {
            uint32_t to = CTL, left = 0u - m - 1u, steps;
            int hit = shift && (uint64_t)inc_m * (CTL - s) > left;
            if (hit)
                to = s + left / inc_m + 1u;
            swarm_wraps(d, &S->carry, a, inc, s, to, -256 * level);
            if (second)
                swarm_wraps(d, &S->carry, b, inc_b, s, to, jump_b);
            steps = to - s;
            a += inc * steps;
            b += inc_b * steps;
            m += inc_m * steps;
            s = to;
            if (hit) {                  /* the master wrapped: restart at the same point of the sample */
                uint32_t restart = (m / ((inc_m >> 16) | 1u)) * (inc >> 16), offset = b - a;
                int32_t at = morph + d_morph * (int32_t)to;
                int32_t jump = (int32_t)(((int64_t)(swarm_line(restart, restart + offset, at) - swarm_line(a, b, at)) * level) >> 16);
                swarm_edge(d, &S->carry, to - 1u, jump, swarm_past_edge(m, inc_m));
                a = restart;
                b = restart + offset;
            }
        }
        S->phase[i] = a;
        S->offset[i] = b - a;
        S->master[i] = m;
    }
    if (S->sub_level) {
        uint32_t ph = S->sub_phase, inc = base >> 1;
        for (s = 0; s < CTL; s++) {
            sub[s] = (SINE[ph >> 22] * S->sub_level) >> 16;
            ph += inc;
        }
        S->sub_phase = ph;
    }
    S->morph = morph_to;
    ramp = S->shape_gain * (1 << CTL_BITS);
    for (s = 0; s < CTL; s++) {
        int32_t y;
        line += d[s];
        y = (line >> 8) * 256 - S->dc;
        line += slope;
        S->dc += y >> 10;                                /* a one-pole high-pass, ~7 Hz: a sync restart moves the mean */
        ramp += gain_to - S->shape_gain;
        out[s] += ((((y >> 8) * (ramp >> (CTL_BITS + 2))) >> 12) + sub[s] * 2) * SWARM_PEAK >> 15;   /* the sub comes after SHAPE's gain */
    }
    S->shape_gain = gain_to;
}
