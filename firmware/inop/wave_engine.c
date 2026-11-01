/* SPDX-License-Identifier: GPL-3.0-only */
/* The WAVE engine: two oscillators that read one wavetable, with a bent phase, ring or FM between them, and noise.
 * No hardware here: tests/inop_test.c runs this on the host. */
#define WAVE_PEAK 8000           /* oscillator 1 alone at the output; oscillator 2 and the noise add to it */
#define WAVE_FM_SHIFT 18         /* a full-scale modulator moves phase by 2 cycles */
#define WAVE_CROSS_MAX 63
#define WAVE_PITCH2_MAX 24
#define WAVE_WARP_KNEE_MIN 0x08000000u   /* 1/32 of a cycle: where the first half of the wave ends at full WARP */

typedef struct {
    uint32_t phase[2];
    int32_t position;            /* Q16 frames: where the last block's ramp ended */
    uint32_t knee, slope_a, slope_b;   /* the bent phase. slope: Q16. knee 0: no bend */
    uint32_t rng;
} wave_voice_t;

static AUDIO_INLINE uint32_t wave_table(int32_t v) { return (uint32_t)clamp(v, 0, WAVE_TABLES - 1); }

static AUDIO_INLINE int32_t wave_position(int32_t wave) { return clamp(wave, 0, 127) * ((WAVE_FRAMES - 1u) << 16) / 127; }

/* 0 is the largest size. Each smaller size has half the harmonics, and the top one must stay under Nyquist */
static AUDIO_INLINE uint32_t wave_size_index(uint32_t inc)
{
    uint32_t m = 0;
    uint64_t top = (uint64_t)inc << (WAVE_TOP_BITS - 2u);
    while (m + 1u < WAVE_SIZES && top >= 0x80000000u) {
        top >>= 1;
        m++;
    }
    return m;
}

static AUDIO_INLINE const int16_t *wave_size(const int16_t *frame, uint32_t m)
{
    return frame + ((1u << (WAVE_TOP_BITS + 1u)) - (1u << (WAVE_TOP_BITS + 1u - m)));
}

/* a sample at a phase, one frame */
static inline __attribute__((always_inline)) int32_t wave_read(const int16_t *a, uint32_t bits, uint32_t ph)
{
    uint32_t i = ph >> (32u - bits), j = (i + 1u) & ((1u << bits) - 1u);
    return a[i] + (((a[j] - a[i]) * (int32_t)((ph << bits) >> 17)) >> 15);
}

/* a sample between two frames. mix: Q15, how far toward b */
static inline __attribute__((always_inline)) int32_t wave_blend(const int16_t *a, const int16_t *b, uint32_t bits, uint32_t ph, int32_t mix)
{
    int32_t x = wave_read(a, bits, ph);
    return x + (((wave_read(b, bits, ph) - x) * mix) >> 15);
}

/* WARP moves the wave's middle toward its start, so the first half is squeezed and the second is stretched */
static inline __attribute__((always_inline)) uint32_t wave_bend(const wave_voice_t *W, uint32_t ph)
{
    if (ph < W->knee)
        return (uint32_t)(((uint64_t)ph * W->slope_a) >> 16);
    return 0x80000000u + (uint32_t)(((uint64_t)(ph - W->knee) * W->slope_b) >> 16);
}

static void wave_set_bend(wave_voice_t *W, int32_t warp)
{
    uint32_t w = (uint32_t)clamp(warp, 0, 127);
    W->knee = 0;
    if (!w)
        return;
    W->knee = 0x80000000u - w * ((0x80000000u - WAVE_WARP_KNEE_MIN) / 127u);
    W->slope_a = (uint32_t)((0x80000000ull << 16) / W->knee);
    W->slope_b = (uint32_t)((0x80000000ull << 16) / (0u - W->knee));
}

static void wave_start(wave_voice_t *W, const pkt_t *p, uint32_t seed)
{
    W->phase[0] = W->phase[1] = 0;
    W->position = wave_position(p->v[PV_E1]);
    W->rng = seed | 1u;
    wave_set_bend(W, p->v[PV_E2]);
}

/* adds one block to out, or makes left in out and right in right, which hold only this voice. pitch: an offset in 1/16 semitones. timbre: an offset to WAVE */
static AUDIO_INLINE void wave_render(wave_voice_t *W, const pkt_t *p, int32_t *out, int32_t *right, int32_t pitch, int32_t timbre)
{
    const int16_t (*frames)[sizeof WAVE_DATA[0][0] / sizeof(int16_t)] = WAVE_DATA[wave_table(p->v[PV_E0])];
    int32_t semis = clamp(p->v[PV_E3], -WAVE_PITCH2_MAX, WAVE_PITCH2_MAX), cross = clamp(p->v[PV_E4], -WAVE_CROSS_MAX, WAVE_CROSS_MAX);
    int32_t level2 = fm_squared(p->v[PV_E5], 32767), noise = fm_squared(p->v[PV_E7], 16384), detune = clamp(p->v[PV_E6], 0, 127);
    int32_t ring = cross < 0 ? -cross * 32767 / WAVE_CROSS_MAX : 0, depth = cross > 0 ? cross * cross * 32767 / (WAVE_CROSS_MAX * WAVE_CROSS_MAX) : 0;
    int32_t position = W->position, to = wave_position(p->v[PV_E1] + timbre), step;
    uint32_t base = note_inc(p->v[PV_NOTE] * 16 + pitch), s, r = W->rng;
    uint32_t inc2 = (uint32_t)(((uint64_t)note_inc(p->v[PV_NOTE] * 16 + pitch + semis * 16) * (65536u + (uint32_t)(detune * detune) * 3866u / 16129u)) >> 16);
    uint32_t size1 = wave_size_index(base), size2 = wave_size_index(inc2);
    uint32_t bits1 = WAVE_TOP_BITS - size1, bits2 = WAVE_TOP_BITS - size2, ph1 = W->phase[0], ph2 = W->phase[1];
    int32_t noise_each = right ? (noise * STEREO_SOURCE_Q15) >> 15 : noise, both[CTL];
    int use2 = level2 || ring || depth;
    const int16_t *frame_a, *frame_b, *frame2;
    uint32_t f, g;
    f = (uint32_t)(to < position ? position - 1 : position) >> 16;
    g = f + 1u < WAVE_FRAMES ? f + 1u : f;
    to = clamp(to, (int32_t)(f << 16), (int32_t)(g << 16));       /* a block stays between two frames. A jump takes more blocks */
    step = (to - position) / (int32_t)CTL;
    frame_a = wave_size(frames[f], size1);
    frame_b = wave_size(frames[g], size1);
    frame2 = wave_size(frames[position - (int32_t)(f << 16) < 0x8000 ? f : g], size2);   /* the nearest: 2 table reads a sample, where a blend takes 4 */
    for (s = 0; s < CTL; s++) {
        uint32_t at = ph1;
        int32_t y, y2 = 0, y_right = 0;
        if (use2) {
            y2 = wave_read(frame2, bits2, W->knee ? wave_bend(W, ph2) : ph2);
            at += (uint32_t)((y2 * depth) >> 15) * (1u << WAVE_FM_SHIFT);
        }
        y = wave_blend(frame_a, frame_b, bits1, W->knee ? wave_bend(W, at) : at, (position - (int32_t)(f << 16)) >> 1);
        if (ring)
            y += (((y * y2) >> 15) - y) * ring >> 15;
        if (right) {
            int32_t parted = (y * level2) >> 15;         /* osc 1 follows OSC2 out of the center, so a quiet osc 2 does not leave it alone on one side */
            both[s] = ((y - parted) * WAVE_PEAK) >> 15;
            y = parted;
            y_right = (y2 * level2) >> 15;
        } else {
            y += (y2 * level2) >> 15;
        }
        if (noise) {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            y += (((int32_t)r >> 16) * noise_each) >> 15;
            if (right) {
                r ^= r << 13;
                r ^= r >> 17;
                r ^= r << 5;
                y_right += (((int32_t)r >> 16) * noise_each) >> 15;
            }
        }
        out[s] += (y * WAVE_PEAK) >> 15;
        if (right)
            right[s] += (y_right * WAVE_PEAK) >> 15;
        ph1 += base;
        ph2 += inc2;
        position += step;
    }
    W->phase[0] = ph1;
    W->phase[1] = ph2;
    W->position = to;
    W->rng = r;
    if (right)
        stereo_mix(out, right, both, p->v[PV_STEREO]);
}
