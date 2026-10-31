/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM engine: one DX-style patch per trigger. PV_E0 picks the factory patch. */
#include "fm6_core.c"
#include "felucca_fm6.h"

typedef struct {
    fm6_note_t note;
    fm6_lfo_t lfo;
    uint8_t patch[FP_SIZE + 1u];
} fm_voice_t;

/* a 128-byte packed record to the 155-byte voice */
static void fm_unpack(const uint8_t *b, uint8_t *v)
{
    uint32_t k, i;
    for (k = 0; k < 6u; k++) {
        const uint8_t *o = b + k * 17u;
        uint8_t *d = v + k * FP_OP;
        for (i = 0; i < 11u; i++)
            d[i] = o[i] & 0x7Fu;
        d[FP_LC] = o[11] & 3u;
        d[FP_RC] = (o[11] >> 2) & 3u;
        d[FP_RS] = o[12] & 7u;
        d[FP_DET] = (o[12] >> 3) & 15u;
        d[FP_AMS] = o[13] & 3u;
        d[FP_KVS] = (o[13] >> 2) & 7u;
        d[FP_OL] = o[14] & 0x7Fu;
        d[FP_MODE] = o[15] & 1u;
        d[FP_FC] = (o[15] >> 1) & 31u;
        d[FP_FF] = o[16] & 0x7Fu;
    }
    for (i = 0; i < 9u; i++)
        v[FP_PR1 + i] = b[102 + i] & 0x7Fu;
    v[FP_ALG] &= 31u;
    v[FP_FB] = b[111] & 7u;
    v[FP_OKS] = (b[111] >> 3) & 1u;
    for (i = 0; i < 4u; i++)
        v[FP_LFS + i] = b[112 + i] & 0x7Fu;
    v[FP_LKS] = b[116] & 1u;
    v[FP_LFW] = (b[116] >> 1) & 7u;
    v[FP_LPMS] = (b[116] >> 4) & 7u;
    v[FP_TRNSP] = b[117] & 0x7Fu;
    for (i = 0; i < 10u; i++)
        v[FP_NAME + i] = b[118 + i] & 0x7Fu;
}

static uint32_t fm_patch_alg(int32_t patch) { return FM6_FACTORY[(uint32_t)patch % FM6_NFACTORY][110] & 31u; }

/* name: 11 bytes */
static void fm_patch_name(int32_t patch, char *name)
{
    uint32_t i;
    for (i = 0; i < 10u; i++)
        name[i] = (char)(FM6_FACTORY[(uint32_t)patch % FM6_NFACTORY][118u + i] & 0x7Fu);
    name[10] = 0;
}

static void fm_start(fm_voice_t *f, const pkt_t *p)
{
    fm_unpack(FM6_FACTORY[(uint32_t)p->v[PV_E0] % FM6_NFACTORY], f->patch);
    fm6_lfo_reset(&f->lfo, f->patch);
    fm6_note_init(&f->note, f->patch, p->v[PV_NOTE] + f->patch[FP_TRNSP] - 24, p->v[PV_VEL], 1);
    fm6_lfo_key(&f->lfo);
}

static void fm_release(fm_voice_t *f) { fm6_note_key(&f->note, f->patch, 0); }

/* adds one block to out; 0 when every carrier has ended. pitch: an offset in 1/16 semitones */
static int fm_render(fm_voice_t *f, const pkt_t *p, int32_t *out, int32_t pitch)
{
    static const int32_t NO_DETUNE[6];
    int32_t bus[FM6_N], lfo = fm6_lfo_sample(&f->lfo), delay = fm6_lfo_delay(&f->lfo), lf, k;
    uint32_t i, alg = f->patch[FP_ALG] & 31u;
    if (f->note.down == 0u && fm6_note_done(&f->note, f->patch, alg))
        return 0;
    lf = fm6_note_logfreq((p->v[PV_NOTE] + f->patch[FP_TRNSP] - 24) * 16 + pitch, 0);
    if (!fm6_note_compute(&f->note, f->patch, bus, lfo, delay, lf, alg, f->patch[FP_FB], NO_DETUNE, 0))
        return 1;
    k = VOICE_FS;
    for (i = 0; i < FM6_N; i++) {
        int32_t x = bus[i] - f->note.dc;
        f->note.dc += x >> 10;                          /* a one-pole high-pass, ~7 Hz: FM puts sidebands at 0 Hz */
        out[i] += (int32_t)(((int64_t)x * k) >> 26);
    }
    return 1;
}
