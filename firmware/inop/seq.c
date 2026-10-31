/* SPDX-License-Identifier: GPL-3.0-only */
/* The sequencer: it never stops. Tracks wrap on their own lengths. A committed pattern starts when the playing pattern's
 * longest track completes, and only then does every track start from step 1. No hardware here: tests/inop_test.c runs this on the host. */
#define SEQ_PLAYS 64u
#define SEQ_NOT_FIRED 0xFFu

typedef struct {
    uint32_t sample_acc, samples_per_step;
    uint32_t count;              /* steps since the playing pattern started */
    uint8_t playing, next;       /* pattern indices; equal when no switch waits */
    uint8_t plays;               /* completed plays of the playing pattern, modulo SEQ_PLAYS */
    uint8_t pos[NTRK];
    uint8_t fired[NTRK];         /* the step of each track's last trigger, or SEQ_NOT_FIRED */
    uint8_t skip[NTRK];          /* step + 1: this step already sounded when it was played in, so its next trigger is dropped */
    uint32_t rng;
} seq_t;

/* fires one trigger; the caller owns the voices */
typedef void (*seq_emit_t)(uint32_t track, uint32_t step, const track_pat_t *t);

static void seq_set_bpm(seq_t *q, uint32_t bpm) { q->samples_per_step = FS * 15u / bpm; }

static void seq_init(seq_t *q, uint32_t bpm)
{
    uint32_t i;
    q->sample_acc = 0;
    q->count = 0;
    q->plays = 0;
    q->playing = q->next = 0;
    for (i = 0; i < NTRK; i++) {
        q->pos[i] = 0;
        q->skip[i] = 0;
        q->fired[i] = SEQ_NOT_FIRED;
    }
    q->rng = 0x13579BDFu;
    seq_set_bpm(q, bpm);
}

static uint32_t pattern_longest(const pattern_t *p)
{
    uint32_t i, n = 1;
    for (i = 0; i < NTRK; i++)
        if (p->track[i].length > n)
            n = p->track[i].length;
    return n;
}

static uint32_t seq_rand100(seq_t *q)
{
    q->rng ^= q->rng << 13;
    q->rng ^= q->rng >> 17;
    q->rng ^= q->rng << 5;
    return q->rng % 100u;
}

static void seq_fire(seq_t *q, const pattern_t *p, seq_emit_t emit)
{
    uint32_t k;
    for (k = 0; k < NTRK; k++) {
        const track_pat_t *t = &p->track[k];
        const step_t *s = &t->steps[q->pos[k]];
        if (q->skip[k] == q->pos[k] + 1u) {
            q->skip[k] = 0;
            continue;
        }
        if (s->on && seq_rand100(q) < s->chance) {
            q->fired[k] = q->pos[k];
            emit(k, q->pos[k], t);
        }
    }
}

static void seq_advance(seq_t *q, const pattern_t *patterns, seq_emit_t emit)
{
    const pattern_t *p = &patterns[q->playing];
    uint32_t k;
    q->count++;
    if (q->count >= pattern_longest(p)) {
        q->count = 0;
        q->plays = (uint8_t)((q->plays + 1u) % SEQ_PLAYS);
    }
    if (q->count == 0u && q->next != q->playing) {
        q->playing = q->next;
        q->plays = 0;
        p = &patterns[q->playing];
        for (k = 0; k < NTRK; k++) {
            q->pos[k] = 0;
            q->skip[k] = 0;
            q->fired[k] = SEQ_NOT_FIRED;
        }
    } else {
        for (k = 0; k < NTRK; k++)
            q->pos[k] = (uint8_t)((q->pos[k] + 1u) % p->track[k].length);
    }
    seq_fire(q, p, emit);
}

/* the track's step nearest to now */
static uint32_t seq_nearest_step(const seq_t *q, uint32_t track, uint32_t length)
{
    return q->sample_acc * 2u < q->samples_per_step ? q->pos[track] : (q->pos[track] + 1u) % length;
}

/* call once for each render block */
static void seq_block(seq_t *q, const pattern_t *patterns, uint32_t samples, seq_emit_t emit)
{
    q->sample_acc += samples;
    while (q->sample_acc >= q->samples_per_step) {
        q->sample_acc -= q->samples_per_step;
        seq_advance(q, patterns, emit);
    }
}
