/* SPDX-License-Identifier: GPL-3.0-only */
/* The sequencer: it runs until a player stops it. Tracks wrap on their own lengths. A committed pattern starts when the playing pattern's
 * longest track completes, and only then does every track start from step 1. No hardware here: tests/inop_test.c runs this on the host. */
#define SEQ_PLAYS 64u
#define SEQ_NOT_FIRED 0xFFu
enum { SEQ_RUNNING, SEQ_STOPPED, SEQ_STARTING };

typedef struct {
    uint32_t sample_acc, samples_per_step;
    uint32_t count;              /* steps since the playing pattern started */
    uint32_t total;              /* steps since the sequencer started */
    uint8_t playing, next;       /* pattern indices; equal when no switch waits */
    uint8_t plays;               /* completed plays of the playing pattern, modulo SEQ_PLAYS */
    uint8_t run;
    uint8_t pos[NTRK];
    uint8_t fired[NTRK];         /* the step of each track's last trigger, or SEQ_NOT_FIRED */
    uint8_t skip[NTRK];          /* step + 1: this step already sounded when it was played in, so its next trigger is dropped */
    uint32_t rng;
    uint8_t looked;              /* this step's look ahead has run */
    int16_t drift[NTRK];         /* where each track's human offset has wandered to, in +-SEQ_DRIFT_MAX */
    struct { const track_pat_t *t; uint32_t wait; uint8_t step; } waiting[NTRK];             /* a step's first hit, off the grid. t 0: none */
    struct { const track_pat_t *t; uint32_t wait, spacing; uint8_t step, hits, left; } ratchet[NTRK];   /* a ratchet's other hits */
    struct { const track_pat_t *t; int32_t offset; uint8_t step, rolled; } plan[NTRK];       /* the look ahead's result for the next step. t 0: none */
} seq_t;

/* fires one trigger; the caller owns the voices. Returns the steps the note spans, which a ratchet's hits divide */
typedef uint32_t (*seq_emit_t)(uint32_t track, uint32_t step, const track_pat_t *t, uint32_t hits);

static void seq_set_bpm(seq_t *q, uint32_t bpm) { q->samples_per_step = FS * 15u / bpm; }

static void seq_rewind(seq_t *q)
{
    uint32_t i;
    q->sample_acc = 0;
    q->count = 0;
    q->total = 0;
    q->plays = 0;
    for (i = 0; i < NTRK; i++) {
        q->pos[i] = 0;
        q->skip[i] = 0;
        q->fired[i] = SEQ_NOT_FIRED;
        q->waiting[i].t = 0;
        q->ratchet[i].left = 0;
        q->plan[i].t = 0;
    }
    q->looked = 0;
}

static void seq_init(seq_t *q, uint32_t bpm)
{
    seq_rewind(q);
    q->run = SEQ_RUNNING;
    q->playing = q->next = 0;
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

/* each trigger's offset keeps 3/4 of the last, so the timing wanders where independent offsets would only sound loose */
#define SEQ_DRIFT_STEP 256
#define SEQ_DRIFT_MAX (SEQ_DRIFT_STEP * 4)

/* samples ahead (negative) or behind the grid. Never past half a step, so hits keep their order */
static int32_t seq_offset(seq_t *q, uint32_t k, const track_pat_t *t, uint32_t step)
{
    int32_t spb = (int32_t)q->samples_per_step, half = spb / 2;
    int32_t units = t->steps[step].time + (step & 1u ? t->swing : 0);
    int32_t offset = units * spb / SEQ_TIME_UNITS;
    if (t->human) {
        q->drift[k] = (int16_t)(q->drift[k] * 3 / 4 + (int32_t)(seq_rand100(q) * (2 * SEQ_DRIFT_STEP + 1) / 100) - SEQ_DRIFT_STEP);
        offset += t->human * q->drift[k] * spb / (SEQ_TIME_UNITS * SEQ_DRIFT_STEP);
    }
    return clamp(offset, -half, half);
}

static void seq_first_hit(seq_t *q, uint32_t k, const track_pat_t *t, uint32_t step, seq_emit_t emit)
{
    uint32_t hits = t->steps[step].hits, span;
    q->fired[k] = (uint8_t)step;
    q->ratchet[k].left = 0;
    span = emit(k, step, t, hits);
    if (hits > 1u) {
        q->ratchet[k].t = t;
        q->ratchet[k].step = (uint8_t)step;
        q->ratchet[k].hits = (uint8_t)hits;
        q->ratchet[k].left = (uint8_t)(hits - 1u);
        q->ratchet[k].spacing = span * q->samples_per_step / hits;
        q->ratchet[k].wait = q->ratchet[k].spacing;
    }
}

/* an early hit planned from the playing pattern, when a pattern chosen since then takes over at the coming step */
static int seq_waiting_replaced(const seq_t *q, const pattern_t *patterns, uint32_t k)
{
    const track_pat_t *t = q->waiting[k].t;
    return q->plan[k].t == t && q->plan[k].rolled && q->plan[k].step == q->waiting[k].step && q->next != q->playing
        && t == &patterns[q->playing].track[k] && q->count + 1u >= pattern_longest(&patterns[q->playing]);
}

/* the step can have been turned off or played in live since it was planned */
static void seq_release_waiting(seq_t *q, const pattern_t *patterns, uint32_t k, seq_emit_t emit)
{
    const track_pat_t *t = q->waiting[k].t;
    uint32_t step = q->waiting[k].step;
    int replaced = seq_waiting_replaced(q, patterns, k);
    q->waiting[k].t = 0;
    if (q->skip[k] == step + 1u) {
        q->skip[k] = 0;
        return;
    }
    if (t->steps[step].on && !replaced)
        seq_first_hit(q, k, t, step, emit);
}

static void seq_wait(seq_t *q, const pattern_t *patterns, uint32_t k, const track_pat_t *t, uint32_t step, uint32_t samples, seq_emit_t emit)
{
    if (q->waiting[k].t)                         /* a tempo rise can catch up with a late hit */
        seq_release_waiting(q, patterns, k, emit);
    q->waiting[k].t = t;
    q->waiting[k].step = (uint8_t)step;
    q->waiting[k].wait = samples;
}

static void seq_fire_due(seq_t *q, const pattern_t *patterns, seq_emit_t emit)
{
    uint32_t k;
    for (k = 0; k < NTRK; k++) {
        if (q->waiting[k].t && !q->waiting[k].wait)
            seq_release_waiting(q, patterns, k, emit);
        if (q->ratchet[k].left && !q->ratchet[k].wait) {
            q->ratchet[k].left--;
            q->ratchet[k].wait = q->ratchet[k].spacing;
            emit(k, q->ratchet[k].step, q->ratchet[k].t, q->ratchet[k].hits);
        }
    }
}

/* half a step before the grid: a step that fires early must start waiting now. The others decide on the grid, so an edit up to then still counts */
static void seq_look_ahead(seq_t *q, const pattern_t *patterns, seq_emit_t emit)
{
    const pattern_t *p = &patterns[q->playing];
    int switching = q->next != q->playing && q->count + 1u >= pattern_longest(p);
    uint32_t k;
    if (switching)
        p = &patterns[q->next];
    for (k = 0; k < NTRK; k++) {
        const track_pat_t *t = &p->track[k];
        uint32_t step = switching ? 0u : (q->pos[k] + 1u) % t->length;
        q->plan[k].t = 0;
        if (!t->steps[step].on)
            continue;
        q->plan[k].t = t;
        q->plan[k].step = (uint8_t)step;
        q->plan[k].offset = seq_offset(q, k, t, step);
        q->plan[k].rolled = q->plan[k].offset < 0;
        if (q->plan[k].rolled && seq_rand100(q) < t->steps[step].chance)
            seq_wait(q, patterns, k, t, step, (uint32_t)((int32_t)(q->samples_per_step - q->sample_acc) + q->plan[k].offset), emit);
    }
}

static void seq_fire(seq_t *q, const pattern_t *patterns, const pattern_t *p, seq_emit_t emit)
{
    uint32_t k;
    for (k = 0; k < NTRK; k++) {
        const track_pat_t *t = &p->track[k];
        uint32_t step = q->pos[k];
        const step_t *s = &t->steps[step];
        int planned = q->plan[k].t == t && q->plan[k].step == step, rolled = planned && q->plan[k].rolled;
        int32_t offset = q->plan[k].offset;
        q->plan[k].t = 0;
        if (q->skip[k] == step + 1u) {
            q->skip[k] = 0;
            continue;
        }
        if (rolled)
            continue;
        if (s->on && seq_rand100(q) < s->chance) {
            if (!planned)
                offset = seq_offset(q, k, t, step);
            if (offset > 0) {
                seq_wait(q, patterns, k, t, step, (uint32_t)offset, emit);
            } else {
                seq_first_hit(q, k, t, step, emit);
            }
        }
    }
}

static void seq_advance(seq_t *q, const pattern_t *patterns, seq_emit_t emit)
{
    const pattern_t *p = &patterns[q->playing];
    uint32_t k;
    q->looked = 0;
    q->count++;
    q->total++;
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
            q->ratchet[k].left = 0;
        }
    } else {
        for (k = 0; k < NTRK; k++)
            q->pos[k] = (uint8_t)((q->pos[k] + 1u) % p->track[k].length);
    }
    seq_fire(q, patterns, p, emit);
}

/* the track's step nearest to now */
static uint32_t seq_nearest_step(const seq_t *q, uint32_t track, uint32_t length)
{
    return q->sample_acc * 2u < q->samples_per_step ? q->pos[track] : (q->pos[track] + 1u) % length;
}

/* where a synced LFO reads its phase from */
static uint32_t seq_sync_pos(const seq_t *q)
{
    return q->total * LFO_SYNC_UNITS_PER_STEP + q->sample_acc * LFO_SYNC_UNITS_PER_STEP / q->samples_per_step;
}

static void seq_stop(seq_t *q)
{
    seq_rewind(q);
    q->run = SEQ_STOPPED;
}

/* a pattern chosen while stopped starts now: no pattern is playing that it must wait for */
static void seq_start(seq_t *q)
{
    q->playing = q->next;
    q->run = SEQ_STARTING;
}

/* call once for each render block */
static void seq_block(seq_t *q, const pattern_t *patterns, uint32_t samples, seq_emit_t emit)
{
    if (q->run == SEQ_STOPPED)
        return;
    if (q->run == SEQ_STARTING) {
        q->run = SEQ_RUNNING;
        seq_fire(q, patterns, &patterns[q->playing], emit);
    }
    while (samples) {
        uint32_t half = (q->samples_per_step + 1u) / 2u, to = q->looked ? q->samples_per_step : half;
        uint32_t d = to > q->sample_acc ? to - q->sample_acc : 0u, k;
        d = d < samples ? d : samples;
        for (k = 0; k < NTRK; k++) {
            if (q->waiting[k].t && q->waiting[k].wait < d)
                d = q->waiting[k].wait;
            if (q->ratchet[k].left && q->ratchet[k].wait < d)
                d = q->ratchet[k].wait;
        }
        q->sample_acc += d;
        samples -= d;
        for (k = 0; k < NTRK; k++) {
            if (q->waiting[k].t)
                q->waiting[k].wait -= d;
            if (q->ratchet[k].left)
                q->ratchet[k].wait -= d;
        }
        if (!q->looked && q->sample_acc >= half) {
            q->looked = 1;
            seq_look_ahead(q, patterns, emit);
        }
        if (q->sample_acc >= q->samples_per_step) {
            q->sample_acc -= q->samples_per_step;
            seq_advance(q, patterns, emit);
        }
        seq_fire_due(q, patterns, emit);    /* after the grid: a step that fires on a ratchet's next hit ends the ratchet */
    }
}
