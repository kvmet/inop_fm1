/* SPDX-License-Identifier: GPL-3.0-only */
/* The shared effects that voices reach by sends. No hardware here: tests/inop_test.c runs this on the host. */
#ifndef POOL_SECTION
#define POOL_SECTION
#endif
#define DELAY_LEN 44100u
#define DELAY_HOP_MAX ((DELAY_LEN - 1u) / 2u)   /* 0.5 s, 4 steps at 120 BPM: a hop is half of the line */
#define DELAY_DAMP 6000          /* of 32768: the repeats lose their top */
#define DELAY_FB_MAX 31000       /* below unity at full feedback, so repeats always decay */
#define REV_BUF_LEN 4096u        /* a power of 2: masked. Holds every line of the loop, at 22 kHz */
#define REV_MASK (REV_BUF_LEN - 1u)
#define REV_INPUT_Q15 22000

typedef struct {
    int16_t delay[DELAY_LEN];
    uint32_t delay_w, delay_cur;
    int32_t delay_lp, delay_lp_loop;
} delay_t;

/* the delay lines share one circular buffer and one moving position, so a read is an add and a mask, with no wrap to check */
typedef struct {
    int16_t buf[REV_BUF_LEN];
    uint32_t w;
    int32_t feedback, lp[2], last_l, last_r;
} reverb_t;

static delay_t delay_fx POOL_SECTION;
static reverb_t reverb_fx POOL_SECTION;

/* back <= DELAY_HOP_MAX * 2, so one wrap is enough */
static AUDIO_INLINE int32_t delay_read(const delay_t *d, uint32_t back)
{
    return d->delay[d->delay_w >= back ? d->delay_w - back : d->delay_w + DELAY_LEN - back];
}

/* adds the repeats to l and r. samples: the time between repeats; it glides there, so a change bends pitch and does not click.
 * Each loop around the line is two repeats: the odd one to the left, read one hop back, and the even one to the right, read two hops back.
 * width: 0 sends both to both channels. Feedback and damping run once per repeat at any width */
AUDIO_RAM static void delay_block(delay_t *d, const int32_t *in, int32_t *l, int32_t *r, uint32_t samples, int32_t feedback, int32_t width)
{
    int32_t fb = clamp(feedback, 0, 127) * DELAY_FB_MAX / 127, near, far;
    uint32_t i;
    samples = samples > DELAY_HOP_MAX ? DELAY_HOP_MAX : samples < 16u ? 16u : samples;
    if (d->delay_cur < 16u)
        d->delay_cur = samples;
    stereo_gains(width, &near, &far);
    for (i = 0; i < CTL; i++) {
        int32_t odd = delay_read(d, d->delay_cur), even = delay_read(d, 2u * d->delay_cur);
        d->delay_lp += ((even - d->delay_lp) * (32767 - DELAY_DAMP)) >> 15;
        even = (d->delay_lp * fb) >> 15;
        d->delay_lp_loop += ((even - d->delay_lp_loop) * (32767 - DELAY_DAMP)) >> 15;
        d->delay[d->delay_w] = (int16_t)clamp(in[i] + ((d->delay_lp_loop * fb) >> 15), -32768, 32767);
        d->delay_w = d->delay_w + 1u < DELAY_LEN ? d->delay_w + 1u : 0u;
        l[i] += (odd * near + even * far) >> 12;
        r[i] += (even * near + odd * far) >> 12;
        if (d->delay_cur != samples)
            d->delay_cur += d->delay_cur < samples ? 1u : (uint32_t)-1;
    }
}

/* a delay line that starts at base and holds len samples: at is the moving position */
#define REV_READ(v, at, base, len) ((v)->buf[((at) + (base) + (len)) & REV_MASK])
#define REV_WRITE(v, at, base, x) ((v)->buf[((at) + (base)) & REV_MASK] = (int16_t)clamp(x, -32768, 32767))

/* a true allpass at coefficient 1/2 */
static AUDIO_INLINE int32_t reverb_ap(reverb_t *v, uint32_t at, uint32_t base, uint32_t len, int32_t a)
{
    int32_t o = REV_READ(v, at, base, len), x = a + (o >> 1);
    REV_WRITE(v, at, base, x);
    return o - (x >> 1);
}

/* each line's base. The lengths share no factor, so the loop's echoes do not line up: 140 ms around */
enum {
    REV_D1 = 67, REV_D2 = 97,
    REV_A1 = 197, REV_L1 = 463, REV_A2 = 271, REV_L2 = 557, REV_A3 = 233, REV_L3 = 419, REV_A4 = 313, REV_L4 = 631,
    REV_D1_AT = 0,
    REV_D2_AT = REV_D1_AT + REV_D1 + 1,
    REV_A1_AT = REV_D2_AT + REV_D2 + 1,
    REV_L1_AT = REV_A1_AT + REV_A1 + 1,
    REV_A2_AT = REV_L1_AT + REV_L1 + 1,
    REV_L2_AT = REV_A2_AT + REV_A2 + 1,
    REV_A3_AT = REV_L2_AT + REV_L2 + 1,
    REV_L3_AT = REV_A3_AT + REV_A3 + 1,
    REV_A4_AT = REV_L3_AT + REV_L3 + 1,
    REV_L4_AT = REV_A4_AT + REV_A4 + 1,
    REV_END = REV_L4_AT + REV_L4 + 1,
};
_Static_assert(REV_END <= REV_BUF_LEN, "the lines must not overlap");

/* one loop of four allpass and delay stages, at half the sample rate. Its allpasses take no multiply, and the two sides tap it at different points */
AUDIO_RAM static void reverb_block(reverb_t *v, const int32_t *in, int32_t *l, int32_t *r, int32_t size, int32_t damp)
{
    int32_t fb = 8500 + clamp(size, 0, 127) * 145, loss = clamp(damp, 0, 127) * 200;
    int32_t last_l = v->last_l, last_r = v->last_r, feedback = v->feedback, lp0 = v->lp[0], lp1 = v->lp[1];
    uint32_t at = v->w, i;
    for (i = 0; i < CTL; i += 2u) {
        int32_t a = (((in[i] + in[i + 1u]) >> 1) * REV_INPUT_Q15) >> 15, out_l, out_r, tap;
        at--;
        a = reverb_ap(v, at, REV_D1_AT, REV_D1, a);
        a = reverb_ap(v, at, REV_D2_AT, REV_D2, a);
        a = reverb_ap(v, at, REV_A1_AT, REV_A1, a + feedback);
        REV_WRITE(v, at, REV_L1_AT, a);
        tap = REV_READ(v, at, REV_L1_AT, REV_L1);
        lp0 = tap + (((lp0 - tap) * loss) >> 15);
        out_l = tap;
        a = reverb_ap(v, at, REV_A2_AT, REV_A2, lp0);
        REV_WRITE(v, at, REV_L2_AT, a);
        tap = REV_READ(v, at, REV_L2_AT, REV_L2);
        out_r = tap;
        a = reverb_ap(v, at, REV_A3_AT, REV_A3, tap);
        REV_WRITE(v, at, REV_L3_AT, a);
        tap = REV_READ(v, at, REV_L3_AT, REV_L3);
        lp1 = tap + (((lp1 - tap) * loss) >> 15);
        out_l -= tap;
        a = reverb_ap(v, at, REV_A4_AT, REV_A4, lp1);
        REV_WRITE(v, at, REV_L4_AT, a);
        tap = REV_READ(v, at, REV_L4_AT, REV_L4);
        out_r -= tap;
        feedback = (tap * fb) >> 15;
        l[i] += (last_l + out_l) >> 1;
        r[i] += (last_r + out_r) >> 1;
        l[i + 1u] += out_l;
        r[i + 1u] += out_r;
        last_l = out_l;
        last_r = out_r;
    }
    v->w = at;
    v->feedback = feedback;
    v->lp[0] = lp0;
    v->lp[1] = lp1;
    v->last_l = last_l;
    v->last_r = last_r;
}
