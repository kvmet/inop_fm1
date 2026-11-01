/* SPDX-License-Identifier: GPL-3.0-only */
/* Steps, locks and packet resolution. No hardware here: tests/inop_test.c runs this on the host. */

static void sound_init(sound_t *s, uint32_t fm_alg)
{
    uint32_t i;
    for (i = 0; i < PV_COUNT; i++)
        s->v[i] = 0;
    s->v[PV_NOTE] = 60;
    s->v[PV_VEL] = 100;
    s->v[PV_LEN] = 1;
    s->v[PV_LEVEL] = 104;
    s->v[PV_AMP_D] = 64;
    s->v[PV_AMP_S] = 127;
    s->v[PV_AMP_R] = 30;
    s->v[PV_FLT_CUT] = 127;
    s->v[PV_MOD_D] = 64;
    s->v[PV_MOD_R] = 64;
    s->v[PV_LFO1_RATE] = 64;
    s->v[PV_LFO2_RATE] = 64;
    /* useful routes at amount 0: turning the amount up is all a sound needs */
    s->v[PV_SLOT_SRC0] = 1;      /* mod envelope to cutoff */
    s->v[PV_SLOT_DST0] = 2;
    s->v[PV_SLOT_SRC1] = 2;      /* LFO 1 to pitch */
    s->v[PV_SLOT_DST1] = 1;
    s->v[PV_SLOT_SRC2] = 3;      /* LFO 2 to cutoff */
    s->v[PV_SLOT_DST2] = 2;
    s->v[PV_SLOT_SRC3] = 4;      /* velocity to cutoff */
    s->v[PV_SLOT_DST3] = 2;
    s->v[PV_PRIO] = PRIO_NORMAL;
    s->v[PV_E0] = (int8_t)fm_alg;
    s->v[PV_E1] = 2;             /* FM ratios: C 1, A 2, B 1 */
    s->v[PV_E2] = 3;
    s->v[PV_E3] = 2;
    s->v[PV_E4] = 40;            /* FM index A */
}

static void track_init(track_pat_t *t, uint32_t base)
{
    uint32_t i;
    t->length = 16;
    t->base = (uint8_t)base;
    t->nlock = 0;
    for (i = 0; i < NSTEP; i++) {
        step_t *s = &t->steps[i];
        s->on = 0;
        s->note = STEP_INHERIT;
        s->vel = STEP_INHERIT;
        s->len = STEP_INHERIT;
        s->sound = SOUND_NONE;
        s->chance = STEP_CHANCE_ALWAYS;
        s->cond = 0;
        s->ratchet = 0;
    }
}

/* the first lock at or after (step, id) */
static uint32_t lock_lower(const track_pat_t *t, uint32_t step, uint32_t id)
{
    uint32_t lo = 0, hi = t->nlock, key = step << 8 | id;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;
        if ((uint32_t)(t->locks[mid].step << 8 | t->locks[mid].id) < key)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

/* 0, or -1 when the track's lock memory is full */
static int lock_set(track_pat_t *t, uint32_t step, uint32_t id, int32_t value)
{
    uint32_t at = lock_lower(t, step, id), i;
    if (at < t->nlock && t->locks[at].step == step && t->locks[at].id == id) {
        t->locks[at].value = (int8_t)value;
        return 0;
    }
    if (t->nlock >= TRACK_LOCKS)
        return -1;
    for (i = t->nlock; i > at; i--)
        t->locks[i] = t->locks[i - 1u];
    t->locks[at].step = (uint8_t)step;
    t->locks[at].id = (uint8_t)id;
    t->locks[at].value = (int8_t)value;
    t->nlock++;
    return 0;
}

/* note, velocity and length have fields in the step; other values are locks. 0, or -1 when the lock memory is full */
static int step_write(track_pat_t *t, uint32_t step, uint32_t id, int32_t value)
{
    step_t *s = &t->steps[step];
    if (id == PV_NOTE)
        s->note = (uint8_t)value;
    else if (id == PV_VEL)
        s->vel = (uint8_t)value;
    else if (id == PV_LEN)
        s->len = (uint8_t)value;
    else
        return lock_set(t, step, id, value);
    return 0;
}

static void lock_clear_range(track_pat_t *t, uint32_t from, uint32_t to)
{
    uint32_t i, n = t->nlock - to;
    for (i = 0; i < n; i++)
        t->locks[from + i] = t->locks[to + i];
    t->nlock = (uint16_t)(t->nlock - (to - from));
}

static void lock_clear(track_pat_t *t, uint32_t step, uint32_t id)
{
    uint32_t at = lock_lower(t, step, id);
    if (at < t->nlock && t->locks[at].step == step && t->locks[at].id == id)
        lock_clear_range(t, at, at + 1u);
}

static void lock_clear_step(track_pat_t *t, uint32_t step)
{
    lock_clear_range(t, lock_lower(t, step, 0), lock_lower(t, step + 1u, 0));
}

/* each engine's values start here, so a new engine sounds at once */
static const int8_t ENGINE_DEFAULTS[ENGINES][ENGINE_VALUES] = {
    [ENGINE_FM] = {0, 2, 3, 2, 40, 0, 0, 0},
    [ENGINE_PERC] = {1, 0, 64, 64, 20, 20, 100, 0},      /* SKIN */
    [ENGINE_SWARM] = {0, 40, 0, 0, 100, 0, 20, 0},
    [ENGINE_WAVE] = {0, 64, 0, 0, 0, 0, 0, 0},
};

/* the same values mean different things to each engine, so a knob swept across the engines must not rewrite them */
static void sound_set_engine(sound_t *s, engine_memory_t *m, int32_t next)
{
    int32_t cur = s->v[PV_ENGINE];
    uint32_t i;
    if (next == cur)
        return;
    for (i = 0; i < ENGINE_VALUES; i++)
        m->v[cur][i] = s->v[PV_E0 + i];
    m->seen |= (uint8_t)(1u << cur);
    for (i = 0; i < ENGINE_VALUES; i++)
        s->v[PV_E0 + i] = m->seen >> next & 1u ? m->v[next][i] : ENGINE_DEFAULTS[next][i];
    s->v[PV_ENGINE] = (int8_t)next;
}

static uint32_t step_lock_count(const track_pat_t *t, uint32_t step)
{
    return lock_lower(t, step + 1u, 0) - lock_lower(t, step, 0);
}

/* anything this step sets for itself */
static int step_locked(const track_pat_t *t, uint32_t step)
{
    const step_t *s = &t->steps[step];
    return s->sound != SOUND_NONE || s->note != STEP_INHERIT || s->vel != STEP_INHERIT || s->len != STEP_INHERIT || s->chance != STEP_CHANCE_ALWAYS || step_lock_count(t, step);
}

/* base sound, then the step's sound lock, then the step's locks */
static void packet_resolve(pkt_t *p, const sound_t *pool, const track_pat_t *t, uint32_t step)
{
    const step_t *s = &t->steps[step];
    uint32_t i = lock_lower(t, step, 0);
    *p = pool[s->sound != SOUND_NONE ? s->sound : t->base];
    if (s->note != STEP_INHERIT)
        p->v[PV_NOTE] = (int8_t)s->note;
    if (s->vel != STEP_INHERIT)
        p->v[PV_VEL] = (int8_t)s->vel;
    if (s->len != STEP_INHERIT)
        p->v[PV_LEN] = (int8_t)s->len;
    for (; i < t->nlock && t->locks[i].step == step; i++)
        p->v[t->locks[i].id] = t->locks[i].value;
}

/* tracks that reach this sound by their base or by a step's sound lock */
static uint32_t sound_use_count(const pattern_t *pats, uint32_t npat, uint32_t sound)
{
    uint32_t n = 0, p, k, i;
    for (p = 0; p < npat; p++)
        for (k = 0; k < NTRK; k++) {
            const track_pat_t *t = &pats[p].track[k];
            uint32_t used = t->base == sound;
            for (i = 0; i < NSTEP && !used; i++)
                used = t->steps[i].sound == sound;
            n += used;
        }
    return n;
}
