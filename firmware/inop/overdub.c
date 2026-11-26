/* SPDX-License-Identifier: GPL-3.0-only */
/* Overdub's knobs: docs/index.md, REC. No hardware here: tests/inop_test.c runs this on the host. */
#define DUB_KNOBS 6u

typedef struct { uint8_t id; int8_t value; uint32_t moved_ms; } dub_knob_t;
typedef struct {
    dub_knob_t knob[DUB_KNOBS];  /* the values being written */
    uint8_t n, full;             /* full: a write found the track's lock memory full */
} dub_t;

static int dub_value(const dub_t *d, uint32_t id, int32_t *value)
{
    uint32_t i;
    for (i = 0; i < d->n; i++)
        if (d->knob[i].id == id) {
            *value = d->knob[i].value;
            return 1;
        }
    return 0;
}

/* a new knob with the table full replaces the one moved longest ago */
static void dub_set(dub_t *d, uint32_t id, int32_t value, uint32_t now)
{
    uint32_t i, at = d->n;
    for (i = 0; i < d->n; i++)
        if (d->knob[i].id == id)
            at = i;
    if (at == d->n && d->n == DUB_KNOBS) {
        at = 0;
        for (i = 1; i < d->n; i++)
            if (now - d->knob[i].moved_ms > now - d->knob[at].moved_ms)
                at = i;
    } else if (at == d->n) {
        d->n++;
    }
    d->knob[at].id = (uint8_t)id;
    d->knob[at].value = (int8_t)value;
    d->knob[at].moved_ms = now;
}

/* a knob still for window_ms stops writing, unless keep holds it */
static void dub_expire(dub_t *d, uint32_t now, uint32_t window_ms, int keep)
{
    uint32_t i, n = 0;
    for (i = 0; i < d->n; i++)
        if (keep || now - d->knob[i].moved_ms < window_ms)
            d->knob[n++] = d->knob[i];
    d->n = (uint8_t)n;
}

/* a played chord as locks: only what differs from the step's sound, so the step still follows later edits of the sound. 0, or -1 when the lock memory is full */
static int dub_chord_write(track_pat_t *t, uint32_t step, const pkt_t *played, const sound_t *pool)
{
    static const uint8_t IDS[] = {PV_CH_DEG, PV_CH_TYPE, PV_CH_BASS};
    const step_t *s = &t->steps[step];
    const sound_t *sound = &pool[s->sound != SOUND_NONE ? s->sound : t->base];
    uint32_t i;
    int result = 0;
    for (i = 0; i < NELEM(IDS); i++) {
        if (played->v[IDS[i]] == sound->v[IDS[i]])
            lock_clear(t, step, IDS[i]);
        else if (lock_set(t, step, IDS[i], played->v[IDS[i]]))
            result = -1;
    }
    t->steps[step].on = 1;
    return result;
}

/* a lock only changes its step's trigger, so a step that is off takes none */
static void dub_write(dub_t *d, track_pat_t *t, uint32_t step)
{
    uint32_t i;
    if (!t->steps[step].on)
        return;
    for (i = 0; i < d->n; i++)
        if (step_write(t, step, d->knob[i].id, d->knob[i].value) < 0)
            d->full = 1;
}
