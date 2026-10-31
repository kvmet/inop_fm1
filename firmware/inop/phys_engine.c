/* SPDX-License-Identifier: GPL-3.0-only */
/* The physical modeling engine: one model per trigger, chosen by PV_E0. It plays drums and tuned bodies. The amp envelope
 * ends a voice: a model rings until then. */
#include "phys_dsp.c"
#include "phys_symp.c"

enum { PM_MODAL, PM_STRING, PM_MEMB, PM_SYMP, PM_COUNT };
#define PHYS_LEVEL_SHIFT 2       /* against the FM engine's level */
static const int32_t PHYS_GAIN[PM_COUNT] = {28000, 12000, 90000, 12000};   /* a strike of all modes in phase peaks high but short */
static const int16_t PHYS_CHORDS[8][PX_NSYMP] = {
    {-1200, 1200, 2400}, {700, 1200, 1900}, {500, 1200, 1700}, {400, 700, 1200},
    {300, 700, 1200}, {200, 700, 1400}, {400, 700, 1000}, {0, 700, 1200},
};
static const int8_t PHYS_DETUNE[PX_NSYMP] = {1, -1, 0};   /* 1/16 semitone: the strings beat a little */

typedef struct {
    uint8_t model;
    union {
        px_modal_t m;            /* MODAL, MEMB */
        px_string_t s;
        px_symp_t y;
    } u;
} phys_voice_t;

static uint32_t phys_model(const pkt_t *p) { return (uint32_t)clamp(p->v[PV_E0], 0, 127) % PM_COUNT; }

static void phys_start(phys_voice_t *S, const pkt_t *p, uint32_t seed)
{
    uint32_t i, *w = (uint32_t *)&S->u;
    for (i = 0; i < sizeof S->u / 4u; i++)
        w[i] = 0;
    S->model = (uint8_t)phys_model(p);
    if (S->model == PM_STRING || S->model == PM_SYMP) {
        S->u.s.rng = seed | 1u;
        S->u.s.src = 1u << 30;
        S->u.s.trig = 1;
    } else {
        S->u.m.rng = seed | 1u;
        S->u.m.trig = 1;
    }
}

/* adds one block to out. pitch: an offset in 1/16 semitones */
static void phys_render(phys_voice_t *S, const pkt_t *p, int32_t *out, int32_t pitch)
{
    int32_t e1 = clamp(p->v[PV_E1], 0, 127), e2 = clamp(p->v[PV_E2], 0, 127), e3 = clamp(p->v[PV_E3], 0, 127);
    int32_t e4 = clamp(p->v[PV_E4], 0, 127), e5 = clamp(p->v[PV_E5], 0, 127), e6 = clamp(p->v[PV_E6], 0, 127);
    int32_t e7 = clamp(p->v[PV_E7], 0, 127);
    int32_t p16 = clamp(p->v[PV_NOTE] * 16 + pitch, 0, 2047);
    int32_t strc = e1 * 516, brit = e2 * 516, damp = e3 * 516, exc = e7 * 258, bow = e6 * 206;
    int32_t acc = clamp(e5 * clamp(p->v[PV_VEL], 0, 127) * 4, 0, 65536);
    int32_t y[CTL], ax[CTL], g = PHYS_GAIN[S->model];
    uint32_t i, k, f0 = pitch_inc((uint32_t)p16);
    if (S->model == PM_MODAL) {
        px_modal_blk_t B;
        px_modal_block(&B, &S->u.m, f0, strc, brit, damp, acc, e4 * 129, bow);
        px_modal_run(&B, &S->u.m, y, ax, CTL);
    } else if (S->model == PM_MEMB) {
        px_modal_blk_t B;
        px_memb_block(&B, &S->u.m, f0, strc, brit, damp, acc, e4 * 516, e6 * 6192);
        px_modal_run(&B, &S->u.m, y, ax, CTL);
    } else if (S->model == PM_STRING) {
        px_string_blk_t B;
        px_string_block(&B, &S->u.s, f0, strc, brit, damp, acc, bow, e4 * 516);
        px_string_excite(&B, &S->u.s, ax, CTL);
        px_string_run(&B, &S->u.s, ax, y, CTL);
    } else {
        px_string_blk_t B;
        uint32_t f[PX_NSYMP];
        for (k = 0; k < PX_NSYMP; k++)
            f[k] = pitch_inc((uint32_t)clamp(p16 + PHYS_DETUNE[k] + ((PHYS_CHORDS[e1 >> 4][k] * 41) >> 8), 0, 2047));
        px_string_block(&B, &S->u.y.s, f0, 16384 - e6 * 129, brit, damp, acc, 0, 0);
        px_string_excite(&B, &S->u.y.s, ax, CTL);
        px_string_run(&B, &S->u.y.s, ax, y, CTL);
        px_symp_block(&S->u.y, f, brit, damp, e4 * 516);
        px_symp_run(&S->u.y, y, y, CTL);
    }
    for (i = 0; i < CTL; i++) {
        int32_t s = px_m(y[i] + px_m(ax[i], exc, 15), g, 20), a = s < 0 ? -s : s;
        if (a > 24000) {                         /* soft knee: only peaks saturate */
            a = 24000 + (softclip(clamp(a - 24000, 0, 1 << 20) * 2) >> 1);
            s = s < 0 ? -a : a;
        }
        out[i] += s >> PHYS_LEVEL_SHIFT;
    }
}
