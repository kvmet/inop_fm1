/* SPDX-License-Identifier: GPL-3.0-only */
/* The Perc engine: six sines and noise, each falling to the amp envelope's sustain over its decay, and summed.
 * The chain's amp envelope gives the attack and the release. No hardware here: tests/inop_test.c runs this on the host. */
enum { PERC_TONE, PERC_SKIN, PERC_BAR, PERC_METAL, PERC_MODES };
#define PERC_PARTIALS 6u
#define PERC_PEAK 8000           /* the sum's full scale at the output; a click on top reaches 1.5 times this */
#define PERC_ENV_FULL (1 << 23)
#define PERC_ENV_END (32 << 8)   /* -60 dB of full */
#define PERC_FM_SHIFT 18         /* a full-scale modulator moves phase by 2 cycles */
#define PERC_BEND_UNITS 6        /* 1/16 semitones per value: 127 starts 47 semitones up */

typedef struct {
    uint32_t phase[PERC_PARTIALS];
    int32_t env[PERC_PARTIALS];      /* Q23 */
    int32_t decay_k[PERC_PARTIALS];  /* Q16, per block */
    int32_t level[PERC_PARTIALS];    /* Q15; these and the noise's share add to full scale */
    int32_t gain[PERC_PARTIALS];     /* Q15: where the last block's ramp ended */
    int32_t noise_env, noise_k, noise_level, noise_gain, click_env;
    int32_t fm_k;
    int32_t sustain;                 /* Q23: where every envelope falls to */
    int32_t fm_env, fm_gain;         /* fm_env: Q23. fm_gain: Q15 */
    int32_t bend, bend_k;            /* bend: 1/16 semitones, Q8 */
    int32_t loudness;                /* Q15: the envelopes' sum */
    uint32_t rng;
} perc_voice_t;

/* partial frequencies over the note's, Q8: harmonics, circular membrane modes, free bar modes, TR-808 cymbal oscillators */
static const uint16_t PERC_RATIOS[PERC_MODES][PERC_PARTIALS] = {
    {256, 512, 768, 1024, 1280, 1536},
    {256, 408, 547, 588, 679, 747},
    {256, 706, 1383, 2287, 3416, 4771},
    {256, 380, 461, 652, 673, 998},
};

static AUDIO_INLINE uint32_t perc_mode(int32_t mode) { return (uint32_t)clamp(mode, 0, 127) % PERC_MODES; }

/* spread 0 is unison, and 64 is the mode's table */
static AUDIO_INLINE uint32_t perc_ratio_q8(int32_t mode, int32_t spread, uint32_t partial)
{
    return 256u + (PERC_RATIOS[perc_mode(mode)][partial] - 256u) * (uint32_t)clamp(spread, 0, 127) / 64u;
}

/* each partial's level falls by harm / 127 from the last. They add to share */
static void perc_levels(int32_t harm, int32_t share, int32_t *level)
{
    int32_t raw = 32767, sum = 0;
    uint32_t i;
    for (i = 0; i < PERC_PARTIALS; i++) {
        level[i] = raw;
        sum += raw >> 3;
        raw = raw * clamp(harm, 0, 127) / 127;
    }
    for (i = 0; i < PERC_PARTIALS; i++)
        level[i] = (level[i] >> 3) * share / sum;
}

/* blocks: how long until -60 dB */
static int32_t perc_decay_k(uint32_t blocks)
{
    return clamp(65536 - (int32_t)(452984u / blocks), 0, 65535);     /* 452984 is 6.91 in Q16: ln(1000) */
}

static AUDIO_INLINE int32_t perc_fall(int32_t env, int32_t floor, int32_t k)
{
    int32_t over = env - floor;
    return over < PERC_ENV_END ? floor : floor + (((over >> 8) * k) >> 8);
}

static void perc_start(perc_voice_t *S, const pkt_t *p, uint32_t seed)
{
    uint32_t blocks = env_blocks(p->v[PV_AMP_D]), bend_blocks, i;
    int32_t harm = clamp(p->v[PV_E3], 0, 127), bend = clamp(p->v[PV_E4], 0, 127);
    int32_t noise = clamp(p->v[PV_E5], 0, 127), click = clamp(p->v[PV_E6], 0, 127);
    int32_t noise_share = noise * 129;               /* NOISE at 127 is half of the sum */
    perc_levels(harm, 32767 - noise_share, S->level);
    for (i = 0; i < PERC_PARTIALS; i++) {
        uint32_t partial_blocks = blocks * 32u / (32u + i * (uint32_t)(127 - harm));
        S->phase[i] = 0;
        S->env[i] = PERC_ENV_FULL;
        S->decay_k[i] = perc_decay_k(partial_blocks ? partial_blocks : 1u);
        S->gain[i] = 0;
    }
    S->noise_env = PERC_ENV_FULL;
    S->fm_k = perc_decay_k(blocks / 2u + 1u);
    S->noise_level = noise_share;
    S->noise_gain = 0;
    S->click_env = click * (PERC_ENV_FULL / 254);
    S->sustain = (PERC_ENV_FULL / 127) * clamp(p->v[PV_AMP_S], 0, 127);
    S->fm_env = PERC_ENV_FULL;
    S->fm_gain = fm_squared(p->v[PV_E1], 32767);
    S->bend = bend * PERC_BEND_UNITS * 256;
    bend_blocks = 235u + blocks / 4u;                /* 235 blocks: a 25 ms time constant */
    S->bend_k = perc_decay_k(bend_blocks);
    S->noise_k = perc_decay_k(bend_blocks < blocks ? bend_blocks : blocks);  /* falls with the bend, and never outlasts the decay */
    S->loudness = 32767;
    S->rng = seed | 1u;
}

/* adds one block to out; 0 when every partial and the noise have ended. pitch: an offset in 1/16 semitones. timbre: an offset to FM */
static AUDIO_INLINE int perc_render(perc_voice_t *S, const pkt_t *p, int32_t *out, int32_t pitch, int32_t timbre)
{
    int32_t x[CTL], mod[CTL], depth[CTL];
    int32_t mode = p->v[PV_E0], spread = p->v[PV_E2], total = 0, ramp, target, fm_on;
    uint32_t base = note_inc(p->v[PV_NOTE] * 16 + pitch + (S->bend >> 8)), i, s;
    int alive = 0;
    S->bend = ((S->bend >> 4) * S->bend_k) >> 12;
    S->fm_env = perc_fall(S->fm_env, S->sustain, S->fm_k);
    target = (fm_squared(p->v[PV_E1] + timbre, 32767) * (S->fm_env >> 8)) >> 15;
    fm_on = target || S->fm_gain;
    ramp = S->fm_gain * (1 << CTL_BITS);
    for (s = 0; s < CTL; s++) {
        x[s] = mod[s] = 0;
        ramp += target - S->fm_gain;
        depth[s] = ramp >> CTL_BITS;
    }
    S->fm_gain = target;
    for (i = PERC_PARTIALS; i-- > 0;) {
        uint32_t inc, ph;
        int32_t from = S->gain[i];
        S->env[i] = perc_fall(S->env[i], S->sustain, S->decay_k[i]);
        target = (S->level[i] * (S->env[i] >> 8)) >> 15;
        total += target;
        alive |= target || from;
        S->gain[i] = target;
        inc = (uint32_t)(((uint64_t)base * perc_ratio_q8(mode, spread, i)) >> 8);
        if (inc >= 0x80000000u || (!fm_on && !target && !from)) {    /* over Nyquist it would alias */
            for (s = 0; fm_on && s < CTL; s++)
                mod[s] = 0;
            continue;
        }
        ramp = from * (1 << CTL_BITS);
        ph = S->phase[i];
        if (fm_on) {                                 /* a modulator's depth does not depend on its level, so a silent partial still modulates */
            for (s = 0; s < CTL; s++) {
                int32_t y = SINE[(ph + (uint32_t)mod[s] * (1u << PERC_FM_SHIFT)) >> 22];
                ramp += target - from;
                x[s] += (y * (ramp >> CTL_BITS)) >> 15;
                mod[s] = (y * depth[s]) >> 15;
                ph += inc;
            }
        } else {
            for (s = 0; s < CTL; s++) {
                ramp += target - from;
                x[s] += (SINE[ph >> 22] * (ramp >> CTL_BITS)) >> 15;
                ph += inc;
            }
        }
        S->phase[i] = ph;
    }
    S->noise_env = perc_fall(S->noise_env, 0, S->noise_k);    /* a transient: SUS holds only the tone */
    S->click_env >>= 1;
    target = ((S->noise_level * (S->noise_env >> 8)) >> 15) + (S->click_env >> 8);
    total += (S->noise_level * (S->noise_env >> 8)) >> 15;
    if (target || S->noise_gain) {
        uint32_t r = S->rng;
        ramp = S->noise_gain * (1 << CTL_BITS);
        for (s = 0; s < CTL; s++) {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            ramp += target - S->noise_gain;
            x[s] += (((int32_t)r >> 16) * (ramp >> CTL_BITS)) >> 15;
        }
        S->rng = r;
        S->noise_gain = target;
        alive = 1;
    }
    S->loudness = total;
    for (s = 0; s < CTL; s++)
        out[s] += (x[s] * PERC_PEAK) >> 15;
    return alive;
}
