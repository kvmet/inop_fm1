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
    s->v[PV_MOD2_D] = 64;
    s->v[PV_MOD2_R] = 64;
    s->v[PV_LFO1_RATE] = 64;
    s->v[PV_LFO2_RATE] = 64;
    /* useful routes at amount 0: turning the amount up is all a sound needs */
    s->v[PV_SLOT_SRC0] = MOD_SRC_ENV;
    s->v[PV_SLOT_DST0] = MOD_DST_CUT;
    s->v[PV_SLOT_SRC1] = MOD_SRC_LFO1;
    s->v[PV_SLOT_DST1] = MOD_DST_PITCH;
    s->v[PV_SLOT_SRC2] = MOD_SRC_LFO2;
    s->v[PV_SLOT_DST2] = MOD_DST_CUT;
    s->v[PV_SLOT_SRC3] = MOD_SRC_VEL;
    s->v[PV_SLOT_DST3] = MOD_DST_CUT;
    s->v[PV_SLOT_SRC4] = MOD_SRC_ENV;   /* only the FM engine has an index */
    s->v[PV_SLOT_DST4] = MOD_DST_INDEX;
    s->v[PV_SLOT_AMT4] = 127;
    s->v[PV_PRIO] = PRIO_NORMAL;
    s->v[PV_ARP_RATE] = ARP_RATE_1_16;
    s->v[PV_ARP_RANGE] = 1;
    s->v[PV_ARP_GATE] = 64;
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
    t->swing = 0;
    t->human = 0;
    for (i = 0; i < NSTEP; i++) {
        step_t *s = &t->steps[i];
        s->on = 0;
        s->note = STEP_INHERIT;
        s->vel = STEP_INHERIT;
        s->len = STEP_INHERIT;
        s->sound = SOUND_NONE;
        s->chance = STEP_CHANCE_ALWAYS;
        s->hits = 1;
        s->time = 0;
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
    return s->sound != SOUND_NONE || s->note != STEP_INHERIT || s->vel != STEP_INHERIT || s->len != STEP_INHERIT || s->chance != STEP_CHANCE_ALWAYS || s->hits != 1u || s->time || step_lock_count(t, step);
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

static const uint8_t CH_SCALE_STEPS[CH_SCALES][7] = {
    [CH_MAJ] = {0, 2, 4, 5, 7, 9, 11},
    [CH_DOR] = {0, 2, 3, 5, 7, 9, 10},
    [CH_PHR] = {0, 1, 3, 5, 7, 8, 10},
    [CH_LYD] = {0, 2, 4, 6, 7, 9, 11},
    [CH_MIX] = {0, 2, 4, 5, 7, 9, 10},
    [CH_MIN] = {0, 2, 3, 5, 7, 8, 10},
    [CH_LOC] = {0, 1, 3, 5, 6, 8, 10},
    [CH_HARM] = {0, 2, 3, 5, 7, 8, 11},
    [CH_MELO] = {0, 2, 3, 5, 7, 9, 11},
};

typedef struct { uint8_t n; int8_t tone[5]; } ch_type_t;
static const ch_type_t CH_TYPE_TONES[CH_TYPES] = {
    [CH_TRI] = {3, {0, 2, 4}},
    [CH_7TH] = {4, {0, 2, 4, 6}},
    [CH_9TH] = {5, {0, 2, 4, 6, 8}},
    [CH_6TH] = {4, {0, 2, 4, 5}},
    [CH_ADD9] = {4, {0, 2, 4, 8}},
    [CH_SUS2] = {3, {0, 1, 4}},
    [CH_SUS4] = {3, {0, 3, 4}},
    [CH_5TH] = {2, {0, 4}},
    [CH_FIX_MAJ] = {3, {0, 4, 7}},
    [CH_FIX_MIN] = {3, {0, 3, 7}},
    [CH_FIX_MAJ7] = {4, {0, 4, 7, 11}},
    [CH_FIX_MIN7] = {4, {0, 3, 7, 10}},
    [CH_FIX_DOM7] = {4, {0, 4, 7, 10}},
    [CH_FIX_DIM] = {3, {0, 3, 6}},
    [CH_FIX_DIM7] = {4, {0, 3, 6, 9}},
    [CH_FIX_AUG] = {3, {0, 4, 8}},
};

/* semitones from the key to a scale step, which can be below the key or octaves above it */
static int32_t scale_semitones(int32_t scale, int32_t degree)
{
    int32_t octave = degree >= 0 ? degree / 7 : -((6 - degree) / 7);
    return 12 * octave + CH_SCALE_STEPS[clamp(scale, 0, CH_SCALES - 1)][degree - 7 * octave];
}

/* the notes a packet plays: NOTE is the key, DEG the scale step of the root. Tones stay in chord order, with a bass first. Returns the count */
static uint32_t chord_notes(const pkt_t *p, int8_t out[CHORD_NOTES])
{
    int32_t key = p->v[PV_NOTE], scale = p->v[PV_CH_SCALE], degree = p->v[PV_CH_DEG], voice = p->v[PV_CH_VOICE];
    int32_t type = clamp(p->v[PV_CH_TYPE], CH_OFF, CH_TYPES - 1), bass = p->v[PV_CH_BASS], notes[CHORD_NOTES], lowest;
    uint32_t n = 0, i;
    if (type == CH_OFF) {
        notes[n++] = key + scale_semitones(scale, degree);
    } else {
        const ch_type_t *t = &CH_TYPE_TONES[type];
        for (i = 0; i < t->n; i++)
            notes[n++] = key + (type < CH_FIX_MAJ ? scale_semitones(scale, degree + t->tone[i]) : scale_semitones(scale, degree) + t->tone[i]);
    }
    for (; voice && n > 1u; voice += voice > 0 ? -1 : 1) {       /* up moves the lowest note an octave up, down moves the highest an octave down */
        uint32_t at = 0;
        for (i = 1; i < n; i++)
            if (voice > 0 ? notes[i] < notes[at] : notes[i] > notes[at])
                at = i;
        notes[at] += voice > 0 ? 12 : -12;
    }
    lowest = notes[0];
    for (i = 1; i < n; i++)
        lowest = notes[i] < lowest ? notes[i] : lowest;
    if (bass > 0) {
        int32_t note = key + scale_semitones(scale, clamp(bass, 1, CH_BASS_MAX) - 1);
        while (note >= lowest)
            note -= 12;
        while (note + 12 < lowest)
            note += 12;
        out[0] = (int8_t)clamp(note, 0, 127);
        for (i = 0; i < n; i++)
            out[i + 1u] = (int8_t)clamp(notes[i], 0, 127);
        return n + 1u;
    }
    for (i = 0; i < n; i++)
        out[i] = (int8_t)clamp(notes[i], 0, 127);
    return n;
}

static int chord_has(const int8_t *notes, uint32_t n, int32_t note)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (notes[i] == note)
            return 1;
    return 0;
}

/* CHORD key mode: the black keys from the left, each a bit in fm1_in.notes */
enum { CHORD_KEY_7TH = 1, CHORD_KEY_9TH = 3, CHORD_KEY_6TH = 5, CHORD_KEY_SUS2 = 8, CHORD_KEY_SUS4 = 10, CHORD_KEY_FLIP = 13, CHORD_KEY_DOM7 = 15, CHORD_KEY_DIM = 17, CHORD_KEY_AUG = 20, CHORD_KEY_SLASH = 22 };
#define CHORD_MOD_KEYS (1u << CHORD_KEY_7TH | 1u << CHORD_KEY_9TH | 1u << CHORD_KEY_6TH | 1u << CHORD_KEY_SUS2 | 1u << CHORD_KEY_SUS4 | 1u << CHORD_KEY_FLIP | 1u << CHORD_KEY_DOM7 | 1u << CHORD_KEY_DIM | 1u << CHORD_KEY_AUG | 1u << CHORD_KEY_SLASH)

typedef struct { uint8_t bass; } chord_keys_t;   /* bass: the slash bass while the slash key stays down, or 0 */

/* white keys by letter, as if in C: every C is the key */
static int32_t chord_key_degree(uint32_t k)
{
    static const int8_t DEGREE_OF_PITCH_CLASS[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
    return DEGREE_OF_PITCH_CLASS[(5u + k) % 12u] + 7 * ((int32_t)((5u + k) / 12u) - 1);
}

/* with no type on the sound, a key plays a triad. The rightmost modifier held wins, and FLIP takes 7TH along */
static int32_t chord_key_type(int32_t sound_type, int32_t scale, int32_t degree, uint32_t notes)
{
    static const struct { uint8_t key, type; } PLAIN[] = {
        {CHORD_KEY_SUS4, CH_SUS4}, {CHORD_KEY_SUS2, CH_SUS2}, {CHORD_KEY_6TH, CH_6TH}, {CHORD_KEY_9TH, CH_9TH}, {CHORD_KEY_7TH, CH_7TH},
    };
    uint32_t i;
    if (notes >> CHORD_KEY_AUG & 1u)
        return CH_FIX_AUG;
    if (notes >> CHORD_KEY_DIM & 1u)
        return CH_FIX_DIM;
    if (notes >> CHORD_KEY_DOM7 & 1u)
        return CH_FIX_DOM7;
    if (notes >> CHORD_KEY_FLIP & 1u) {
        int major = scale_semitones(scale, degree + 2) - scale_semitones(scale, degree) == 4, seventh = notes >> CHORD_KEY_7TH & 1u;
        return major ? (seventh ? CH_FIX_MIN7 : CH_FIX_MIN) : (seventh ? CH_FIX_MAJ7 : CH_FIX_MAJ);
    }
    for (i = 0; i < NELEM(PLAIN); i++)
        if (notes >> PLAIN[i].key & 1u)
            return PLAIN[i].type;
    return sound_type != CH_OFF ? sound_type : CH_TRI;
}

/* turns a press of white key k into its chord on p, which starts as the sound. 0: the press only chose a slash bass, so nothing sounds */
static int chord_keys_press(chord_keys_t *c, pkt_t *p, uint32_t k, uint32_t notes, int32_t octave)
{
    int32_t degree = chord_key_degree(k);
    if (!(notes >> CHORD_KEY_SLASH & 1u)) {
        c->bass = 0;
    } else if (!c->bass) {
        c->bass = (uint8_t)((degree + 7) % 7 + 1);
        return 0;
    }
    degree = clamp(degree + 7 * octave, -CH_DEG_MAX, CH_DEG_MAX);
    p->v[PV_CH_TYPE] = (int8_t)chord_key_type(p->v[PV_CH_TYPE], p->v[PV_CH_SCALE], degree, notes);
    p->v[PV_CH_DEG] = (int8_t)degree;
    if (c->bass)
        p->v[PV_CH_BASS] = (int8_t)c->bass;
    return 1;
}

/* does this step set this value for itself? */
static int step_value_locked(const track_pat_t *t, uint32_t step, uint32_t id)
{
    const step_t *s = &t->steps[step];
    uint32_t at = lock_lower(t, step, id);
    if (id == PV_NOTE)
        return s->note != STEP_INHERIT;
    if (id == PV_VEL)
        return s->vel != STEP_INHERIT;
    if (id == PV_LEN)
        return s->len != STEP_INHERIT;
    return at < t->nlock && t->locks[at].step == step && t->locks[at].id == id;
}

/* the trigger stays: the step plays the base sound again, every time */
static void step_clear_locks(track_pat_t *t, uint32_t step)
{
    step_t *s = &t->steps[step];
    lock_clear_step(t, step);
    s->note = s->vel = s->len = STEP_INHERIT;
    s->sound = SOUND_NONE;
    s->chance = STEP_CHANCE_ALWAYS;
    s->hits = 1;
    s->time = 0;
}

/* 0, or -1 when the lock memory filled before every lock was copied */
static int step_copy(track_pat_t *t, uint32_t from, uint32_t to)
{
    uint32_t i, n;
    int result = 0;
    if (from == to)
        return 0;
    lock_clear_step(t, to);
    t->steps[to] = t->steps[from];
    n = step_lock_count(t, from);
    for (i = 0; i < n; i++) {
        lock_t l = t->locks[lock_lower(t, from, 0) + i];   /* a lock set on an earlier step moves the source */
        if (lock_set(t, to, l.id, l.value))
            result = -1;
    }
    return result;
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
