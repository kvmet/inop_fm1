/* SPDX-License-Identifier: GPL-3.0-only */
/* The shared effects that voices reach by sends. No hardware here: tests/inop_test.c runs this on the host. */
#ifndef POOL_SECTION
#define POOL_SECTION
#endif
#define DELAY_LEN 32768u         /* 0.74 s; a power of 2: masked */
#define DELAY_MASK (DELAY_LEN - 1u)
#define DELAY_DAMP 6000          /* of 32768: the repeats lose their top */
#define DELAY_FB_MAX 31000       /* below unity at full feedback, so repeats always decay */
#define REV_COMBS 4u
#define REV_APS 2u
#define REV_SPREAD 23u           /* the right allpasses' extra length, for width */

static const uint16_t REV_COMB_LEN[REV_COMBS] = {1116, 1188, 1277, 1356};
static const uint16_t REV_AP_LEN[REV_APS] = {225, 556};

typedef struct {
    int16_t delay[DELAY_LEN];
    uint32_t delay_w, delay_cur;
    int32_t delay_lp;
} delay_t;

typedef struct {
    int16_t comb[REV_COMBS][1356];
    int16_t ap[2][REV_APS][556 + REV_SPREAD];
    uint16_t comb_i[REV_COMBS], ap_i[2][REV_APS];
    int32_t comb_lp[REV_COMBS];
} reverb_t;

static delay_t delay_fx;
static reverb_t reverb_fx POOL_SECTION;

/* adds the repeats to l and r. samples: the delay time; it glides there, so a change bends pitch and does not click */
static void delay_block(delay_t *d, const int32_t *in, int32_t *l, int32_t *r, uint32_t samples, int32_t feedback)
{
    int32_t fb = clamp(feedback, 0, 127) * DELAY_FB_MAX / 127;
    uint32_t i;
    samples = samples >= DELAY_LEN ? DELAY_LEN - 1u : samples < 16u ? 16u : samples;
    if (d->delay_cur < 16u)
        d->delay_cur = samples;
    for (i = 0; i < CTL; i++) {
        int32_t x = d->delay[(d->delay_w - d->delay_cur) & DELAY_MASK];
        d->delay_lp += ((x - d->delay_lp) * (32767 - DELAY_DAMP)) >> 15;
        d->delay[d->delay_w & DELAY_MASK] = (int16_t)clamp(in[i] + ((d->delay_lp * fb) >> 15), -32768, 32767);
        d->delay_w++;
        l[i] += x;
        r[i] += x;
        if (d->delay_cur != samples)
            d->delay_cur += d->delay_cur < samples ? 1u : (uint32_t)-1;
    }
}

static int32_t reverb_ap(reverb_t *v, uint32_t side, int32_t a)
{
    uint32_t k;
    for (k = 0; k < REV_APS; k++) {
        uint16_t *pos = &v->ap_i[side][k];
        uint32_t len = REV_AP_LEN[k] + side * REV_SPREAD;
        int32_t o = v->ap[side][k][*pos];
        v->ap[side][k][*pos] = (int16_t)clamp(a + (o >> 1), -32768, 32767);
        a = o - a;
        if (++*pos >= len)
            *pos = 0;
    }
    return a;
}

/* 4 damped combs into 2 allpasses per side. The combs are shared and the sides sum them with different signs */
static void reverb_block(reverb_t *v, const int32_t *in, int32_t *l, int32_t *r, int32_t size, int32_t damp)
{
    int32_t fb = 25000 + clamp(size, 0, 127) * 50, loss = clamp(damp, 0, 127) * 200;
    uint32_t i, k;
    for (i = 0; i < CTL; i++) {
        int32_t c[REV_COMBS], wet = (in[i] * 2580) >> 15;
        for (k = 0; k < REV_COMBS; k++) {
            uint16_t pos = v->comb_i[k];
            int32_t o = v->comb[k][pos];
            v->comb_lp[k] = o + (((v->comb_lp[k] - o) * loss) >> 15);
            v->comb[k][pos] = (int16_t)clamp(wet + ((v->comb_lp[k] * fb) >> 15), -32768, 32767);
            v->comb_i[k] = (uint16_t)(pos + 1u >= REV_COMB_LEN[k] ? 0u : pos + 1u);
            c[k] = o;
        }
        l[i] += reverb_ap(v, 0, c[0] + c[1] + c[2] + c[3]);
        r[i] += reverb_ap(v, 1, c[0] - c[1] + c[2] - c[3]);
    }
}
