/* SPDX-License-Identifier: GPL-3.0-only */
/* The master: tilt, compressor, width, drive, limiter, soft clip, volume. A stage at its neutral value costs no CPU.
 * No hardware here: tests/inop_test.c runs this on the host. */
#define MASTER_LIMIT 12000
#define MASTER_CEILING 14000     /* the limiter's attack lets a few samples past MASTER_LIMIT */
#define MASTER_LIMIT_LOG_Q8 3469 /* log2(MASTER_LIMIT) */
#define MASTER_HEADROOM 100000   /* a stage's input stops here, so its largest gain cannot overflow 32 bits */
#define MASTER_DRIVE_OUT_MAX 500000   /* what the limiter's 32 bits can take */
#define MASTER_GAIN_UNITY_Q12 4096
#define MASTER_VOLUME_FULL_Q12 4096
#define MASTER_WIDTH_UNITY 64
#define MASTER_TILT_MAX 63       /* 1 octave of gain, 6 dB, each way */
#define MASTER_DRIVE_MAX 48      /* 12 units are 6 dB */
#define TILT_COEF_Q15 3110       /* a one-pole at 700 Hz, where both shelves meet */
#define COMP_THRESHOLD_OCTAVES 5 /* full COMP starts 30 dB under the limit */
#define COMP_KNEE_Q8 128         /* 3 dB each side of the threshold */
#define COMP_ATTACK_OCT_PER_UNIT 16    /* in 1/256 octaves: 8 octaves over the knob, 1 to 256 blocks: 0.7 to 186 ms */
#define COMP_RELEASE_OCT_PER_UNIT 12   /* 6 octaves over the knob, 40 to 2500 blocks: 29 ms to 1.8 s */
#define COMP_RELEASE_MIN_BLOCKS 40

typedef struct {
    int32_t env;                 /* the limiter's */
    int32_t tilt_l, tilt_r;      /* Q8 */
    int32_t reduction_q16;       /* the compressor's gain reduction, in octaves */
    int32_t gain_q12;            /* the compressor's gain, makeup included, at the end of the last block */
} master_t;
#define MASTER_INIT {.env = MASTER_LIMIT, .gain_q12 = MASTER_GAIN_UNITY_Q12}

typedef struct { int32_t comp, attack, release, tilt, width, drive, volume_q12; } master_params_t;
#define MASTER_NEUTRAL {0, 0, 0, 0, MASTER_WIDTH_UNITY, 0, MASTER_VOLUME_FULL_Q12}
#define MASTER_WORST {127, 0, 0, MASTER_TILT_MAX, 127, MASTER_DRIVE_MAX, MASTER_VOLUME_FULL_Q12}

/* v >= 1. Accurate to about 0.015 octave */
static AUDIO_INLINE int32_t log2_q8(uint32_t v)
{
    int32_t e = 0, m;
    while (v >> (e + 1))
        e++;
    m = (int32_t)(e >= 8 ? v >> (e - 8) : v << (8 - e)) - 256;
    return e * 256 + m + ((m * (256 - m) * 87) >> 16);
}

/* 2 to the power log, Q12. Accurate to about 0.3 % */
static AUDIO_INLINE int32_t exp2_q12(int32_t log_q8)
{
    int32_t octave = log_q8 >> 8, f = log_q8 & 255;
    int32_t y = 4096 + (f * (2689 + ((1407 * f) >> 8)) >> 8);
    return octave >= 0 ? y << octave : y >> -octave;
}

static AUDIO_INLINE int32_t comp_threshold_q8(int32_t comp) { return MASTER_LIMIT_LOG_Q8 - comp * COMP_THRESHOLD_OCTAVES * 256 / 127; }

/* Q8 fraction of the level over the threshold that the compressor removes: a ratio of 1.5:1 at low COMP and 4:1 at full */
static AUDIO_INLINE int32_t comp_slope_q8(int32_t comp) { return 85 + comp * 107 / 127; }

/* half of what a signal at the limit loses, so turning COMP up keeps the loudness about level */
static AUDIO_INLINE int32_t comp_makeup_q8(int32_t comp)
{
    return ((MASTER_LIMIT_LOG_Q8 - comp_threshold_q8(comp)) * comp_slope_q8(comp)) >> 9;
}

static AUDIO_INLINE int32_t comp_reduction_q8(int32_t level_q8, int32_t comp)
{
    int32_t over = level_q8 - comp_threshold_q8(comp);
    if (over <= -COMP_KNEE_Q8)
        return 0;
    if (over < COMP_KNEE_Q8)
        over = (over + COMP_KNEE_Q8) * (over + COMP_KNEE_Q8) / (4 * COMP_KNEE_Q8);
    return (over * comp_slope_q8(comp)) >> 8;
}

/* blocks to 63 % of a change as Q15 of the distance a block moves */
static AUDIO_INLINE int32_t comp_coef_q15(int32_t blocks_q12) { return (int32_t)((32767u << 12) / (uint32_t)blocks_q12); }

static AUDIO_INLINE void master_tilt(master_t *m, int32_t *l, int32_t *r, int32_t tilt)
{
    int32_t log = tilt * 256 / MASTER_TILT_MAX, low_gain = exp2_q12(log), high_gain = exp2_q12(-log), sl = m->tilt_l, sr = m->tilt_r;
    uint32_t i;
    for (i = 0; i < CTL; i++) {
        int32_t x = clamp(l[i], -MASTER_HEADROOM, MASTER_HEADROOM), y = clamp(r[i], -MASTER_HEADROOM, MASTER_HEADROOM), low;
        sl += ((x - (sl >> 8)) * TILT_COEF_Q15) >> 7;
        low = sl >> 8;
        l[i] = (low * low_gain + (x - low) * high_gain) >> 12;
        sr += ((y - (sr >> 8)) * TILT_COEF_Q15) >> 7;
        low = sr >> 8;
        r[i] = (low * low_gain + (y - low) * high_gain) >> 12;
    }
    m->tilt_l = sl;
    m->tilt_r = sr;
}

/* The gain moves once a block, on the peak of the block it applies to, so a hit is caught before it passes */
static AUDIO_INLINE void master_compress(master_t *m, int32_t *l, int32_t *r, const master_params_t *p)
{
    int32_t peak = 1, target, coef, gain = m->gain_q12, step;
    uint32_t i;
    for (i = 0; i < CTL; i++) {
        l[i] = clamp(l[i], -MASTER_HEADROOM, MASTER_HEADROOM);
        r[i] = clamp(r[i], -MASTER_HEADROOM, MASTER_HEADROOM);
        peak = l[i] > peak ? l[i] : -l[i] > peak ? -l[i] : peak;
        peak = r[i] > peak ? r[i] : -r[i] > peak ? -r[i] : peak;
    }
    target = p->comp ? comp_reduction_q8(log2_q8((uint32_t)peak), p->comp) << 8 : 0;
    coef = target > m->reduction_q16 ? comp_coef_q15(exp2_q12(clamp(p->attack, 0, 127) * COMP_ATTACK_OCT_PER_UNIT))
                                     : comp_coef_q15(exp2_q12(clamp(p->release, 0, 127) * COMP_RELEASE_OCT_PER_UNIT) * COMP_RELEASE_MIN_BLOCKS);
    m->reduction_q16 += ((target - m->reduction_q16) >> 8) * coef >> 7;
    if (!p->comp && m->reduction_q16 < 256)
        m->reduction_q16 = 0;
    target = exp2_q12(comp_makeup_q8(p->comp) - (m->reduction_q16 >> 8));
    step = (target - gain) / (int32_t)CTL;
    for (i = 0; i < CTL; i++) {
        gain += step;
        l[i] = (l[i] * gain) >> 12;
        r[i] = (r[i] * gain) >> 12;
    }
    m->gain_q12 = target;
}

static AUDIO_INLINE void master_width(int32_t *l, int32_t *r, int32_t width)
{
    uint32_t i;
    for (i = 0; i < CTL; i++) {
        int32_t mid = (l[i] + r[i]) >> 1, side = (((l[i] - r[i]) >> 1) * width) >> 6;
        l[i] = mid + side;
        r[i] = mid - side;
    }
}

/* Q8 of gain at this point: Q12 could not hold +24 dB on a sample at the headroom */
static AUDIO_INLINE void master_drive(int32_t *l, int32_t *r, int32_t drive)
{
    int32_t g = exp2_q12(drive * 64 / 3) >> 4;
    uint32_t i;
    for (i = 0; i < CTL; i++) {
        l[i] = clamp((clamp(l[i], -MASTER_HEADROOM, MASTER_HEADROOM) * g) >> 8, -MASTER_DRIVE_OUT_MAX, MASTER_DRIVE_OUT_MAX);
        r[i] = clamp((clamp(r[i], -MASTER_HEADROOM, MASTER_HEADROOM) * g) >> 8, -MASTER_DRIVE_OUT_MAX, MASTER_DRIVE_OUT_MAX);
    }
}

static AUDIO_INLINE void master_sample(master_t *m, int32_t *l, int32_t *r)
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

/* volume comes last, so turning it down does not change how hard the stages work */
AUDIO_RAM static void master_block(master_t *m, int32_t *l, int32_t *r, const master_params_t *p)
{
    uint32_t i;
    if (p->tilt)
        master_tilt(m, l, r, p->tilt);
    if (p->comp || m->gain_q12 != MASTER_GAIN_UNITY_Q12)
        master_compress(m, l, r, p);
    if (p->width != MASTER_WIDTH_UNITY)
        master_width(l, r, p->width);
    if (p->drive)
        master_drive(l, r, p->drive);
    for (i = 0; i < CTL; i++) {
        master_sample(m, &l[i], &r[i]);
        l[i] = (l[i] * p->volume_q12) >> 12;
        r[i] = (r[i] * p->volume_q12) >> 12;
    }
}

/* the limiter's gain reduction, Q8 octaves */
static int32_t master_limit_reduction_q8(const master_t *m)
{
    return m->env > MASTER_LIMIT ? log2_q8((uint32_t)m->env) - MASTER_LIMIT_LOG_Q8 : 0;
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
    return (uint32_t)clamp(e * 8 + (int32_t)((v >> (e - 3)) & 7u) - FULL_Q3 + METER_OCTAVES * 8, 0, METER_OCTAVES * 8) * w / (METER_OCTAVES * 8u);
}
