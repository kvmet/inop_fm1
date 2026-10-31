/* SPDX-License-Identifier: GPL-3.0-only */
/* The panel: docs/index.md, Keyboard and REC. Runs in the main loop. */
enum { B_FX = 2, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC, B_OCTDN = 0, B_OCTUP = 1 };
enum { E_SELECT = 0, E_ALGO = 1, E_PRESETS = 6, E_K1 = 2 };
#define HOLD_MS 300u
#define LOCK_BLINK_MS 64u        /* out of each 1024 ms */
#define NO_KEY 0xFFu
#define KEY_LED(k) (14u + (k))
#define SHIFT_BLACK 10u          /* the last black key */
#define SHIFT_STEP 10

#define OCTAVE_MAX 3
#define SLOTS 4u
#define NKNOB 6u
#define DUB_STILL_MS 150u        /* with one step, the longest a knob can rest and keep writing */
/* a page's values in screen order: row 1 is SEL and knobs 1 to 4, row 2 is ALGO */
static const uint8_t KNOB_ENC[NKNOB] = {E_SELECT, E_K1, E_K1 + 1, E_K1 + 2, E_K1 + 3, E_ALGO};

/* values that belong to the panel and not to a sound */
enum { ID_BPM = ID_GLOBAL(G_COUNT), ID_SLOT, ID_LENGTH, ID_BASE, ID_NONE = 0xFF };

typedef struct { const char *name; uint8_t id[NKNOB]; const char *label[NKNOB]; } page_t;
static const page_t PAGES[] = {
    {"HOME", {PV_PAN, PV_NOTE, PV_VEL, PV_LEN, PV_LEVEL, PV_PRIO}, {"PAN", "NOTE", "VEL", "LEN", "LEVEL", "PRIO"}},
    {"AMP", {ID_NONE, PV_AMP_A, PV_AMP_D, PV_AMP_S, PV_AMP_R, ID_NONE}, {"", "ATK", "DEC", "SUS", "REL", ""}},
    {"FLT", {ID_NONE, PV_FLT_TYPE, PV_FLT_CUT, PV_FLT_RES, PV_FLT_KEY, ID_NONE}, {"", "TYPE", "CUT", "RES", "KEY", ""}},
    {"MOD", {ID_NONE, PV_MOD_A, PV_MOD_D, PV_MOD_S, PV_MOD_R, ID_NONE}, {"", "ATK", "DEC", "SUS", "REL", ""}},
    {"FX", {PV_SEND_DELAY, PV_SHAPER, PV_CRUSH, PV_RATE, PV_DRIVE, PV_SEND_REVERB}, {"DELAY", "SHAPE", "CRUSH", "RATE", "DRIVE", "REVERB"}},
    {"LFO1", {ID_NONE, PV_LFO1_SHAPE, PV_LFO1_PHASE, PV_LFO1_RATE, PV_LFO1_START, ID_NONE}, {"", "SHAPE", "MODE", "RATE", "START", ""}},
    {"LFO2", {ID_NONE, PV_LFO2_SHAPE, PV_LFO2_PHASE, PV_LFO2_RATE, PV_LFO2_START, ID_NONE}, {"", "SHAPE", "MODE", "RATE", "START", ""}},
    {"SLOT", {ID_SLOT, PV_SLOT_SRC0, PV_SLOT_DST0, PV_SLOT_AMT0, ID_NONE, ID_NONE}, {"SLOT", "SRC", "DST", "AMT", "", ""}},
    {"ENG1", {PV_ENGINE, PV_E1, PV_E2, PV_E3, PV_E4, PV_E0}, {"ENGINE", "E1", "E2", "E3", "E4", "E0"}},
    {"ENG2", {ID_NONE, PV_E5, PV_E6, PV_E7, ID_NONE, ID_NONE}, {"", "E5", "E6", "E7", "", ""}},
    {"GLOB", {ID_BPM, ID_GLOBAL(G_DELAY_TIME), ID_GLOBAL(G_DELAY_FB), ID_GLOBAL(G_REVERB_SIZE), ID_GLOBAL(G_REVERB_DAMP), ID_NONE}, {"BPM", "DLY T", "DLY FB", "SIZE", "DAMP", ""}},
    {"TRK", {ID_NONE, ID_LENGTH, ID_BASE, ID_NONE, ID_NONE, ID_NONE}, {"", "LENGTH", "SOUND", "", "", ""}},
};

enum { PG_HOME, PG_AMP, PG_FILTER, PG_MOD, PG_FX, PG_LFO1, PG_LFO2, PG_SLOTS, PG_E1, PG_E2, PG_GLOBAL, PG_TRACK };
_Static_assert(PG_TRACK + 1 == NELEM(PAGES), "PG_ names follow PAGES");

/* a page button's first press shows its group's last page; each next press moves through the group */
typedef struct { uint8_t button, n, page[3]; } page_group_t;
static const page_group_t GROUPS[] = {
    {B_HOME, 1, {PG_HOME}},
    {B_SEL, 2, {PG_E1, PG_E2}},
    {B_ARP, 1, {PG_FILTER}},
    {B_ENV, 2, {PG_AMP, PG_MOD}},
    {B_FX, 1, {PG_FX}},
    {B_LFO, 3, {PG_LFO1, PG_LFO2, PG_SLOTS}},
    {B_GLO, 1, {PG_GLOBAL}},
};
#define NGROUPS NELEM(GROUPS)

/* key index to white position 0..15 or black position 16..26 */
static uint8_t key_pos[27];
static uint8_t white_key[16], black_key[11];
static uint32_t white_keys;
static void keys_map_init(void)
{
    uint32_t k, w = 0, b = 0;
    for (k = 0; k < 27u; k++) {
        uint32_t pc = (5u + k) % 12u;
        if (pc == 1u || pc == 3u || pc == 6u || pc == 8u || pc == 10u) {
            key_pos[k] = (uint8_t)(16u + b);
            black_key[b++] = (uint8_t)k;
        } else {
            key_pos[k] = (uint8_t)w;
            white_key[w++] = (uint8_t)k;
        }
    }
}

static struct {
    uint8_t track, edit_pat, view, pattern_mode;
    uint8_t group, group_page[NGROUPS];
    uint8_t copy_src;            /* REC held: the source, NO_KEY until one is pressed */
    uint8_t slot;                /* the mod slot its page edits */
    uint8_t track_key;           /* the held track key, or NO_KEY */
    uint8_t sound_copy, sound_to;
    uint8_t dub, dub_prev_play;  /* overdub on, and the keys' state to return to */
    uint8_t play_used;           /* this PLAY press already acted: its release must not toggle the keys */
    uint32_t bpm, rec_t0, held_keys, track_t0;
} ui;
enum { SOUND_COPY_NONE, SOUND_COPY_CHOOSE, SOUND_COPY_CONFIRM };

/* one per white key. t is 0 when the key holds no step */
typedef struct { track_pat_t *t; uint32_t t0; uint8_t step, used; } held_t;
static held_t held[16];
static held_t *held_first;       /* the oldest held step: the screen shows its values */

static track_pat_t *ui_track(void) { return &patterns[ui.edit_pat].track[ui.track]; }
static const page_t *ui_page(void)
{
    if (ui.track_key != NO_KEY && fm1_ms - ui.track_t0 >= HOLD_MS)
        return &PAGES[PG_TRACK];
    return &PAGES[GROUPS[ui.group].page[ui.group_page[ui.group]]];
}

/* the slot page lists slot 1's ids: the other slots follow them */
static uint32_t page_id(const page_t *pg, uint32_t k)
{
    uint32_t id = pg->id[k];
    return id >= PV_SLOT_SRC0 && id <= PV_SLOT_AMT3 ? id + ui.slot : id;
}

static int ui_shift(void) { return !ui.pattern_mode && ((fm1_in.notes >> black_key[SHIFT_BLACK]) & 1u); }

static void page_button(uint32_t g)
{
    if (!ui.pattern_mode && ui.group == g)
        ui.group_page[g] = (uint8_t)((ui.group_page[g] + 1u) % GROUPS[g].n);
    ui.group = (uint8_t)g;
    ui.pattern_mode = 0;
}

static dub_t dub;
static struct { track_pat_t *t; uint32_t t0; uint8_t step; } dub_note[27];

static track_pat_t *dub_track(void) { return &patterns[seq.playing].track[ui.track]; }
static uint32_t dub_next_step(void) { return (seq.pos[ui.track] + 1u) % dub_track()->length; }
static uint32_t step_ms(void) { return seq.samples_per_step * 1000u / FS; }

/* what the knobs show: the held step's values as it would play, in overdub the last trigger's, else the base sound's */
static void ui_packet(pkt_t *p)
{
    if (held_first)
        packet_resolve(p, sounds, held_first->t, held_first->step);
    else if (ui.dub && seq.fired[ui.track] != SEQ_NOT_FIRED)
        packet_resolve(p, sounds, dub_track(), seq.fired[ui.track]);
    else
        *p = sounds[ui_track()->base];
}

static int32_t ui_value(uint32_t id)
{
    pkt_t p;
    int32_t moving;
    if (ui.dub && id < PV_COUNT && dub_value(&dub, id, &moving))
        return moving;
    if (id == ID_BPM)
        return (int32_t)ui.bpm;
    if (id == ID_SLOT)
        return ui.slot;
    if (id == ID_LENGTH)
        return ui_track()->length;
    if (id == ID_BASE)
        return ui_track()->base + 1;
    if (id >= PV_COUNT)
        return globals[id - PV_COUNT];
    ui_packet(&p);
    return p.v[id];
}

/* a selector's names, or 0 for a plain number */
static const char *const NAMES_SLOT[SLOTS] = {"1", "2", "3", "4"};
static const char *const NAMES_FILTER[FLT_TYPES] = {"OFF", "LP", "BP", "HP"};
static const char *const NAMES_SRC[MOD_SRCS] = {"OFF", "ENV", "LF1", "LF2", "VEL"};
static const char *const NAMES_DST[MOD_DSTS] = {"OFF", "PTCH", "CUT", "RES", "LVL", "PAN", "DRV", "SHP", "CRSH", "RATE", "TIMB"};
static const char *const NAMES_SHAPE[LFO_SHAPES] = {"SINE", "TRI", "SAW", "SQR", "S+H"};
static const char *const NAMES_MODE[LFO_MODES] = {"RTRG", "FREE"};
static const char *const NAMES_ENGINE[ENGINES] = {"FM", "PHYS", "PERC", "SWARM"};
static const char *const NAMES_PRIO[PRIOS] = {"XXLO", "XLO", "LOW", "NORM", "HIGH", "XHI", "XXHI"};
static const char *const NAMES_MODEL[PM_COUNT] = {"MODL", "STRG", "MEMB", "SYMP"};
static const char *const LABELS_PHYS[PM_COUNT][7] = {
    {"STRC", "BRIT", "DAMP", "POS", "ACC", "BOW", "EXC"},
    {"STRC", "BRIT", "DAMP", "POS", "ACC", "BOW", "EXC"},
    {"HARM", "BRIT", "DAMP", "POS", "ACC", "BEND", "EXC"},
    {"CHRD", "BRIT", "DAMP", "SYMP", "ACC", "BUZZ", "EXC"},
};

static const char *const NAMES_ALG[FM_ALGS] = {"1", "2", "3", "4", "5", "6", "7", "8"};
static const char *const NAMES_RATIO[FM_RATIOS] = {"1/4", "1/2", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "14", "16"};
static const char *const LABELS_FM[7] = {"RATIO C", "RATIO A", "RATIO B", "INDEX A", "INDEX B", "FEEDBK", "DETUNE"};
static const char *const NAMES_SWARM[SWARM_MODES] = {"SAME", "OCTS"};
static const char *const LABELS_SWARM[7] = {"DETUNE", "SHAPE", "SYNC", "SIDES", "SUB", "DRIFT", "--"};
static const char *const NAMES_PERC[PERC_MODES] = {"TONE", "SKIN", "BAR", "METAL"};
static const char *const LABELS_PERC[7] = {"FM", "SPREAD", "HARM", "BEND", "NOISE", "CLICK", "--"};

static int ui_engine(void) { return clamp(ui_value(PV_ENGINE), 0, ENGINES - 1); }

/* the engine values mean what the sound's engine says */
static const char *ui_label(const page_t *pg, uint32_t k)
{
    uint32_t id = page_id(pg, k);
    if (id == PV_E0)
        return ui_engine() == ENGINE_SWARM ? "STACK" : ui_engine() == ENGINE_PERC ? "MODE" : ui_engine() == ENGINE_PHYS ? "MODEL" : "ALGO";
    if (id > PV_E0 && id <= PV_E7)
        return ui_engine() == ENGINE_SWARM ? LABELS_SWARM[id - PV_E1] : ui_engine() == ENGINE_PERC ? LABELS_PERC[id - PV_E1] : ui_engine() == ENGINE_PHYS ? LABELS_PHYS[(uint32_t)clamp(ui_value(PV_E0), 0, 127) % PM_COUNT][id - PV_E1] : LABELS_FM[id - PV_E1];
    return pg->label[k];
}

static const char *const *value_names(uint32_t id, int32_t *count)
{
    if (id == ID_SLOT) { *count = SLOTS; return NAMES_SLOT; }
    if (id == PV_FLT_TYPE) { *count = FLT_TYPES; return NAMES_FILTER; }
    if (id == PV_ENGINE) { *count = ENGINES; return NAMES_ENGINE; }
    if (id == PV_PRIO) { *count = PRIOS; return NAMES_PRIO; }
    if (id == PV_E0 && ui_engine() == ENGINE_PHYS) { *count = PM_COUNT; return NAMES_MODEL; }
    if (id == PV_E0 && ui_engine() == ENGINE_PERC) { *count = PERC_MODES; return NAMES_PERC; }
    if (id == PV_E0 && ui_engine() == ENGINE_SWARM) { *count = SWARM_MODES; return NAMES_SWARM; }
    if (id == PV_E0 && ui_engine() == ENGINE_FM) { *count = FM_ALGS; return NAMES_ALG; }
    if (id >= PV_E1 && id <= PV_E3 && ui_engine() == ENGINE_FM) { *count = FM_RATIOS; return NAMES_RATIO; }
    if (id >= PV_SLOT_SRC0 && id <= PV_SLOT_SRC3) { *count = MOD_SRCS; return NAMES_SRC; }
    if (id >= PV_SLOT_DST0 && id <= PV_SLOT_DST3) { *count = MOD_DSTS; return NAMES_DST; }
    if (id == PV_LFO1_SHAPE || id == PV_LFO2_SHAPE) { *count = LFO_SHAPES; return NAMES_SHAPE; }
    if (id == PV_LFO1_PHASE || id == PV_LFO2_PHASE) { *count = LFO_MODES; return NAMES_MODE; }
    return 0;
}

/* each value stops at its own ends, so a knob turned past one answers at once when turned back */
static int32_t value_clamp(uint32_t id, int32_t v)
{
    int32_t count;
    if (id == ID_BPM)
        return clamp(v, 30, 300);
    if (id == ID_LENGTH)
        return clamp(v, 1, NSTEP);
    if (id == ID_BASE)
        return clamp(v, 1, NSOUND);
    if (id == ID_GLOBAL(G_DELAY_TIME))
        return clamp(v, 1, 16);                  /* in steps */
    if (id == PV_VEL)
        return clamp(v, 1, 127);
    if (id == PV_LEN)
        return clamp(v, 1, LEN_HOLD);
    if (value_names(id, &count))
        return clamp(v, 0, count - 1);
    if (id == PV_PAN)
        return clamp(v, -63, 63);
    if (id >= PV_SLOT_AMT0 && id <= PV_SLOT_AMT3)
        return clamp(v, -127, 127);
    return clamp(v, 0, 127);
}

/* each held step moves by delta from its own value, so steps that differ keep their differences */
static void held_knob(held_t *h, uint32_t id, int32_t delta)
{
    pkt_t p;
    int32_t v;
    uint32_t irq;
    packet_resolve(&p, sounds, h->t, h->step);
    v = value_clamp(id, p.v[id] + delta);
    irq = irq_save();
    h->t->steps[h->step].on = 1;
    (void)step_write(h->t, h->step, id, v);
    irq_restore(irq);
    h->used = 1;
}

/* a moved knob starts from the value on the screen, or from what dub_value holds: ui_value reads both */
static void dub_knob(uint32_t id, int32_t delta)
{
    dub_set(&dub, id, value_clamp(id, ui_value(id) + delta), fm1_ms);
}

/* writes ahead of the playhead: a step's packet is resolved as it fires, so the next trigger plays the knob's value */
static void dub_service(uint32_t rec)
{
    uint32_t irq, window = step_ms() > DUB_STILL_MS ? step_ms() : DUB_STILL_MS;
    dub_expire(&dub, fm1_ms, window, rec != 0u);
    irq = irq_save();
    dub_write(&dub, dub_track(), dub_next_step());
    irq_restore(irq);
    ui.edit_pat = seq.playing;
}

static void dub_enter(void)
{
    ui.dub = 1;
    ui.dub_prev_play = keys_play;
    keys_play = 1;
    keys_mask = ((1u << 27) - 1u) & ~(1u << black_key[SHIFT_BLACK]);
    ui.pattern_mode = 0;
    ui.edit_pat = seq.playing;
    ui.sound_copy = SOUND_COPY_NONE;
    dub.n = 0;
    dub.full = 0;
}

static void dub_exit(void)
{
    ui.dub = 0;
    keys_play = ui.dub_prev_play;
    keys_mask = white_keys;
}

/* a press sounds now. On a step still ahead, that step must not sound again when the playhead reaches it */
static void dub_key_down(uint32_t k)
{
    track_pat_t *t = dub_track();
    uint32_t step = seq_nearest_step(&seq, ui.track, t->length);
    uint32_t irq = irq_save();
    t->steps[step].on = 1;
    t->steps[step].note = (uint8_t)keys_note(k);
    if (step != seq.pos[ui.track])
        seq.skip[ui.track] = (uint8_t)(step + 1u);
    irq_restore(irq);
    dub_note[k].t = t;
    dub_note[k].step = (uint8_t)step;
    dub_note[k].t0 = fm1_ms;
}

static void dub_key_up(uint32_t k)
{
    if (dub_note[k].t) {
        uint32_t ms = step_ms();
        dub_note[k].t->steps[dub_note[k].step].len = (uint8_t)clamp((int32_t)((fm1_ms - dub_note[k].t0 + ms / 2u) / ms), 1, NSTEP);
        dub_note[k].t = 0;
    }
}

static void ui_knob(uint32_t id, int32_t delta)
{
    uint32_t i;
    int32_t panel;
    if (ui.dub && id < PV_COUNT) {
        dub_knob(id, delta);
        return;
    }
    panel = value_clamp(id, ui_value(id) + delta);
    if (id == ID_BPM) {
        ui.bpm = (uint32_t)panel;
        seq_set_bpm(&seq, ui.bpm);
    } else if (id == ID_SLOT) {
        ui.slot = (uint8_t)panel;
    } else if (id == ID_LENGTH) {
        track_pat_t *t = ui_track();
        uint32_t irq = irq_save();
        t->length = (uint8_t)panel;
        irq_restore(irq);
        while (ui.view && ui.view * 16u >= t->length)
            ui.view--;
    } else if (id == ID_BASE) {
        ui_track()->base = (uint8_t)(panel - 1);
    } else if (id >= PV_COUNT) {
        globals[id - PV_COUNT] = (int8_t)value_clamp(id, globals[id - PV_COUNT] + delta);
    } else if (held_first) {
        for (i = 0; i < NELEM(held); i++)
            if (held[i].t)
                held_knob(&held[i], id, delta);
    } else {
        track_pat_t *t = ui_track();
        int8_t v = (int8_t)value_clamp(id, sounds[t->base].v[id] + delta);
        uint32_t irq = irq_save();
        sounds[t->base].v[id] = v;
        irq_restore(irq);
    }
}

static void held_sound(held_t *h, int32_t delta)
{
    step_t *s = &h->t->steps[h->step];
    int32_t cur = s->sound == SOUND_NONE ? h->t->base : s->sound;
    s->sound = (uint8_t)((cur + delta + (int32_t)NSOUND) % (int32_t)NSOUND);
    s->on = 1;
    h->used = 1;
}

static void copy_step(track_pat_t *t, uint32_t from, uint32_t to)
{
    uint32_t i, n;
    uint32_t irq = irq_save();
    lock_clear_step(t, to);
    t->steps[to] = t->steps[from];
    n = t->nlock;
    for (i = 0; i < n; i++)
        if (t->locks[i].step == from)
            (void)lock_set(t, to, t->locks[i].id, t->locks[i].value);
    irq_restore(irq);
}

static void copy_pattern(uint32_t from, uint32_t to)
{
    uint32_t irq = irq_save();
    patterns[to] = patterns[from];
    irq_restore(irq);
}

static void copy_track(uint32_t from, uint32_t to)
{
    uint32_t irq = irq_save();
    patterns[ui.edit_pat].track[to] = patterns[ui.edit_pat].track[from];
    irq_restore(irq);
}

static void sound_copy_commit(void)
{
    track_pat_t *t = ui_track();
    uint32_t irq = irq_save();
    sounds[ui.sound_to] = sounds[t->base];
    t->base = ui.sound_to;
    irq_restore(irq);
}

static void key_pattern(uint32_t pos, uint32_t rec)
{
    if (rec) {
        if (ui.copy_src == NO_KEY)
            ui.copy_src = (uint8_t)pos;
        else
            copy_pattern(ui.copy_src, pos);
        return;
    }
    if (pos == ui.edit_pat)
        seq.next = (uint8_t)pos;
    else
        ui.edit_pat = (uint8_t)pos;
}

static void key_down(uint32_t k, uint32_t rec)
{
    uint32_t pos = key_pos[k];
    if (ui.pattern_mode) {
        key_pattern(pos, rec);
        return;
    }
    if (ui.dub) {                                /* the track stays selected, so the black keys are notes */
        if (pos != 16u + SHIFT_BLACK)
            dub_key_down(k);
        return;
    }
    if (pos >= 16u) {                            /* black keys */
        uint32_t trk = pos - 16u;
        if (trk >= NTRK)
            return;
        if (rec) {
            if (ui.copy_src == NO_KEY)
                ui.copy_src = (uint8_t)trk;
            else
                copy_track(ui.copy_src, trk);
        } else {
            ui.track = (uint8_t)trk;
            keys_track = (uint8_t)trk;
            ui.track_key = (uint8_t)trk;
            ui.track_t0 = fm1_ms;
        }
        return;
    }
    if (keys_play)
        return;
    pos += ui.view * 16u;
    if (rec) {
        if (ui.copy_src == NO_KEY)
            ui.copy_src = (uint8_t)pos;
        else
            copy_step(ui_track(), ui.copy_src, pos);
        return;
    }
    {
        held_t *h = &held[key_pos[k]];
        h->t = ui_track();
        h->step = (uint8_t)pos;
        h->used = 0;
        h->t0 = fm1_ms;
        if (!held_first)
            held_first = h;
    }
}

/* a key's release counts whatever the other keys or the mode did since its press */
static void key_up(uint32_t k)
{
    uint32_t pos = key_pos[k], i;
    held_t *h = pos < 16u ? &held[pos] : 0;
    dub_key_up(k);
    if (pos >= 16u && pos - 16u == ui.track_key)
        ui.track_key = NO_KEY;
    if (!h || !h->t)
        return;
    if (!h->used && fm1_ms - h->t0 < HOLD_MS)
        h->t->steps[h->step].on ^= 1u;
    h->t = 0;
    if (held_first != h)
        return;
    held_first = 0;
    for (i = 0; i < NELEM(held); i++)
        if (held[i].t && (!held_first || fm1_ms - held[i].t0 > fm1_ms - held_first->t0))
            held_first = &held[i];
}

/* the confirm step of a sound copy: REC copies, and any other press or turn cancels and does nothing else */
static void sound_copy_confirm(uint32_t edges, uint32_t pressed)
{
    uint32_t i, other = (edges & ~(1u << B_REC)) | pressed;
    for (i = 0; i < FM1_NENC; i++)
        other |= fm1_enc_take(i) != 0;
    if (edges >> B_REC & 1u)
        sound_copy_commit();
    if (other || edges >> B_REC & 1u)
        ui.sound_copy = SOUND_COPY_NONE;
}

static void presets_knob(int32_t d, uint32_t rec)
{
    uint32_t k;
    track_pat_t *t = ui_track();
    if (rec) {
        if (ui.sound_copy == SOUND_COPY_NONE) {
            ui.sound_copy = SOUND_COPY_CHOOSE;
            ui.sound_to = t->base;
        }
        ui.sound_to = (uint8_t)(((int32_t)ui.sound_to + d + (int32_t)NSOUND) % (int32_t)NSOUND);
    } else if (held_first) {
        for (k = 0; k < NELEM(held); k++)
            if (held[k].t)
                held_sound(&held[k], d);
    } else {
        t->base = (uint8_t)(((int32_t)t->base + d + (int32_t)NSOUND) % (int32_t)NSOUND);
    }
}

static void oct_button(int32_t dir)
{
    if (keys_play)
        keys_octave = (int8_t)clamp(keys_octave + dir, -OCTAVE_MAX, OCTAVE_MAX);
    else if (dir > 0 && (ui.view + 1u) * 16u < ui_track()->length)
        ui.view++;
    else if (dir < 0 && ui.view)
        ui.view--;
}

static void ui_input(void)
{
    uint32_t released, edges = fm1_input_edges(&released), now = fm1_in.notes, changed = now ^ ui.held_keys, k;
    uint32_t rec = (fm1_in.buttons >> B_REC) & 1u, pressed = now & changed;
    int32_t d, shift = ui_shift() ? SHIFT_STEP : 1;
    const page_t *pg;
    ui.held_keys = now;
    for (k = 0; k < 27u; k++)
        if ((changed & ~now) >> k & 1u)
            key_up(k);
    if (released >> B_PLAY & 1u && !ui.play_used && ui.sound_copy != SOUND_COPY_CONFIRM) {
        keys_play = !keys_play;
        ui.pattern_mode = 0;
    }
    if (ui.sound_copy == SOUND_COPY_CONFIRM) {
        if (edges >> B_PLAY & 1u)
            ui.play_used = 1;
        sound_copy_confirm(edges, pressed);
        return;
    }
    if (edges >> B_PLAY & 1u) {
        ui.play_used = ui.dub || rec;
        if (ui.dub)
            dub_exit();
        else if (rec)
            dub_enter();
    } else if (edges >> B_REC & 1u && fm1_in.buttons >> B_PLAY & 1u && !ui.dub) {
        ui.play_used = 1;
        dub_enter();
    }
    if (released >> B_REC & 1u && ui.sound_copy == SOUND_COPY_CHOOSE)
        ui.sound_copy = ui.sound_to != ui_track()->base ? SOUND_COPY_CONFIRM : SOUND_COPY_NONE;
    if (edges >> B_REC & 1u) {
        ui.rec_t0 = fm1_ms;
        ui.copy_src = NO_KEY;
    }
    for (k = 0; k < 27u; k++)
        if (pressed >> k & 1u)
            key_down(k, rec && !ui.dub);
    if (edges >> B_SEQ & 1u && !ui.dub)
        ui.pattern_mode = !ui.pattern_mode;
    for (k = 0; k < NGROUPS; k++)
        if (edges >> GROUPS[k].button & 1u)
            page_button(k);
    if (edges >> B_OCTUP & 1u)
        oct_button(1);
    if (edges >> B_OCTDN & 1u)
        oct_button(-1);
    d = fm1_enc_take(E_PRESETS) * shift;
    if (d)
        presets_knob(d, rec && !ui.dub);
    pg = ui_page();
    for (k = 0; k < NKNOB; k++) {
        d = fm1_enc_take(KNOB_ENC[k]);
        if (d && pg->id[k] != ID_NONE) {
            int32_t count;
            uint32_t id = page_id(pg, k);
            ui_knob(id, value_names(id, &count) ? d : d * shift);   /* a selector's short list would jump to its end */
        }
    }
    if (ui.dub)
        dub_service(rec);
}

static uint32_t ui_blink(void) { return (fm1_ms >> 7) & 1u; }

static void ui_leds(void)
{
    uint32_t i, blink = ui_blink(), lock_blink = (fm1_ms & 1023u) < LOCK_BLINK_MS;
    const track_pat_t *t = ui_track();
    for (i = 0; i < 16u; i++) {
        uint32_t level = FM1_LV_GLOW;
        if (ui.pattern_mode) {
            if (i == seq.playing || (i == seq.next && blink) || (i == ui.edit_pat && blink))
                level = FM1_LV_LIT;
        } else if (!keys_play) {
            uint32_t st = i + ui.view * 16u, in_track = st < t->length, on = in_track && t->steps[st].on;
            uint32_t bright_group = !((i >> 2) & 1u);    /* steps 1-4 and 9-12, as on the screen */
            if (seq.playing == ui.edit_pat && st == seq.pos[ui.track])
                level = on ? FM1_LV_GLOW : FM1_LV_LIT;
            else if (lock_blink && in_track && step_locked(t, st))
                level = on ? FM1_LV_FAINT : FM1_LV_HALF;
            else if (on)
                level = bright_group ? FM1_LV_LIT : FM1_LV_HALF;
            else
                level = in_track && bright_group ? FM1_LV_GLOW : FM1_LV_OFF;
        }
        fm1_led_level(KEY_LED(white_key[i]), level);
    }
    for (i = 0; i < 11u; i++) {
        uint32_t on;
        if (ui.pattern_mode) {
            uint32_t p = 16u + i;
            on = p == seq.playing || (p == seq.next && blink) || (p == ui.edit_pat && blink);
        } else {
            on = (i == ui.track && (!ui.dub || blink)) || (i == SHIFT_BLACK && ui_shift());
        }
        fm1_led_key(KEY_LED(black_key[i]), (int)on);
    }
    fm1_led_key(B_PLAY, keys_play);
    fm1_led_key(B_SEQ, ui.pattern_mode);
    fm1_led_key(B_REC, ui.sound_copy == SOUND_COPY_CONFIRM ? (int)blink : ui.dub || ((fm1_in.buttons >> B_REC) & 1));
    for (i = 0; i < NGROUPS; i++)
        fm1_led_key(GROUPS[i].button, !ui.pattern_mode && i == ui.group);
}

static char *put_text(char *s, const char *t)
{
    while (*t)
        *s++ = *t++;
    *s = 0;
    return s;
}
static char *put_int(char *s, int32_t v)
{
    uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    char tmp[12];
    uint32_t n = 0;
    if (v < 0)
        *s++ = '-';
    do
        tmp[n++] = (char)('0' + u % 10u);
    while (u /= 10u);
    while (n)
        *s++ = tmp[--n];
    *s = 0;
    return s;
}

/* SPI runs at 12 MHz, so a full-width text line blocks the main loop for about 6 ms. Only changed text is sent. */
enum { LINE_DUB, LINE_HEAD, LINE_COUNTER, LINE_BPM, LINE_STATUS, LINE_HINT, LINE_LABEL, LINE_VALUE = LINE_LABEL + NKNOB, LINE_COUNT = LINE_VALUE + NKNOB };
#define LINE_CHARS 48u
static struct { char text[LINE_CHARS]; uint16_t ink; } line_drawn[LINE_COUNT];

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };
static void ui_text(uint32_t slot, uint32_t x, uint32_t y, uint32_t w, const aafont_t *f, const char *s, uint16_t ink, int align)
{
    char *d = line_drawn[slot].text;
    uint32_t i = 0;
    while (d[i] == s[i] && s[i])
        i++;
    if (line_drawn[slot].ink == ink && d[i] == s[i])
        return;
    for (i = 0; s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
    line_drawn[slot].ink = ink;
    draw_text_line(x, y, w, f, s, ink, T_BG, align);
}

/* The areas share 1 px borders and fill all 240 lines, so a taller area needs a shorter one */
#define SCREEN_EDGE 239u
#define TITLE_RULE_Y 21u
#define TITLE_X 3u
#define TITLE_W 158u
#define DUB_W 27u
#define KNOB_RULE_Y 69u
#define GRID_RULE_Y 117u
#define BOTTOM_RULE_Y 222u
#define KNOB_PITCH 48u
#define KNOB_LABEL_DY 4u
#define KNOB_VALUE_DY 22u
#define FREE_X 49u
#define FREE_Y 70u
#define FREE_W 190u
#define FREE_H 47u
#define BOTTOM_Y 223u
#define GRID_Y 119u
#define GRID_ROW 17u
#define GRID_TRIG_H 11u          /* in each row: the step's square, then the playhead's lane, then the view bar */
#define GRID_PLAY_Y 12u
#define GRID_PLAY_H 2u
#define GRID_VIEW_Y 15u
#define GRID_DOT 5u
#define GRID_X 15u
#define GRID_W 224u

enum { CELL_NONE, CELL_EMPTY_A, CELL_EMPTY_B, CELL_ON, CELL_BASE = 3u, CELL_LOCKED = 4u, CELL_PLAY = 8u };
#define NO_TRACK 0xFFu
static struct {
    uint8_t cell[NTRK][NSTEP];
    uint32_t width;
    uint8_t track, bar_track, bar_first, bar_end, frame_drawn;
    uint32_t count;
} grid;

static uint8_t free_valid;

static void grid_invalidate(void)
{
    uint32_t i;
    for (i = 0; i < LINE_COUNT; i++) {
        line_drawn[i].ink = 0;
        line_drawn[i].text[0] = 0;
    }
    grid.frame_drawn = 0;
    free_valid = 0;
    grid.width = 0;
    grid.track = NO_TRACK;
    grid.bar_track = NO_TRACK;
}

static void grid_square(uint32_t x, uint32_t y, uint32_t w, uint32_t state)
{
    static const uint8_t NARROW_DOT = GRID_DOT + 2u;
    uint32_t base = state & CELL_BASE;
    uint16_t ink = base == CELL_ON ? T_THEME : base == CELL_EMPTY_A ? T_DIM : base == CELL_EMPTY_B ? ux_mix(T_DIM, T_BG, 50) : T_BG;
    lcd_fill(x, y, w, GRID_TRIG_H, ink);
    if (state & CELL_LOCKED) {                   /* a cut in the square: the palette's colours can all be equal */
        uint32_t dot_w = w >= NARROW_DOT ? GRID_DOT : w;
        lcd_fill(x + (w - dot_w) / 2u, y + (GRID_TRIG_H - GRID_DOT) / 2u, dot_w, GRID_DOT, base == CELL_ON ? T_BG : T_TEXT);
    }
}

/* a playhead move sends only the lane: it is most of what changes on each step */
static void grid_cell(uint32_t k, uint32_t i, uint32_t state, uint32_t old)
{
    uint32_t x = GRID_X + i * grid.width, y = GRID_Y + k * GRID_ROW, w = grid.width - 1u;
    if ((state ^ old) & (CELL_BASE | CELL_LOCKED))
        grid_square(x, y, w, state);
    if ((state ^ old) & CELL_PLAY)
        lcd_fill(x, y + GRID_PLAY_Y, w, GRID_PLAY_H, state & CELL_PLAY ? T_ACCENT : T_BG);
}

static void grid_bar(uint32_t k, uint32_t first, uint32_t end, uint16_t ink)
{
    lcd_fill(GRID_X + first * grid.width, GRID_Y + k * GRID_ROW + GRID_VIEW_Y, (end - first) * grid.width - 1u, 1, ink);
}

/* one row per track, at one step width so tracks of different lengths line up as they play. Only cells that changed are
 * sent: a full redraw of this area takes 25 ms, and a redraw at that cost beats against the step clock */
static void draw_grid(void)
{
    const pattern_t *pat = &patterns[ui.edit_pat];
    const track_pat_t *sel = &pat->track[ui.track];
    uint32_t cell = GRID_W / pattern_longest(pat), k, i, first, end;
    grid.count = seq.count;
    if (cell != grid.width) {
        lcd_fill(GRID_X, GRID_Y, GRID_W, GRID_ROW * NTRK, T_BG);
        grid.width = cell;
        grid.bar_track = NO_TRACK;
        for (k = 0; k < NTRK; k++)
            for (i = 0; i < NSTEP; i++)
                grid.cell[k][i] = CELL_NONE;             /* the fill above drew exactly this */
    }
    if (grid.track != ui.track) {
        for (k = 0; k < NTRK; k++) {
            char label[2] = {(char)('1' + k), 0};
            draw_text_line(1, GRID_Y + k * GRID_ROW - 1u, GRID_X - 1u, &AF_S, label, k == ui.track ? T_BG : T_MID, k == ui.track ? T_ACCENT : T_BG, 1);
        }
        grid.track = ui.track;
    }
    for (k = 0; k < NTRK; k++) {
        const track_pat_t *t = &pat->track[k];
        uint32_t cells = GRID_W / cell;
        for (i = 0; i < NSTEP && i < cells; i++) {
            uint32_t state = CELL_NONE;
            if (i < t->length) {
                state = t->steps[i].on ? CELL_ON : (i >> 2) & 1u ? CELL_EMPTY_B : CELL_EMPTY_A;
                if (step_locked(t, i))
                    state |= CELL_LOCKED;
                if (seq.playing == ui.edit_pat && seq.pos[k] == i)
                    state |= CELL_PLAY;
            }
            if (state != grid.cell[k][i]) {
                grid_cell(k, i, state, grid.cell[k][i]);
                grid.cell[k][i] = (uint8_t)state;
            }
        }
    }
    first = ui.view * 16u;                       /* the steps the white keys reach */
    end = first + 16u < sel->length ? first + 16u : sel->length;
    if (grid.bar_track != ui.track || grid.bar_first != first || grid.bar_end != end) {
        if (grid.bar_track != NO_TRACK)
            grid_bar(grid.bar_track, grid.bar_first, grid.bar_end, T_BG);
        grid_bar(ui.track, first, end, T_MID);
        grid.bar_track = ui.track;
        grid.bar_first = (uint8_t)first;
        grid.bar_end = (uint8_t)end;
    }
}

static void draw_counter(void)
{
    char b[48];
    char *s = put_int(b, seq.plays + 1);
    s = put_text(s, ".");
    put_int(s, (int32_t)seq.count + 1);
    ui_text(LINE_COUNTER, 161, 1, 38, &AF_M, b, T_TEXT, ALIGN_CENTER);
}

/* polled while the main loop waits, so the playhead and counter move on the step and not on the next frame */
static void ui_step_service(void)
{
    if (seq.count != grid.count) {
        draw_grid();
        draw_counter();
    }
}

static void value_text(char *b, uint32_t id)
{
    int32_t count;
    const char *const *names;
    b[0] = 0;
    if (id == ID_NONE)
        return;
    names = value_names(id, &count);
    if (names)
        put_text(b, names[clamp(ui_value(id), 0, count - 1)]);
    else if (id == PV_LEN && ui_value(id) == (int32_t)LEN_HOLD)
        put_text(b, "HOLD");
    else
        put_int(b, ui_value(id));
}

static void draw_frame(void)
{
    uint32_t k;
    lcd_fill(0, 0, 240, 1, T_MID);
    lcd_fill(0, TITLE_RULE_Y, 240, 1, T_MID);
    lcd_fill(0, KNOB_RULE_Y, 240, 1, T_MID);
    lcd_fill(0, GRID_RULE_Y, 240, 1, T_MID);
    lcd_fill(0, BOTTOM_RULE_Y, 240, 1, T_MID);
    lcd_fill(0, SCREEN_EDGE, 240, 1, T_MID);
    lcd_fill(0, 0, 1, 240, T_MID);
    lcd_fill(SCREEN_EDGE, 0, 1, 240, T_MID);
    for (k = 1; k < 5u; k++)
        lcd_fill(k * KNOB_PITCH, TITLE_RULE_Y, 1, KNOB_RULE_Y - TITLE_RULE_Y, T_MID);
    lcd_fill(KNOB_PITCH, KNOB_RULE_Y, 1, GRID_RULE_Y - KNOB_RULE_Y, T_MID);
}

/* each knob's square: row 1 is SEL and knobs 1 to 4, row 2 is ALGO */
static void draw_knobs(const page_t *pg)
{
    char b[48];
    uint32_t k;
    for (k = 0; k < NKNOB; k++) {
        uint32_t x = (k < 5u ? k * KNOB_PITCH : 0u) + 1u, y = (k < 5u ? TITLE_RULE_Y : KNOB_RULE_Y) + 1u;
        uint32_t w = (k == 4u ? SCREEN_EDGE : x + KNOB_PITCH - 1u) - x;
        ui_text(LINE_LABEL + k, x, y + KNOB_LABEL_DY, w, &AF_S, ui_label(pg, k), T_MID, ALIGN_CENTER);
        value_text(b, page_id(pg, k));
        ui_text(LINE_VALUE + k, x, y + KNOB_VALUE_DY, w, &AF_M, b, T_TEXT, ALIGN_CENTER);
    }
}

/* the free space. These draw on a canvas that the caller sends whole: 12 ms, so only when what it shows has changed */
static void free_copy_prompt(const track_pat_t *t)
{
    char b[48], *s;
    s = put_text(b, "COPY ");
    s = put_int(s, t->base + 1);
    s = put_text(s, " > ");
    put_int(s, ui.sound_to + 1);
    cv_text(4, 0, &AF_M, b, T_ACCENT);
    s = put_int(b, (int32_t)sound_use_count(patterns, NPATTERN, ui.sound_to));
    s = put_text(s, " TRACKS USE ");
    put_int(s, ui.sound_to + 1);
    cv_text(4, 18, &AF_S, b, T_TEXT);
    cv_text(4, 31, &AF_S, ui.sound_copy == SOUND_COPY_CONFIRM ? "REC: COPY. OTHER: CANCEL" : "RELEASE REC TO CONFIRM", T_ACCENT);
}

static void free_routes(const pkt_t *p)
{
    char b[48], *s;
    uint32_t i;
    for (i = 0; i < SLOTS; i++) {
        int32_t src = clamp(p->v[PV_SLOT_SRC0 + i], 0, MOD_SRCS - 1), dst = clamp(p->v[PV_SLOT_DST0 + i], 0, MOD_DSTS - 1);
        int32_t amt = p->v[PV_SLOT_AMT0 + i];
        s = put_int(b, (int32_t)i + 1);
        s = put_text(s, " ");
        s = put_text(s, NAMES_SRC[src]);
        s = put_text(s, ">");
        s = put_text(s, NAMES_DST[dst]);
        s = put_text(s, amt < 0 ? " " : " +");
        put_int(s, amt);
        cv_text(4 + (int32_t)(i & 1u) * 95, 8 + (int32_t)(i >> 1) * 16, &AF_S, b, i == ui.slot ? T_TEXT : T_MID);
    }
}

static void free_voices(void)
{
    static const char *const VARIANTS[COST_VARIANTS] = {"FM ", "MOD ", "STR ", "MEM ", "SYM ", "PERC ", "SWM "};
    char b[48], *s;
    uint32_t k;
    cv_text(4, 0, &AF_S, "VOICES THAT FIT", T_MID);
    for (k = 0; k < COST_VARIANTS; k++) {
        s = put_text(b, VARIANTS[k]);
        put_int(s, (int32_t)voices_fit(k));
        cv_text(4 + (int32_t)(k % 3u) * 62, 16 + (int32_t)(k / 3u) * 15, &AF_S, b, T_TEXT);
    }
}

#define CURVE_TOP 3
#define CURVE_H 41
#define CURVE_HOLD_W 30u
#define CURVE_W (FREE_W - 8u - CURVE_HOLD_W)
static int32_t curve_y(uint32_t y) { return CURVE_TOP + CURVE_H - (int32_t)y * CURVE_H / 128; }

static void free_envelope(int32_t a, int32_t d, int32_t sus, int32_t r)
{
    static uint8_t y[CURVE_W], px[CURVE_W + 4u], py[CURVE_W + 4u];
    env_curve_t c;
    uint32_t m, i;
    env_curve(&c, y, CURVE_W, a, d, sus, r);
    m = env_polyline(&c, y, CURVE_W, CURVE_HOLD_W, px, py);
    for (i = 1; i < m; i++)
        cv_line(4 + px[i - 1u], curve_y(py[i - 1u]), 4 + px[i], curve_y(py[i]), T_THEME);
}

/* two cycles, so that a shape's join shows */
static void free_lfo(int32_t shape, int32_t start)
{
    lfo_t l;
    int32_t x, py = 0;
    lfo_start(&l, shape, LFO_RETRIGGER, 0, start);
    l.inc = 0xFFFFFFFFu / ((FREE_W - 8u) / 2u);
    for (x = 0; x < (int32_t)FREE_W - 8; x++) {
        int32_t cy = CURVE_TOP + CURVE_H / 2 - lfo_block(&l, 0) * (CURVE_H / 2) / 32767;
        if (x)
            cv_line(3 + x, py, 4 + x, cy, T_THEME);
        py = cy;
    }
}

#define CHART_PITCH_X 30
#define CHART_PITCH_Y 11
#define CHART_BOX_W 22
#define CHART_BOX_H 9
#define CHART_BOTTOM 41          /* the bottom row's last line; the output line is under it */
#define FM_OP_COL(c) ((c) & 15)
#define FM_OP_ROW(c) ((c) >> 4)
static int32_t chart_y(uint32_t cell) { return CHART_BOTTOM + 1 - CHART_BOX_H - FM_OP_ROW(cell) * CHART_PITCH_Y; }

/* a 5 by 7 letter: the boxes are too short for the UI fonts. C, A or B, as the ratio knob that sets the operator */
static void chart_letter(int32_t x, int32_t y, uint32_t op, uint16_t ink)
{
    static const uint8_t LETTER[FM_OPS][7] = {
        {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
        {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
        {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
        {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    };
    int32_t row, col;
    for (row = 0; row < 7; row++)
        for (col = 0; col < 5; col++)
            if (LETTER[op][row] >> (4 - col) & 1)
                cv_rect(x + col, y + row, 1, 1, ink);
}

/* a carrier is solid and a modulator is an outline: the palette's colours can all be equal */
static void free_fm_chart(const pkt_t *p)
{
    /* operator i's place: high nibble the row above the output, low nibble the column */
    static const uint8_t CELL[FM_ALGS][FM_OPS] = {
        {0x00, 0x10, 0x20, 0x30}, {0x00, 0x10, 0x11, 0x21}, {0x01, 0x10, 0x11, 0x12}, {0x00, 0x10, 0x20, 0x21},
        {0x00, 0x10, 0x01, 0x11}, {0x00, 0x01, 0x02, 0x11}, {0x00, 0x10, 0x01, 0x02}, {0x00, 0x01, 0x02, 0x03},
    };
    uint32_t alg = fm_alg(p->v[PV_E0]), i, j, last_col = 0;
    int32_t left, width;
    for (i = 0; i < FM_OPS; i++)
        last_col = (uint32_t)FM_OP_COL(CELL[alg][i]) > last_col ? (uint32_t)FM_OP_COL(CELL[alg][i]) : last_col;
    width = (int32_t)last_col * CHART_PITCH_X + CHART_BOX_W;
    left = ((int32_t)FREE_W - width) / 2;
    for (i = 0; i < FM_OPS; i++) {
        uint32_t c = CELL[alg][i];
        int32_t x = left + FM_OP_COL(c) * CHART_PITCH_X, y = chart_y(c), carrier = fm_is_carrier(alg, i);
        cv_rect(x, y, CHART_BOX_W, CHART_BOX_H, T_THEME);
        if (!carrier)
            cv_rect(x + 1, y + 1, CHART_BOX_W - 2, CHART_BOX_H - 2, T_BG);
        chart_letter(x + (CHART_BOX_W - 5) / 2, y + 1, i, carrier ? T_BG : T_THEME);
        for (j = 0; j < FM_OPS; j++)
            if (FM_ROUTES[alg][i] >> j & 1u) {
                int32_t tx = x + CHART_BOX_W / 2, sx = left + FM_OP_COL(CELL[alg][j]) * CHART_PITCH_X + CHART_BOX_W / 2, sy = chart_y(CELL[alg][j]) + CHART_BOX_H;
                cv_line(sx, sy, sx, sy + 1, T_MID);
                cv_line(sx, sy + 1, tx, sy + 1, T_MID);
            }
    }
    {
        int32_t x = left + FM_OP_COL(CELL[alg][FM_B2]) * CHART_PITCH_X + CHART_BOX_W, y = chart_y(CELL[alg][FM_B2]);
        cv_rect(x, y + 1, 3, 1, T_MID);
        cv_rect(x + 2, y + 1, 1, CHART_BOX_H - 2, T_MID);
        cv_rect(x, y + CHART_BOX_H - 2, 3, 1, T_MID);
    }
    cv_rect(left, CHART_BOTTOM + 1, width, 1, T_MID);
}

/* one cycle of the wave with SYNC's restarts. Below it, one tick per oscillator: across for DETUNE, a row per octave for STACK,
 * height for SIDES. Under them, a bar that widens with SUB */
static void free_swarm(const pkt_t *p)
{
    enum { WAVE_TOP = 3, WAVE_H = 18, ROW_Y = 24, ROW_H = 5, TICK_H = 4, SUB_Y = 40, SUB_H = 2 };
    int32_t x, py = 0, morph, w = (int32_t)FREE_W - 8, spread = clamp(p->v[PV_E1], 0, 127), amount = spread * spread * 32767 / 16129;
    int32_t mix = clamp(p->v[PV_E4], 0, 127), sub_w = clamp(p->v[PV_E5], 0, 127) * w / 127;
    uint32_t width, cycle = 0xFFFFFFFFu / (uint32_t)w, restarts = (uint32_t)(256 + clamp(p->v[PV_E3], 0, 127) * 6), mode = swarm_mode(p->v[PV_E0]), i;
    swarm_shape(p->v[PV_E2], &morph, &width);
    for (x = 0; x < w; x++) {
        uint32_t ph = (uint32_t)(((uint64_t)((uint32_t)x * cycle) * restarts) >> 8);
        int32_t cy = WAVE_TOP + WAVE_H / 2 - clamp(swarm_wave(ph, morph, width), -65535, 65535) * (WAVE_H / 2) / 65536;
        if (x)
            cv_line(3 + x, py, 4 + x, cy, T_THEME);
        py = cy;
    }
    for (i = 0; i < SWARM_OSCS; i++) {
        int32_t h = (swarm_level(mix, i) * TICK_H + 32766) / 32767;
        int32_t row = SWARM_OCTAVE_Q8[mode][i] == 512 ? 0 : SWARM_OCTAVE_Q8[mode][i] == 256 ? 1 : 2;
        if (h)
            cv_rect((int32_t)FREE_W / 2 + ((SWARM_DETUNE[i] * amount) >> 15) * w / 2 / 7300, ROW_Y + row * ROW_H + TICK_H - h, 2, h, T_MID);
    }
    if (sub_w)
        cv_rect(((int32_t)FREE_W - sub_w) / 2, SUB_Y, sub_w, SUB_H, T_MID);
}

static void free_phys(int32_t model)
{
    static const char *const MODELS[PM_COUNT] = {"MODAL", "STRING", "MEMBRANE", "SYMPATHETIC"};
    cv_text(4, 4, &AF_S, "PHYSICAL MODEL", T_MID);
    cv_text(4, 20, &AF_M, MODELS[(uint32_t)clamp(model, 0, 127) % PM_COUNT], T_TEXT);
}

/* each partial as a bar: where it sits over the note's pitch, and how loud it starts */
static void free_perc(const pkt_t *p)
{
    static const int32_t X_SPAN = 170;           /* SPREAD at 127 reaches this, in every mode */
    int32_t level[PERC_PARTIALS], widest = (int32_t)perc_ratio_q8(p->v[PV_E0], 127, PERC_PARTIALS - 1u) - 256;
    uint32_t i;
    perc_levels(p->v[PV_E3], 32767, level);
    for (i = 0; i < PERC_PARTIALS; i++) {
        int32_t h = level[i] * CURVE_H / level[0], x = 6 + ((int32_t)perc_ratio_q8(p->v[PV_E0], p->v[PV_E2], i) - 256) * X_SPAN / widest;
        h = h < 1 ? 1 : h;
        cv_rect(x, CURVE_TOP + CURVE_H - h, 3, h, T_THEME);
    }
}

#define METER_X (FREE_X + 26u)
#define METER_W 160u
#define METER_ROW_Y(i) (FREE_Y + 3u + (i) * 11u)
#define METER_H 8u
#define METER_FALL 6u            /* px per screen draw */
static uint8_t meter_drawn[METERS];

static void free_meter_labels(void)
{
    static const char *const LABELS[METERS] = {"L", "R", "DLY", "REV"};
    uint32_t i;
    for (i = 0; i < METERS; i++) {
        cv_text(4, (int32_t)(METER_ROW_Y(i) - FREE_Y) - 4, &AF_S, LABELS[i], T_MID);
        meter_drawn[i] = 0;
    }
}

/* sends only the part of each bar that moved */
static void draw_meters(void)
{
    uint32_t i;
    for (i = 0; i < METERS; i++) {
        uint32_t peak = meter_peak[i], bar, old = meter_drawn[i];
        meter_peak[i] = 0;
        bar = meter_bar((int32_t)peak, METER_W);
        if (bar + METER_FALL < old)
            bar = old - METER_FALL;
        if (bar > old)
            lcd_fill(METER_X + old, METER_ROW_Y(i), bar - old, METER_H, T_THEME);
        else if (bar < old)
            lcd_fill(METER_X + bar, METER_ROW_Y(i), old - bar, METER_H, T_BG);
        meter_drawn[i] = (uint8_t)bar;
    }
}

static uint32_t hash_mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

/* the pages whose drawing reads the packet redraw when it changes */
/* one segment per page of the group, along the free space's lower edge. The shown page's is solid and the others are a line */
static void free_page_tabs(const page_t *pg)
{
    enum { TAB_Y = 44, TAB_H = 3, TAB_GAP = 4 };
    const page_group_t *g = &GROUPS[ui.group];
    uint32_t shown = ui.group_page[ui.group], i;
    int32_t w;
    if (g->n < 2u || pg != &PAGES[g->page[shown]])
        return;
    w = ((int32_t)FREE_W - 8 - TAB_GAP * ((int32_t)g->n - 1)) / (int32_t)g->n;
    for (i = 0; i < g->n; i++) {
        int32_t x = 4 + (int32_t)i * (w + TAB_GAP);
        if (i == shown)
            cv_rect(x, TAB_Y, w, TAB_H, T_THEME);
        else
            cv_rect(x, TAB_Y + TAB_H - 1, w, 1, T_MID);
    }
}

static int free_reads_packet(const page_t *pg)
{
    return pg == &PAGES[PG_AMP] || pg == &PAGES[PG_MOD] || pg == &PAGES[PG_LFO1] || pg == &PAGES[PG_LFO2] || pg == &PAGES[PG_SLOTS] || pg == &PAGES[PG_E1] || pg == &PAGES[PG_E2];
}

static void draw_free(const page_t *pg, const track_pat_t *t)
{
    static uint32_t drawn_key;
    pkt_t p;
    uint32_t key = 2166136261u, i;
    ui_packet(&p);
    key = hash_mix(key, (uint32_t)(pg - PAGES));
    key = hash_mix(key, ui.sound_copy);
    if (ui.sound_copy != SOUND_COPY_NONE) {
        key = hash_mix(key, t->base);
        key = hash_mix(key, ui.sound_to);
    } else if (free_reads_packet(pg)) {
        key = hash_mix(key, ui.slot);
        for (i = 0; i < PV_COUNT; i++)
            key = hash_mix(key, (uint8_t)p.v[i]);
    }
    if (!free_valid || key != drawn_key) {
        cv_begin(FREE_W, FREE_H, T_BG);
        if (ui.sound_copy != SOUND_COPY_NONE)
            free_copy_prompt(t);
        else if (pg == &PAGES[PG_HOME])
            free_meter_labels();
        else if (pg == &PAGES[PG_AMP])
            free_envelope(p.v[PV_AMP_A], p.v[PV_AMP_D], p.v[PV_AMP_S], p.v[PV_AMP_R]);
        else if (pg == &PAGES[PG_MOD])
            free_envelope(p.v[PV_MOD_A], p.v[PV_MOD_D], p.v[PV_MOD_S], p.v[PV_MOD_R]);
        else if (pg == &PAGES[PG_LFO1])
            free_lfo(p.v[PV_LFO1_SHAPE], p.v[PV_LFO1_START]);
        else if (pg == &PAGES[PG_LFO2])
            free_lfo(p.v[PV_LFO2_SHAPE], p.v[PV_LFO2_START]);
        else if (pg == &PAGES[PG_SLOTS])
            free_routes(&p);
        else if ((pg == &PAGES[PG_E1] || pg == &PAGES[PG_E2]) && clamp(p.v[PV_ENGINE], 0, ENGINES - 1) == ENGINE_SWARM)
            free_swarm(&p);
        else if ((pg == &PAGES[PG_E1] || pg == &PAGES[PG_E2]) && clamp(p.v[PV_ENGINE], 0, ENGINES - 1) == ENGINE_PERC)
            free_perc(&p);
        else if ((pg == &PAGES[PG_E1] || pg == &PAGES[PG_E2]) && clamp(p.v[PV_ENGINE], 0, ENGINES - 1) == ENGINE_PHYS)
            free_phys(p.v[PV_E0]);
        else if (pg == &PAGES[PG_E1] || pg == &PAGES[PG_E2])
            free_fm_chart(&p);
        else if (pg == &PAGES[PG_GLOBAL])
            free_voices();
        if (ui.sound_copy == SOUND_COPY_NONE)
            free_page_tabs(pg);
        cv_blit(FREE_X, FREE_Y);
        lcd_sync();
        drawn_key = key;
        free_valid = 1;
    }
    if (pg == &PAGES[PG_HOME] && ui.sound_copy == SOUND_COPY_NONE)
        draw_meters();
}

/* inverts in time with the flashing track key */
static void draw_dub_word(void)
{
    uint16_t ink = ui_blink() ? T_BG : T_TEXT;
    if (line_drawn[LINE_DUB].text[0] && line_drawn[LINE_DUB].ink == ink)
        return;
    line_drawn[LINE_DUB].text[0] = 'D';
    line_drawn[LINE_DUB].ink = ink;
    draw_text_line(TITLE_X, 1, DUB_W, &AF_M, "DUB", ink, ink == T_BG ? T_TEXT : T_BG, ALIGN_CENTER);
}

static void draw_title(const page_t *pg, const track_pat_t *t)
{
    char b[48], *s;
    uint32_t head_x = ui.dub ? TITLE_X + DUB_W + 2u : TITLE_X;
    if (ui.dub)
        draw_dub_word();
    else
        line_drawn[LINE_DUB].text[0] = 0;
    s = put_text(b, ui.dub ? "" : ui.pattern_mode ? "PATT " : keys_play ? "PLAY " : "COMP ");
    s = put_text(s, pg->name);
    s = put_text(s, " T");
    s = put_int(s, ui.track + 1);
    s = put_text(s, " S");
    s = put_int(s, t->base + 1);
    s = put_text(s, " P");
    s = put_int(s, ui.edit_pat + 1);
    if (ui.edit_pat != seq.playing) {
        s = put_text(s, ">");
        put_int(s, seq.playing + 1);
    }
    ui_text(LINE_HEAD, head_x, 1, TITLE_X + TITLE_W - head_x, &AF_M, b, T_TEXT, ALIGN_LEFT);
    draw_counter();
    put_int(b, (int32_t)ui.bpm);
    ui_text(LINE_BPM, 199, 1, 38, &AF_M, b, T_TEXT, ALIGN_RIGHT);
}

/* the held step's values, or else the load and how often triggers lost out. The counts wrap: a change is the news */
static void draw_status(void)
{
    char b[48], *s;
    uint32_t k, others = 0;
    if (ui.dub && dub.full) {
        put_text(b, "LOCKS FULL");
    } else if (held_first) {
        const step_t *st = &held_first->t->steps[held_first->step];
        for (k = 0; k < NELEM(held); k++)
            others += held[k].t != 0;
        s = put_text(b, "STEP ");
        s = put_int(s, held_first->step + 1);
        if (others > 1u) {
            s = put_text(s, " +");
            s = put_int(s, (int32_t)others - 1);
        }
        if (st->sound != SOUND_NONE) {
            s = put_text(s, " SND ");
            s = put_int(s, st->sound + 1);
        }
        if (step_lock_count(held_first->t, held_first->step)) {
            s = put_text(s, " LOCKS ");
            put_int(s, (int32_t)step_lock_count(held_first->t, held_first->step));
        }
    } else {
        s = put_text(b, "CPU");
        s = put_int(s, (int32_t)(audio_cpu_q8 * 100u / 256u));
        s = put_text(s, " V");
        s = put_int(s, (int32_t)voices_active());
        s = put_text(s, " L");
        s = put_int(s, (int32_t)(audio_late % 100u));
        s = put_text(s, " D");
        s = put_int(s, (int32_t)(voices_dropped % 100u));
        s = put_text(s, " S");
        put_int(s, (int32_t)(voices_stolen % 100u));
    }
    ui_text(LINE_STATUS, 96, BOTTOM_Y, 141, &AF_S, b, held_first || (ui.dub && dub.full) ? T_ACCENT : T_MID, ALIGN_RIGHT);
}

static void draw_hint(const track_pat_t *t)
{
    char b[48], *s = put_text(b, "OCT: ");
    if (keys_play) {
        s = put_text(s, keys_octave > 0 ? "OCTAVE +" : "OCTAVE ");
        put_int(s, keys_octave);
    } else {
        uint32_t first = ui.view * 16u, end = first + 16u < t->length ? first + 16u : t->length;
        s = put_text(s, "STEPS ");
        s = put_int(s, (int32_t)first + 1);
        s = put_text(s, "-");
        put_int(s, (int32_t)end);
    }
    ui_text(LINE_HINT, 3, BOTTOM_Y, 93, &AF_S, b, T_MID, ALIGN_LEFT);
}

static void ui_draw(void)
{
    const track_pat_t *t = ui_track();
    const page_t *pg = ui_page();
    if (!grid.frame_drawn) {
        draw_frame();
        grid.frame_drawn = 1;
    }
    draw_title(pg, t);
    draw_grid();
    draw_knobs(pg);
    draw_free(pg, t);
    draw_status();
    draw_hint(t);
}

static void sound_phys(sound_t *s, int32_t model, int32_t e1, int32_t e2, int32_t e3, int32_t e4, int32_t e5, int32_t e6, int32_t e7)
{
    s->v[PV_ENGINE] = ENGINE_PHYS;
    s->v[PV_E0] = (int8_t)model;
    s->v[PV_E1] = (int8_t)e1;
    s->v[PV_E2] = (int8_t)e2;
    s->v[PV_E3] = (int8_t)e3;
    s->v[PV_E4] = (int8_t)e4;
    s->v[PV_E5] = (int8_t)e5;
    s->v[PV_E6] = (int8_t)e6;
    s->v[PV_E7] = (int8_t)e7;
    s->v[PV_AMP_R] = 116;
    s->v[PV_NOTE] = 48;
}

/* ratios: knob values. mod_d, mod_s: the MOD envelope, which sets how the index moves */
static void sound_fm(sound_t *s, int32_t alg, int32_t note, int32_t ratio_c, int32_t ratio_a, int32_t ratio_b, int32_t index_a, int32_t index_b, int32_t feedback, int32_t detune, int32_t mod_d, int32_t mod_s)
{
    s->v[PV_ENGINE] = ENGINE_FM;
    s->v[PV_E0] = (int8_t)alg;
    s->v[PV_NOTE] = (int8_t)note;
    s->v[PV_E1] = (int8_t)ratio_c;
    s->v[PV_E2] = (int8_t)ratio_a;
    s->v[PV_E3] = (int8_t)ratio_b;
    s->v[PV_E4] = (int8_t)index_a;
    s->v[PV_E5] = (int8_t)index_b;
    s->v[PV_E6] = (int8_t)feedback;
    s->v[PV_E7] = (int8_t)detune;
    s->v[PV_MOD_D] = (int8_t)mod_d;
    s->v[PV_MOD_S] = (int8_t)mod_s;
}

static void sound_swarm(sound_t *s, int32_t mode, int32_t note, int32_t spread, int32_t shape, int32_t shift, int32_t mix, int32_t sub, int32_t drift)
{
    s->v[PV_ENGINE] = ENGINE_SWARM;
    s->v[PV_E0] = (int8_t)mode;
    s->v[PV_NOTE] = (int8_t)note;
    s->v[PV_E1] = (int8_t)spread;
    s->v[PV_E2] = (int8_t)shape;
    s->v[PV_E3] = (int8_t)shift;
    s->v[PV_E4] = (int8_t)mix;
    s->v[PV_E5] = (int8_t)sub;
    s->v[PV_E6] = (int8_t)drift;
    s->v[PV_E7] = 0;
}

/* a note length that lets a decay finish at the starting tempo, so AMP R does not cut it */
static int8_t starter_len(int32_t decay) { return (int8_t)clamp((int32_t)(env_blocks(decay) * CTL / (FS * 15u / 120u)) + 1, 1, NSTEP); }

/* it rings for its decay while the note lasts; at the note's end AMP R takes over */
static void sound_perc(sound_t *s, int32_t mode, int32_t note, int32_t decay, int32_t fm, int32_t spread, int32_t harm, int32_t fold, int32_t bend, int32_t noise, int32_t click)
{
    s->v[PV_ENGINE] = ENGINE_PERC;
    s->v[PV_E0] = (int8_t)mode;
    s->v[PV_NOTE] = (int8_t)note;
    s->v[PV_AMP_D] = (int8_t)decay;
    s->v[PV_AMP_S] = 0;
    s->v[PV_LEN] = starter_len(decay);
    s->v[PV_E1] = (int8_t)fm;
    s->v[PV_E2] = (int8_t)spread;
    s->v[PV_E3] = (int8_t)harm;
    s->v[PV_SHAPER] = (int8_t)fold;
    s->v[PV_E4] = (int8_t)bend;
    s->v[PV_E5] = (int8_t)noise;
    s->v[PV_E6] = (int8_t)click;
    s->v[PV_E7] = 0;
}

static void ui_init(void)
{
    uint32_t i, k;
    keys_map_init();
    for (i = 0; i < 16u; i++)
        white_keys |= 1u << white_key[i];
    keys_mask = white_keys;
    fm1_led_dim_level(0);
    for (i = 0; i < FM1_NCOL; i++)
        fm1_led_dim[i] = 0x1Eu;                  /* LED rows are bits 1..4: every LED glows when it is not lit */
    for (i = 0; i < NSOUND; i++)
        sound_init(&sounds[i], i % FM_ALGS);
    for (i = 0; i < NPATTERN; i++)
        for (k = 0; k < NTRK; k++)
            track_init(&patterns[i].track[k], k);
    globals[G_DELAY_TIME] = 3;
    globals[G_DELAY_FB] = 50;
    globals[G_REVERB_SIZE] = 80;
    globals[G_REVERB_DAMP] = 40;
    sound_phys(&sounds[3], PM_MEMB, 0, 52, 40, 30, 100, 40, 30);     /* toms */
    sound_phys(&sounds[4], PM_MEMB, 112, 76, 56, 44, 100, 14, 24);   /* hand drum */
    sound_phys(&sounds[5], PM_STRING, 56, 72, 84, 40, 96, 0, 0);     /* pluck */
    sound_perc(&sounds[0], PERC_SKIN, 36, 82, 0, 0, 0, 10, 50, 0, 127);       /* kick */
    sound_perc(&sounds[1], PERC_SKIN, 50, 70, 0, 64, 70, 20, 15, 70, 127);     /* snare */
    sound_perc(&sounds[2], PERC_METAL, 72, 50, 60, 64, 110, 40, 0, 60, 127);   /* hat */
    sounds[2].v[PV_FLT_TYPE] = FLT_HP;
    sounds[2].v[PV_FLT_CUT] = 90;
    sound_perc(&sounds[6], PERC_METAL, 60, 90, 60, 64, 100, 50, 5, 5, 117);    /* metal hit */
    sound_fm(&sounds[7], 1, 36, 2, 2, 3, 70, 40, 0, 0, 70, 0);        /* bass */
    sound_fm(&sounds[8], 4, 72, 2, 11, 6, 70, 40, 0, 30, 100, 0);     /* bell */
    sounds[8].v[PV_AMP_D] = 110;
    sounds[8].v[PV_AMP_S] = 0;
    sounds[8].v[PV_LEN] = starter_len(110);
    sound_fm(&sounds[9], 0, 48, 2, 6, 9, 90, 90, 40, 50, 80, 0);      /* metal clang */
    sounds[9].v[PV_AMP_D] = 90;
    sounds[9].v[PV_AMP_S] = 0;
    sounds[9].v[PV_LEN] = starter_len(90);
    sound_fm(&sounds[10], 3, 36, 2, 2, 3, 80, 70, 90, 0, 90, 60);     /* growl */
    sound_fm(&sounds[11], 7, 60, 2, 3, 5, 80, 60, 0, 0, 64, 127);     /* organ */
    sound_swarm(&sounds[12], SWARM_UNISON, 36, 50, 0, 0, 90, 60, 70);        /* drone */
    sounds[12].v[PV_LEN] = (int8_t)LEN_HOLD;
    sounds[12].v[PV_AMP_A] = 95;
    sounds[12].v[PV_AMP_R] = 110;
    sound_swarm(&sounds[13], SWARM_UNISON, 36, 40, 10, 0, 70, 40, 10);       /* big bass */
    sound_swarm(&sounds[14], SWARM_OCTAVES, 48, 40, 80, 0, 80, 0, 60);       /* pulse pad */
    sounds[14].v[PV_AMP_A] = 85;
    sounds[14].v[PV_AMP_R] = 105;
    sounds[14].v[PV_LFO1_RATE] = 30;
    sounds[14].v[PV_SLOT_DST1] = MOD_DST_TIMBRE;
    sounds[14].v[PV_SLOT_AMT1] = 30;
    sound_swarm(&sounds[15], SWARM_UNISON, 48, 35, 0, 70, 60, 0, 10);        /* sync lead */
    ui.bpm = 120;
    ui.copy_src = NO_KEY;
    ui.track_key = NO_KEY;
    seq_init(&seq, ui.bpm);
    grid_invalidate();
}
