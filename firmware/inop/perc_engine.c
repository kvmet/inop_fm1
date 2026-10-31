/* SPDX-License-Identifier: GPL-3.0-only */
/* The Perc engine: six decaying sines and noise, summed, shaped at the onset and folded. A trigger's length means nothing
 * to it: E1 sets how long it rings. No hardware here: tests/inop_test.c runs this on the host. */
enum { PERC_SKIN, PERC_METAL, PERC_MODES };
#define PERC_PARTIALS 6u
#define PERC_PEAK 8000           /* the sum's full scale at the output; a click on top reaches 1.5 times this */
#define PERC_ENV_FULL (1 << 23)
#define PERC_ENV_END (32 << 8)   /* -60 dB of full */
#define PERC_METAL_SHIFT 18      /* a full-scale modulator moves phase by 2 cycles */
#define PERC_BEND_UNITS 6        /* 1/16 semitones per value: 127 starts 47 semitones up */
#define PERC_NOTE0_INC 49765u    /* phase per sample at note 0 (8.18 Hz), >> 4 */

typedef struct {
    uint32_t phase[PERC_PARTIALS];
    int32_t env[PERC_PARTIALS];      /* Q23 */
    int32_t decay_k[PERC_PARTIALS];  /* Q16, per block */
    int32_t level[PERC_PARTIALS];    /* Q15; these and the noise's share add to full scale */
    int32_t gain[PERC_PARTIALS];     /* Q15: where the last block's ramp ended */
    int32_t noise_env, noise_k, noise_level, noise_gain, click_env;
    int32_t bend, bend_k;            /* bend: 1/16 semitones, Q8 */
    int32_t onset, onset_inc;        /* Q15 */
    int32_t drive_q4, punch_floor, punch;
    int32_t loudness;                /* Q15: the envelopes' sum */
    uint32_t rng;
} perc_voice_t;

/* a partial's frequency over the note's. 64 spreads them as harmonics */
static uint32_t perc_ratio_q8(int32_t spread, uint32_t partial) { return 256u + partial * (uint32_t)clamp(spread, 0, 127) * 4u; }

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

static inline int32_t perc_fall(int32_t env, int32_t k) { return ((env >> 8) * k) >> 8; }

static void perc_start(perc_voice_t *S, const pkt_t *p, uint32_t seed)
{
    uint32_t blocks = env_blocks(p->v[PV_E1]), i;
    int32_t harm = clamp(p->v[PV_E3], 0, 127), fold = clamp(p->v[PV_E4], 0, 127), bend = clamp(p->v[PV_E5], 0, 127);
    int32_t noise = clamp(p->v[PV_E6], 0, 127), attack = clamp(p->v[PV_E7], 0, 127);
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
    S->noise_k = perc_decay_k(blocks / 2u + 1u);
    S->noise_level = noise_share;
    S->noise_gain = 0;
    S->click_env = (127 - attack) * (PERC_ENV_FULL / 254);
    S->bend = bend * PERC_BEND_UNITS * 256;
    S->bend_k = perc_decay_k(235u + blocks / 4u);    /* 235 blocks: a 25 ms time constant */
    S->onset = 0;
    S->onset_inc = 32767 / (1 + attack * attack / 256);
    S->drive_q4 = 16 + fold * fold * 240 / 16129;    /* FOLD at 127 drives by 16 */
    S->punch_floor = 32768 * 16 / S->drive_q4;
    S->punch = 32767;
    S->loudness = 32767;
    S->rng = seed | 1u;
}

/* folds any value into -32768..32767 in one step */
static inline int32_t perc_fold(int32_t x)
{
    uint32_t u = ((uint32_t)x + 32768u) & 0x1FFFFu;
    return u < 65536u ? (int32_t)u - 32768 : 98303 - (int32_t)u;
}

/* phase per sample. pitch: 1/16 semitones above note 0 */
static uint32_t perc_inc(int32_t pitch)
{
    uint32_t q = (uint32_t)clamp(pitch, 0, 127 * 16 + 15), semi = q >> 4;
    uint32_t v = ((PERC_NOTE0_INC * SEMI_Q12[semi % 12u]) >> 8) << (semi / 12u);
    return v + (v >> 8) * (q & 15u);                 /* one 1/16 semitone is 0.36 %, and 1/256 is 0.39 % */
}

/* adds one block to out; 0 when every partial and the noise have ended. pitch: an offset in 1/16 semitones */
static int perc_render(perc_voice_t *S, const pkt_t *p, int32_t *out, int32_t pitch)
{
    int32_t x[CTL], mod[CTL];
    int metal = clamp(p->v[PV_E0], 0, 127) % PERC_MODES == PERC_METAL;
    int32_t spread = p->v[PV_E2], total = 0, ramp, target;
    uint32_t base = perc_inc(p->v[PV_NOTE] * 16 + pitch + (S->bend >> 8)), i, s;
    int alive = 0;
    for (s = 0; s < CTL; s++)
        x[s] = mod[s] = 0;
    S->bend = ((S->bend >> 4) * S->bend_k) >> 12;
    for (i = PERC_PARTIALS; i-- > 0;) {
        uint32_t inc, ph;
        S->env[i] = S->env[i] < PERC_ENV_END ? 0 : perc_fall(S->env[i], S->decay_k[i]);
        target = (S->level[i] * (S->env[i] >> 8)) >> 15;
        total += target;
        inc = (uint32_t)(((uint64_t)base * perc_ratio_q8(spread, i)) >> 8);
        if (!target && !S->gain[i]) {                /* an ended partial costs nothing, and modulates nothing */
            for (s = 0; metal && s < CTL; s++)
                mod[s] = 0;
            continue;
        }
        alive = 1;
        if (inc >= 0x80000000u) {                    /* over Nyquist it would alias */
            S->gain[i] = target;
            for (s = 0; metal && s < CTL; s++)
                mod[s] = 0;
            continue;
        }
        ramp = S->gain[i] * (1 << CTL_BITS);
        ph = S->phase[i];
        if (metal) {
            for (s = 0; s < CTL; s++) {
                int32_t y;
                ramp += target - S->gain[i];
                y = (SINE[(ph + (uint32_t)mod[s] * (1u << PERC_METAL_SHIFT)) >> 22] * (ramp >> CTL_BITS)) >> 15;
                x[s] += y;
                mod[s] = y;
                ph += inc;
            }
        } else {
            for (s = 0; s < CTL; s++) {
                ramp += target - S->gain[i];
                x[s] += (SINE[ph >> 22] * (ramp >> CTL_BITS)) >> 15;
                ph += inc;
            }
        }
        S->phase[i] = ph;
        S->gain[i] = target;
    }
    S->noise_env = S->noise_env < PERC_ENV_END ? 0 : perc_fall(S->noise_env, S->noise_k);
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
    if (S->onset < 32767) {
        int32_t from = S->onset;
        S->onset = from + S->onset_inc > 32767 ? 32767 : from + S->onset_inc;
        ramp = from * (1 << CTL_BITS);
        for (s = 0; s < CTL; s++) {
            ramp += S->onset - from;
            x[s] = (x[s] * (ramp >> CTL_BITS)) >> 15;
        }
    }
    S->loudness = total;
    /* folding holds the level at full scale until the driven sum falls under it. This gain brings back the envelopes' fall
     * until then. Below that point the folded sum falls by itself, and 1 / drive scales it back to the unfolded level */
    target = total > S->punch_floor ? total : S->punch_floor;
    if (S->drive_q4 == 16) {
        for (s = 0; s < CTL; s++)
            out[s] += (x[s] * PERC_PEAK) >> 15;
    } else {
        ramp = S->punch * (1 << CTL_BITS);
        for (s = 0; s < CTL; s++) {
            ramp += target - S->punch;
            out[s] += (((perc_fold((x[s] * S->drive_q4) >> 4) * (ramp >> CTL_BITS)) >> 15) * PERC_PEAK) >> 15;
        }
        S->punch = target;
    }
    return alive;
}
