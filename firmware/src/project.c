/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Projects: four slots. The slots live in .noinit RAM: they
 * survive resets and UBOOT entry. With FELUCCA_FLASH (default) every save
 * also goes to flash through storage.c, and an
 * empty RAM slot is filled from flash on load.
 *
 * This firmware reads SKF3 and SKF4 (below). A record of any other format reads as an empty slot.
 *
 * Built on the Mac too (tests/project_test.c, -DPROJ_HOST): the part above the #ifndef
 * PROJ_HOST needs core.h, params.c (TP) and engines.c. */
#define PROJ_MAGIC 0x534B4631u                 /* (Skiff) "SKF1": FUN9 + the steps' NUDGE (see the top) */
#define PROJ_PHYS 2u                           /* project_t.phys: PHYS without DUST and DRUM (see the top) */
#define PROJ_NT 4u                             /* (Skiff phase 6) the tracks every format before SKF3 holds (FUN1's one
                                                * aside): their readers and writers keep to them; a track past them
                                                * loads as at power-on (proj_power_on) */
#define PROJ_PARTS 4u                          /* project_t.parts as every format since 1.0 writes it (0: track 4 is the
                                                * old GM drum part, PROJ_DRUMT) */
#define PROJ_DRUMT 3u
#define PROJ_NAME_LEN 12u                      /* the name: FUN7 bytes PROJ_NAME_OFF.. (the reserved tail's end) */
typedef struct {                               /* one track */
    int16_t p[P_COUNT];
    uint8_t engine, preset;
    uint8_t user;                              /* ((2.0, #20) SKF4: the user sound it came from, slot + 1; 0 none) */
} proj_trk_t;
typedef struct {
    uint32_t magic, size;
    int16_t g[G_COUNT];
    uint8_t sel;                               /* the selected track */
    uint8_t parts;                             /* PROJ_PARTS; 0: track 4 is the old GM drum part (see the top) */
    uint8_t phys;                              /* PROJ_PHYS: PHYS MODEL values as today; 1: MODEL 4 was DRUM;
                                                * 0 (a reserved byte before 1.0): MODEL 2 was DUST */
    uint8_t rsv;
    proj_trk_t t[NTRK];
    uint8_t fm6[NTRK][FM6_PACKED];             /* each track's FM6 patch, packed (eng_fm6.c) */
    char name[PROJ_NAME_LEN];                  /* the project's name: upper-case ASCII 32..126, 0-padded; "" = none */
    uint8_t ent[NTRK][NENT][2];                /* (SKF2) each track's list: pattern id + 1 (0: none), repeats - 1 */
    uint8_t snd[NTRK][NENT][2];                /* ((2.0, #30) SKF4) each entry's sound (ui.c lists.snd) */
    uint8_t cur[NTRK];                         /* (SKF2) the entry each track is on */
    uint8_t song;                              /* (SKF2) SONG on */
    uint8_t scene[PROJ_SCENES];                /* (SKF2) the scenes (bank.c) */
    uint8_t dx_on;                             /* (Skiff phase 7) 1: dx is the project's DRUM-X kit; 0 none (the factory's) */
    dx_lane_t dx[DV_NLANE];                    /* (SKF3) the DRUM-X kit (drumx_voice.c dx_kit), one for every DRUM-X track */
    uint32_t sum;
} project_t;
/* Serialized FUN7 keeps the retained cache's exact extent. Params are biased
 * bytes, steps pack n/time/flags. Reserved tail is zero and covered by hash.
 * The tail's last 12 bytes (PROJ_NAME_OFF, just before the hash) are the project's name since 1.0:
 * ASCII 32..126 (upper case), 0-padded, all 0 = no name ("PROJECT A"). Firmware before wrote them 0 and
 * never reads them, so every FUN7 file stays valid both ways; FUN6..FUN1 imports get no name.
 * FUN8: the same, 3584 bytes, the four packed FM6 patches at PROJ_FM6_OFF (before the name); 16 bytes of the
 * reserved tail were left for parameters added later. FUN9: the same, 3648 bytes (see the top). */
#define PROJ_STORE_SIZE 3840u                  /* (Skiff) SKF1 */
#define PROJ_NUDGE_BYTES (NSTEP / 4u * 3u)     /* (Skiff) a track's NUDGE plane */
#define PROJ_NAME_OFF (PROJ_STORE_SIZE - 4u - PROJ_NAME_LEN)
#define PROJ_FM6_OFF (PROJ_NAME_OFF - PROJ_NT * FM6_PACKED)
/* (Skiff phase 5) "SKF2": a project without steps; the patterns are the pool's (bank.c), a project names them. 1280
 * bytes: 0..67 SKF1's header (65: SONG on), from 68 each track's np parameters (value + 64), engine and preset, then
 * the lists (PROJ_NT x NENT x (pattern id + 1, 0 none; repeats - 1)), the entry each track is on, the scenes
 * (PROJ_SC2 bytes); the FM6 patches at 752 (PROJ_FM6_AT), the name, the hash. Room for np up to 123 */
/* (Skiff phase 6) "SKF3": SKF2 for six tracks and their scenes, 2048 bytes, the same codec (proj_fmt_t: a format's
 * magic, size, tracks, scene bytes): 68..697 the six tracks' parameters (np 103), engines and presets, 698..889 the
 * lists, 890..895 the entries, 896..977 the scenes; (phase 7) the DRUM-X kit at 1175 (PROJ_DX_AT: PROJ_DX_MARK, then
 * its 8 lanes, 11 bytes each, drumx_voice.c dx_lane_t; a record of before, 0 there: the factory kit), the FM6 patches
 * at 1264, the name at 2032, the hash. SKF2 holds no kit.
 * (Skiff phase 9) Every format stores PROJ_NP parameters a track, the layout before phase 9 (P_COUNT 103), by count:
 * the first PROJ_NP - 8 ids in order, then P_E0..P_E7 (proj_np_put; params_by_count reads them), so firmware before,
 * which refuses a count above its own, reads what this one writes. The parameters added since (PROJ_NX of them, P_DTYPE
 * .. P_LDECAY7) are SKF3's PX block, before the kit (PROJ_PX_AT, 981..1174: PROJ_PX_MARK, their count n, then each
 * track's n values + 64), written only when one is not its default: a project that uses none is the bytes it was, and
 * one without the block (every one of before) reads them as the defaults. Firmware before reads it without them. SKF1
 * and SKF2 hold none: only old records are written as them again (proj_fetch, a rename, a backup), their defaults */
#define PROJ_MAGIC3 0x534B4633u
#define PROJ_STORE3 2048u
typedef struct { uint32_t magic, size, nt, sc, dx; } proj_fmt_t;   /* (dx: 1 = it holds the DRUM-X kit) */
static const proj_fmt_t PROJ_SKF3 = {PROJ_MAGIC3, PROJ_STORE3, 6u, PROJ_SCENES, 1u};
/* (Skiff 2.0, Tier B: design/TIER_B.md 5) "SKF4": 2560 bytes, the parameters by stored id (core.h sid_put): one added
 * or moved never shifts the others. 8 the layout (PROJ_LAYOUT4; another is not one this firmware reads), 9 flags (0);
 * from 10 (PROJ_ENT4) the lists, 4 bytes an entry (pattern id + 1 or PAT_REST, the REPS byte, the entry's sound: #30's,
 * 0 0 none, engine + 1 and its preset, 0x80 + a user slot and 0), the entry each track is on, the scenes, sel, SONG,
 * the DRUM-X kit (PROJ_DX_MARK
 * and its lanes; 0: the factory's), the FM6 patches; then the globals by GSID and each track's engine, preset, user
 * slot + 1 (#20's, 0 none) and parameters by SID, the counts the record's (a newer one's past ours passed over, an
 * older one's missing at their defaults); the name, the hash last. Room for about 50 more parameters a track */
#define PROJ_MAGIC4 0x534B4634u
#define PROJ_STORE4 2560u
#define PROJ_LAYOUT4 1u
#define PROJ_ENT4 10u
static const proj_fmt_t PROJ_SKF4 = {PROJ_MAGIC4, PROJ_STORE4, 6u, PROJ_SCENES, 1u};
_Static_assert(PROJ_ENT4 + NTRK * (NENT * 4u + 1u) + PROJ_SCENES + 2u + 1u + DV_NLANE * 11u + NTRK * FM6_PACKED +
               1u + 2u * GSID_COUNT + NTRK * (4u + SID_COUNT) + PROJ_NAME_LEN + 4u <= PROJ_STORE4, "SKF4 holds it all");
#define PROJ_SKF (&PROJ_SKF4)                  /* the format a save writes */
#define PROJ_NAME_AT(f) ((f)->size - 4u - PROJ_NAME_LEN)
#define PROJ_FM6_AT(f) (PROJ_NAME_AT(f) - (f)->nt * FM6_PACKED)
#define PROJ_DX_MARK 0x58u                    /* ('X') the kit follows */
#define PROJ_DX_AT(f) (PROJ_FM6_AT(f) - 1u - DV_NLANE * sizeof(dx_lane_t))   /* (a dx format's) */
#define PROJ_NP 103u                          /* (phase 9) the parameters a track a store holds (see above) */
#define PROJ_NX 32u                           /* .. the ones after them, in the PX block: P_DTYPE .. P_ESYNC ((2.0, #36)
                                               * frozen: the lane pans after them are SKF4's alone) */
#define PROJ_ESHIFT (P_E0 - (PROJ_NP - 8u))   /* a store of PROJ_NP: its engine values' ids + this are today's */
#define PROJ_PX_MAX 32u                       /* .. its room: so many a track */
#define PROJ_PX_MARK 0x50u                    /* ('P') the PX block follows */
#define PROJ_PX_AT(f) (PROJ_DX_AT(f) - 2u - (f)->nt * PROJ_PX_MAX)   /* (a dx format's) */
#define PROJ_HEAD_END(f) ((f)->dx ? PROJ_PX_AT(f) : PROJ_FM6_AT(f))      /* where the tracks, lists and scenes end */
_Static_assert(sizeof(dx_lane_t) == 11u, "a DRUM-X lane is 11 bytes in SKF3");
_Static_assert(NTRK == 6u, "SKF3: six tracks");
_Static_assert(P_COUNT >= PROJ_NP && PROJ_NX <= PROJ_PX_MAX, "the parameters past PROJ_NP fit the PX block");
_Static_assert(68u + 6u * (PROJ_NP + 2u + NENT * 2u + 1u) + PROJ_SCENES <=
               PROJ_STORE3 - 4u - PROJ_NAME_LEN - 6u * FM6_PACKED - 1u - DV_NLANE * 11u - 2u - 6u * PROJ_PX_MAX,
               "SKF3's tracks, lists and scenes end before its PX block");
typedef union { uint32_t align; uint8_t raw[PROJ_STORE4]; } project_store_t;   /* SKF4's, the largest record it reads */
_Static_assert(G_COUNT == 27u, "FUN7 globals retain original IDs");
#define PROJ_DEF_SOUND 0xFFu                   /* preset byte: the track's power-on sound, no steps (format 1) */
#define PROJ_DEF_KEEP 0xFEu                    /* .. DRUM's kit (once SAMPLE PERC), steps and the rest kept (old drums) */
project_store_t proj_slot[4] __attribute__((section(".noinit")));


static uint32_t proj_hash(const void *p, uint32_t n)   /* FNV-1a over n bytes */
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t i, s = 0x811C9DC5u;
    for (i = 0; i < n; i++)
        s = (s ^ b[i]) * 16777619u;
    return s;
}
static uint32_t proj_sum(const project_t *p) { return proj_hash(p, sizeof *p - 4u); }
static int proj_ok(const project_t *q) { return q->magic == PROJ_MAGIC && q->size == sizeof *q &&
    q->sum == proj_sum(q)
    ; }


/* (Skiff phase 6) tracks from .. NTRK - 1 as at power-on, for a format holding fewer: the engine's defaults, its
 * power-on sound (PROJ_DEF_SOUND: project_restore_runtime loads it), no steps, the init patch, no list */
static void proj_power_on_trk(project_t *q, uint32_t i)
{
    uint32_t k;
    for (k = 0; k < P_COUNT; k++)
        q->t[i].p[k] = param_desc_of(trk_def_engine(i), k)->def;
    q->t[i].engine = (uint8_t)trk_def_engine(i);
    q->t[i].preset = PROJ_DEF_SOUND;
    memcpy(q->fm6[i], FM6_INIT, FM6_PACKED);
}
static void proj_power_on(project_t *q, uint32_t from)
{
    uint32_t i;
    for (i = from; i < NTRK; i++)
        proj_power_on_trk(q, i);
}
static int proj_import_any(project_t *q, const void *b, int n);
static const proj_fmt_t *proj_fmt_of(const void *b, int n);
/* every track's engine one this firmware has: a project of another number is not one */
static int proj_engines_ok(const project_t *q)
{
    uint32_t t;
    for (t = 0; t < NTRK; t++)
        if (q->t[t].engine >= NENGINES || (!FELUCCA_FM4 && q->t[t].engine == ENGI_DIGITAL))
            return 0;
    return 1;
}
/* n bytes of a stored project (any format) -> q as today's, DIGITAL and SAMPLE PERC tracks converted; 0 = not a
 * project */
static int proj_import(project_t *q, const void *b, int n)
{
    uint32_t k;
    if (!proj_import_any(q, b, n) || !proj_engines_ok(q))
        return 0;
    for (k = NHEAVY; k < NTRK; k++)                /* (Skiff phase 6) a light track holding PHYS or GRAIN (no firmware */
        if (!eng_allowed(k, q->t[k].engine)) {     /* writes one): as at power-on, its list kept */
            proj_power_on_trk(q, k);
            q->sum = proj_sum(q);
        }
    return 1;
}
static int proj_unpack2(project_t *q, const uint8_t *b, const proj_fmt_t *f);
/* the format of a record of n bytes if SKF3 or SKF4 (a retained slot too: its size in it); 0 not */
static const proj_fmt_t *proj_fmt_of(const void *b, int n)
{
    const uint32_t *w = (const uint32_t *)b;
    const proj_fmt_t *f = n < 8 ? 0 : w[0] == PROJ_MAGIC4 ? &PROJ_SKF4 : w[0] == PROJ_MAGIC3 ? &PROJ_SKF3 : 0;
    return f && n >= (int)f->size && w[1] == f->size ? f : 0;
}
static int proj_import_any(project_t *q, const void *b, int n)
{
    const proj_fmt_t *f = proj_fmt_of(b, n);
    if (f)
        return proj_unpack2(q, b, f);              /* (Skiff phase 5, 6) */
    return 0;
}

/* 12 stored name bytes -> d (PROJ_NAME_LEN + 1): up to the first 0, upper case; a byte outside 32..126 makes it
 * no name (a damaged tail never refuses the project) */
static uint32_t proj_name_get(char *d, const uint8_t *s)   /* its length */
{
    uint32_t i;
    for (i = 0; i < PROJ_NAME_LEN && s[i]; i++) {
        if (s[i] < 32u || s[i] > 126u) { i = 0; break; }
        d[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32u : s[i]);
    }
    d[i] = 0;
    return i;
}

/* a step as FUN7..FUN9 / SKF1 store it, 9 bytes: its four notes (bit 7 of each: (Skiff) a bit of the trig condition),
 * the meta byte (n, time, accent, slide; bit 7 (Skiff) FLAM), velocity, the lane hits and their accents, the chance;
 * RATCH in bit 7 of the velocity and chance bytes (see the top). What step_get9 checks, so what is stored always
 * loads: 0 for a step no format can hold. (Skiff: the pattern bank, bank.c, stores steps the same way) */
static int step_put9(uint8_t *b, const step_t *s)
{
    uint32_t r = step_ratchet(s) - 1u, j;
    if (s->n > 4u || s->time > ST_REST || (s->flags & ~(3u | SF_RATCH | SF_FLAM)) || s->probability > 101u ||
        s->cond >= TC_COUNT || s->nudge < NUDGE_MIN || s->nudge > NUDGE_MAX)
        return 0;
    for (j = 0; j < 4u; j++)
        b[j] = (uint8_t)((s->note[j] > 127u ? 127u : s->note[j]) | ((s->cond >> j) & 1u) << 7);
    b[4] = (uint8_t)(s->n | s->time << 3 | (s->flags & 3u) << 5 | ((s->flags & SF_FLAM) ? 128u : 0u));
    b[5] = (uint8_t)((s->vel > 127u ? 127u : s->vel) | (r & 1u) << 7);
    b[6] = s->hit;
    b[7] = (uint8_t)(s->hit ? s->acc & s->hit : step_ties(s));   /* ((2.0, #25) no hits: the per-note ties) */
    b[8] = (uint8_t)(s->probability | (r >> 1) << 7);
    return 1;
}
/* 9 stored bytes -> *s (its nudge untouched): 0 for bytes no firmware wrote (FLAM with RATCH x2.., n above 4, ..) */
static int step_get9(step_t *s, const uint8_t *b)
{
    uint32_t meta = b[4], j;
    memcpy(s->note, b, 4);
    s->n = meta & 7u; s->time = (meta >> 3) & 3u; s->flags = (meta >> 5) & 3u;
    s->vel = b[5] & 127u; meta |= (b[5] & 128u) << 1u;   /* (RATCH bit 0 at bit 8 of meta) */
    s->hit = b[6]; s->acc = b[7];
    s->probability = b[8] & 127u; meta |= (b[8] & 128u) << 2u;   /* (bit 1 at bit 9) */
    if (((meta & 128u) && (meta >> 8)) || s->n > 4u || s->time > ST_REST || s->probability > 101u)
        return 0;                                       /* ((Skiff) FLAM: bit 7 of meta, with RATCH x1 only) */
    s->cond = 0;                                        /* (Skiff) the trig condition: bit 7 of the note bytes */
    for (j = 0; j < 4u; j++) {
        s->cond |= (uint8_t)((s->note[j] >> 7) << j);
        s->note[j] &= 127u;
    }
    if (s->cond >= TC_COUNT || (s->acc & ~(s->hit ? s->hit : (1u << s->n) - 1u)))   /* ((2.0) no hits: ties of its */
        return 0;                                                                   /* notes; 1.4 refuses them) */
    step_set_ratchet(s, (meta >> 8) + 1u);
    if (meta & 128u)
        s->flags |= SF_FLAM;
    return 1;
}


/* (Skiff phase 9) a track's values p -> the PROJ_NP bytes a store holds (+ 64, by count: the first PROJ_NP - 8 ids,
 * then P_E0..P_E7; see PROJ_NP): 0 for a value no format holds */
static int proj_np_put(uint8_t *b, const int16_t *p)
{
    uint32_t i;
    for (i = 0; i < PROJ_NP; i++) {
        int32_t v = p[i < PROJ_NP - 8u ? i : P_E0 + i - (PROJ_NP - 8u)];
        if (v < -64 || v > 127) return 0;
        b[i] = (uint8_t)(v + 64);
    }
    return 1;
}
/* the motion of a FUN7 / FUN8 written with np parameters (np < P_COUNT: a FUN7 of 89 before the chord keys, every
 * FUN8 of 91 before the DRUM lane levels) -> today's ids: an event names a parameter id, and the ids from that
 * store's P_E0 (np - 8) on moved up with P_E0, as its values did (params_by_count); the ones below kept theirs. A
 * lock stays a lock: the id moves (a FUN8 / FUN7 of a 1.1 development build may hold
 * locks). 0: an id that store could not have named (np or more) */
static int proj_motion_ids(motion_store_t *m, uint32_t np)
{
    uint32_t i;
    for (i = 0; i < m->count && i < MOTION_MAX; i++) {
        uint32_t id = MOTION_ID(&m->event[i]);
        if (id >= np) return 0;
        if (np < P_COUNT && id >= np - 8u)
            m->event[i].id = (uint8_t)(id + P_COUNT - np);
    }
    return 1;
}

/* (Skiff phase 5) the lists: id + 1 up to the pool (0 none; (#28) PAT_REST a REST: firmware before refuses a project
 * holding one), repeats - 1 up to 15 ((2.0) or REPS_CHAIN), each track on one of its entries, SONG 0 / 1 ((2.0) 2
 * ONCE; both refused before) */
static int proj_ent_ok(const project_t *q)
{
    uint32_t t, e;
    if (q->song > SONG_ONCE)
        return 0;
    for (t = 0; t < NTRK; t++) {
        if (q->cur[t] >= NENT)
            return 0;
        for (e = 0; e < NENT; e++)
            if (q->ent[t][e][0] > PAT_REST || (q->ent[t][e][1] > 15u && q->ent[t][e][1] != REPS_CHAIN))
                return 0;
    }
    return 1;
}
/* (2.0) q -> an SKF4 record (see the top; no steps or motion: the patterns hold them); 0 for a value it can't hold */
static int proj_pack4(project_store_t *out, const project_t *q)
{
    uint8_t *b = out->raw;
    uint32_t pos = PROJ_ENT4, t, e, n, sum;
    for (t = 0; t < NTRK; t++)
        for (e = 0; e < FM6_PACKED; e++)
            if (q->fm6[t][e] > 127u) return 0;
    if (!proj_ent_ok(q)) return 0;
    memset(out, 0, sizeof *out); memcpy(b, &PROJ_SKF4.magic, 4); memcpy(b + 4, &PROJ_SKF4.size, 4);
    b[8] = PROJ_LAYOUT4;
    for (t = 0; t < NTRK; t++)
        for (e = 0; e < NENT; e++, pos += 4u) {            /* (+2, +3: the entry's sound, #30) */
            b[pos] = q->ent[t][e][0];
            b[pos + 1u] = q->ent[t][e][1];
            b[pos + 2u] = q->snd[t][e][0];
            b[pos + 3u] = q->snd[t][e][1];
        }
    memcpy(b + pos, q->cur, NTRK); pos += NTRK;
    memcpy(b + pos, q->scene, PROJ_SCENES); pos += PROJ_SCENES;
    b[pos++] = q->sel; b[pos++] = q->song;
    if (q->dx_on && dx_kit_ok(q->dx)) {
        b[pos] = PROJ_DX_MARK;
        memcpy(b + pos + 1u, q->dx, sizeof q->dx);
    }
    pos += 1u + sizeof q->dx;
    memcpy(b + pos, q->fm6, sizeof q->fm6); pos += sizeof q->fm6;
    pos += gsid_put(b + pos, q->g);
    for (t = 0; t < NTRK; t++) {
        b[pos] = q->t[t].engine; b[pos + 1u] = q->t[t].preset; b[pos + 2u] = q->t[t].user; pos += 3u;   /* (#20) */
        if (!(n = sid_put(b + pos, q->t[t].p))) return 0;
        pos += n;
    }
    {
        char nm[PROJ_NAME_LEN + 1u];
        memcpy(b + PROJ_NAME_AT(&PROJ_SKF4), nm, proj_name_get(nm, (const uint8_t *)q->name));
    }
    sum = proj_hash(b, PROJ_STORE4 - 4u); memcpy(b + PROJ_STORE4 - 4u, &sum, 4);
    return 1;
}
/* (2.0) an SKF4 record (its magic and size checked: proj_fmt_of) -> q; 0 if it is not one this firmware reads */
static int proj_unpack4(project_t *q, const uint8_t *b)
{
    uint32_t pos = PROJ_ENT4, end = PROJ_NAME_AT(&PROJ_SKF4), t, e, n, sum;
    memcpy(&sum, b + PROJ_STORE4 - 4u, 4);
    if (b[8] != PROJ_LAYOUT4 || sum != proj_hash(b, PROJ_STORE4 - 4u)) return 0;
    memset(q, 0, sizeof *q); q->magic = PROJ_MAGIC; q->size = sizeof *q;
    q->parts = PROJ_PARTS; q->phys = PROJ_PHYS;
    for (t = 0; t < NTRK; t++)
        for (e = 0; e < NENT; e++, pos += 4u) {
            q->ent[t][e][0] = b[pos];
            q->ent[t][e][1] = b[pos + 1u];
            q->snd[t][e][0] = b[pos + 2u];             /* ((#30) as it is: one no longer there plays none, esnd_make) */
            q->snd[t][e][1] = b[pos + 3u];
        }
    memcpy(q->cur, b + pos, NTRK); pos += NTRK;
    memcpy(q->scene, b + pos, PROJ_SCENES); pos += PROJ_SCENES;
    q->sel = b[pos++]; q->song = b[pos++];
    if (!proj_ent_ok(q)) return 0;
    if (b[pos] == PROJ_DX_MARK && dx_kit_ok((const dx_lane_t *)(b + pos + 1u))) {
        memcpy(q->dx, b + pos + 1u, sizeof q->dx);
        q->dx_on = 1;
    }
    pos += 1u + sizeof q->dx;
    for (t = 0; t < NTRK; t++)
        for (e = 0; e < FM6_PACKED; e++)
            q->fm6[t][e] = b[pos++] & 0x7Fu;
    for (e = 0; e < G_COUNT; e++)
        q->g[e] = GP[e].def;
    if (!(n = gsid_get(q->g, b + pos, end - pos))) return 0;
    pos += n;
    for (t = 0; t < NTRK; t++) {
        if (end - pos < 3u) return 0;
        q->t[t].engine = b[pos]; q->t[t].preset = b[pos + 1u];
        q->t[t].user = b[pos + 2u] <= UP_SLOTS ? b[pos + 2u] : 0u;   /* ((#20) past the slots: none, the project still loads) */
        pos += 3u;
        if (q->t[t].engine >= NENGINES) return 0;
        for (e = 0; e < P_COUNT; e++)
            q->t[t].p[e] = param_desc_of(q->t[t].engine, e)->def;
        if (!(n = sid_get(q->t[t].p, b + pos, end - pos))) return 0;
        pos += n;
    }
    {
        char nm[PROJ_NAME_LEN + 1u];
        memcpy(q->name, nm, proj_name_get(nm, b + end));
    }
    q->sum = proj_sum(q);
    return 1;
}
/* q -> an SKF2 or (Skiff phase 6) SKF3 record, format f (its steps and motion not: they are the patterns'): 0 for a
 * value no format can hold, or a format of more tracks than this firmware has. Its first f->nt tracks, the first f->sc
 * scene bytes: SKF2 holds four tracks, their scenes. (2.0) SKF4: proj_pack4 */
static int proj_pack2(project_store_t *out, const project_t *q, const proj_fmt_t *f)
{
    uint8_t *b = out->raw;
    uint32_t pos = 68u, t, i, sum;
    if (f == &PROJ_SKF4) return proj_pack4(out, q);
    if (f->nt > NTRK || f->sc > PROJ_SCENES) return 0;
    for (t = 0; t < f->nt; t++)
        for (i = 0; i < FM6_PACKED; i++)
            if (q->fm6[t][i] > 127u) return 0;
    if (!proj_ent_ok(q)) return 0;
    memset(out, 0, sizeof *out); memcpy(b, &f->magic, 4); memcpy(b + 4, &f->size, 4);
    memcpy(b + 8, q->g, sizeof q->g); b[62] = q->sel; b[63] = q->parts; b[64] = q->phys; b[65] = q->song;
    b[66] = PROJ_NP;
    for (t = 0; t < f->nt; t++) {
        if (!proj_np_put(b + pos, q->t[t].p)) return 0;   /* ((phase 9) PROJ_NP, by count) */
        pos += PROJ_NP;
        b[pos++] = q->t[t].engine; b[pos++] = q->t[t].preset;
    }
    memcpy(b + pos, q->ent, f->nt * NENT * 2u); pos += f->nt * NENT * 2u;
    memcpy(b + pos, q->cur, f->nt); pos += f->nt;
    memcpy(b + pos, q->scene, f->sc); pos += f->sc;
    if (pos > PROJ_HEAD_END(f)) return 0;
    if (f->dx) {                                      /* ((phase 9) the parameters past PROJ_NP: the PX block, if one */
        uint32_t any = 0, j;                          /* is not its default) */
        for (t = 0; t < f->nt; t++)
            for (j = 0; j < PROJ_NX; j++) {
                int32_t v = q->t[t].p[PROJ_NP - 8u + j];
                if (v < -64 || v > 127) return 0;
                any |= v != TP[PROJ_NP - 8u + j].def;
            }
        if (any) {
            uint8_t *x = b + PROJ_PX_AT(f);
            x[0] = PROJ_PX_MARK;
            x[1] = (uint8_t)PROJ_NX;
            for (t = 0; t < f->nt; t++)
                for (j = 0; j < PROJ_NX; j++)
                    x[2u + t * PROJ_NX + j] = (uint8_t)(q->t[t].p[PROJ_NP - 8u + j] + 64);
        }
    }
    if (f->dx && q->dx_on && dx_kit_ok(q->dx)) {     /* (phase 7) the DRUM-X kit (none: 0, the factory's) */
        b[PROJ_DX_AT(f)] = PROJ_DX_MARK;
        memcpy(b + PROJ_DX_AT(f) + 1u, q->dx, sizeof q->dx);
    }
    memcpy(b + PROJ_FM6_AT(f), q->fm6, f->nt * FM6_PACKED);
    {
        char n[PROJ_NAME_LEN + 1u];
        memcpy(b + PROJ_NAME_AT(f), n, proj_name_get(n, (const uint8_t *)q->name));
    }
    sum = proj_hash(b, f->size - 4u); memcpy(b + f->size - 4u, &sum, 4);
    return 1;
}
/* how many entries of the saved projects (the SKF2, SKF3 and SKF4 slots) name pattern id (bank.c pat_refs) */
static uint32_t proj_slot_refs(uint32_t id)
{
    uint32_t sl, i, n = 0, np, off, w;
    for (sl = 0; sl < 4u; sl++) {
        const uint8_t *b = proj_slot[sl].raw;
        const proj_fmt_t *f = proj_fmt_of(b, sizeof proj_slot[sl]);
        np = b[66];
        if (f == &PROJ_SKF4)
            off = PROJ_ENT4, w = 4u;
        else if (!f || np < 8u || np > P_COUNT)
            continue;
        else
            off = 68u + f->nt * (np + 2u), w = 2u;
        for (i = 0; i < f->nt * NENT; i++)
            n += b[off + w * i] == id + 1u;
    }
    return n;
}
/* an SKF2 or SKF3 record (format f) -> q (no steps, no motion: the patterns hold them); f->nt tracks, the rest as at
 * power-on (an SKF2's scenes leave T5, T6 as they are: bank.c scene_trk). (2.0) SKF4: proj_unpack4 */
static int proj_unpack2(project_t *q, const uint8_t *b, const proj_fmt_t *f)
{
    uint32_t pos = 68u, t, i, magic, size, sum, np = b[66];
    if (f == &PROJ_SKF4) return proj_unpack4(q, b);
    memcpy(&magic, b, 4); memcpy(&size, b + 4, 4); memcpy(&sum, b + f->size - 4u, 4);
    if (f->nt > NTRK || f->sc > PROJ_SCENES || magic != f->magic || size != f->size ||
        sum != proj_hash(b, f->size - 4u) || np < 8u || np > P_COUNT ||
        68u + f->nt * (np + 2u + NENT * 2u + 1u) + f->sc > PROJ_HEAD_END(f))
        return 0;
    memset(q, 0, sizeof *q); q->magic = PROJ_MAGIC; q->size = sizeof *q;
    if (f->dx && b[PROJ_DX_AT(f)] == PROJ_DX_MARK && dx_kit_ok((const dx_lane_t *)(b + PROJ_DX_AT(f) + 1u))) {
        memcpy(q->dx, b + PROJ_DX_AT(f) + 1u, sizeof q->dx);   /* (phase 7) its DRUM-X kit (bytes: dx_lane_t's are) */
        q->dx_on = 1;
    }
    memcpy(q->g, b + 8, sizeof q->g); q->sel = b[62]; q->parts = b[63]; q->phys = b[64]; q->song = b[65];
    for (t = 0; t < f->nt; t++) {
        int16_t values[P_COUNT], def[P_COUNT];
        for (i = 0; i < np; i++) { if (b[pos] > 191u) return 0; values[i] = (int16_t)b[pos++] - 64; }
        q->t[t].engine = b[pos++]; q->t[t].preset = b[pos++];
        if (q->t[t].engine >= NENGINES) return 0;
        for (i = 0; i < P_COUNT; i++) def[i] = param_desc_of(q->t[t].engine, i)->def;
        params_by_count(q->t[t].p, values, np, def);
    }
    if (f->dx && np == PROJ_NP && b[PROJ_PX_AT(f)] == PROJ_PX_MARK) {   /* ((phase 9) the parameters past PROJ_NP: */
        const uint8_t *x = b + PROJ_PX_AT(f);         /* n of them a track, the ones this firmware knows taken) */
        uint32_t n = x[1], j;
        if (n > PROJ_PX_MAX) return 0;
        for (t = 0; t < f->nt; t++)
            for (j = 0; j < n; j++) {
                if (x[2u + t * n + j] > 191u) return 0;
                if (j < PROJ_NX)
                    q->t[t].p[PROJ_NP - 8u + j] = (int16_t)(x[2u + t * n + j] - 64);
            }
    }
    memcpy(q->ent, b + pos, f->nt * NENT * 2u); pos += f->nt * NENT * 2u;
    memcpy(q->cur, b + pos, f->nt); pos += f->nt;
    memcpy(q->scene, b + pos, f->sc);
    if (!proj_ent_ok(q)) return 0;
    for (t = 0; t < f->nt; t++)
        for (i = 0; i < FM6_PACKED; i++)
            q->fm6[t][i] = b[PROJ_FM6_AT(f) + t * FM6_PACKED + i] & 0x7Fu;
    proj_power_on(q, f->nt);
    {
        char n[PROJ_NAME_LEN + 1u];
        memcpy(q->name, n, proj_name_get(n, b + PROJ_NAME_AT(f)));
    }
    q->sum = proj_sum(q);
    return 1;
}

#ifndef PROJ_HOST

static project_t proj_scratch;              /* decoded main-loop work, never audio ISR */
static char proj_name[PROJ_NAME_LEN + 1u]    /* the name of the music as it is now (loaded, saved, the editor's */
    __attribute__((section(".pool")));       /* runtime restore); "" = none. A save takes it unless one is given */
#define PROJ_NO_SLOT 0xFFu
static uint8_t proj_cur = PROJ_NO_SLOT;      /* the slot the music was loaded from or last saved to (a rename of it
                                              * renames the music too); PROJ_NO_SLOT none (the editor's restore) */
static union {                               /* serialized main-loop work; no retained expansion */
    project_store_t s;
    uint8_t raw[3840];                         /* (the staging of a backup object, up to a storage object: editor_backup.c) */
} proj_wire_u;
#define proj_wire (proj_wire_u.s)
static uint8_t proj_wire_gen;                /* +1 whenever proj_wire is rewritten (a backup's runtime copy lives there) */


/* an imported older format may hold values FUN7 cannot pack: keep them inside the fields and ranges */
static void proj_bound(project_t *q)
{
    uint32_t t, i;
    for (t = 0; t < NTRK; t++) {
        for (i = 0; i < P_COUNT; i++) {
            const param_desc_t *d = param_desc_of(q->t[t].engine % NENGINES, i);
            q->t[t].p[i] = (int16_t)param_fit(d, q->t[t].p[i]);   /* (a retired KIT: the kit it plays) */
        }
    }
    q->sum = proj_sum(q);
}

#if FELUCCA_FLASH
/* slot from flash into RAM (SKF3, SKF4; anything else: empty) */
static void proj_fetch(uint32_t slot)
{
    project_store_t *q = &proj_slot[slot & 3u];
    const proj_fmt_t *f;
    int n;
    proj_wire_gen++;
    n = st_load(OBJ_PROJECT0 + (slot & 3u), proj_wire_u.raw, sizeof proj_wire_u.raw);   /* ((2.0) a longer one too) */
    f = proj_fmt_of(proj_wire_u.raw, n);           /* (phase 5, 6, 2.0) SKF2 .. SKF4 stay as they are: they name their
                                                   * patterns */
    if (!proj_import(&proj_scratch, proj_wire_u.raw, n))
        q->align = 0u;
    else {
        proj_bound(&proj_scratch);
        if (!proj_pack2(q, &proj_scratch, f))
            memset(q->raw, 0, 4);
    }
}
#endif

static void project_capture(project_t *p)
{
    uint32_t i;
    uint32_t f = motion_guard();
    memset(p, 0, sizeof *p);
    p->magic = PROJ_MAGIC;
    p->size = sizeof *p;
    for (i = 0; i < G_COUNT; i++)
        p->g[i] = song.g[i];
    p->sel = song.sel;
    p->parts = PROJ_PARTS;
    p->phys = PROJ_PHYS;
    for (i = 0; i < NTRK; i++) {
        for (uint32_t j = 0; j < P_COUNT; j++) p->t[i].p[j] = motion_base_value(&trk[i], j);
        p->t[i].engine = trk[i].eng_req;
        p->t[i].preset = trk[i].preset;
        p->t[i].user = (uint8_t)(user_of(&trk[i]) < UP_SLOTS ? user_of(&trk[i]) + 1u : 0u);   /* ((2.0, #20)) */
        fm6_pack(fm6_patch[i], p->fm6[i]);
    }
    motion_unguard(f);
    memcpy(p->name, proj_name, str_len(proj_name));
    memcpy(p->ent, lists.ent, sizeof p->ent);      /* (phase 5) each track's list, the entry it is on, SONG */
    memcpy(p->snd, lists.snd, sizeof p->snd);      /* ((2.0, #30) the entries' sounds) */
    memcpy(p->cur, lists.cur, sizeof p->cur);
    p->song = lists.song;
    memcpy(p->scene, lists.scene, sizeof p->scene);  /* (the scenes too: the project's) */
    if (memcmp(dx_kit, DX_KIT_DEF, sizeof dx_kit)) {   /* (phase 7) the DRUM-X kit, when not the factory's (none: */
        p->dx_on = 1;                              /* the factory's when loaded; a project of before as it was) */
        memcpy(p->dx, dx_kit, sizeof p->dx);
    }
    p->sum = proj_sum(p);
}

/* the music as it is now -> slot, named `name` (0: the current name; "" none); 0 saved, nonzero refused or failed.
 * The current name becomes the saved one */
static void project_clean(void);
static int project_save_as(uint32_t slot, const char *name)
{
    project_t *p = &proj_scratch;
    int kr;
    if (transport_busy()) {                            /* a flash erase silences the audio and stalls the */
        ui_message("STOP TO SAVE");                     /* sequencer (storage_hw.c): only while stopped */
        return 1;
    }
    if ((kr = keep_all()) != 0) {                      /* (phase 5) the tracks' patterns first: a project names them */
        ui_way(kr == 1 ? "PATTERNS FULL" : "SAVE ERROR", kr == 1 ? WAY_PATTERNS : "");
        return kr;
    }
    project_capture(p);
    if (name) {
        memset(p->name, 0, sizeof p->name);
        memcpy(p->name, name, str_len(name) < PROJ_NAME_LEN ? str_len(name) : PROJ_NAME_LEN);
    }
    proj_wire_gen++;
    if (!proj_pack2(&proj_wire, p, PROJ_SKF)) { ui_message("SAVE FORMAT ERROR"); return 2; }

#if FELUCCA_FLASH
    if (flash_ok) {
        if (st_save(OBJ_PROJECT0 + (slot & 3u), &proj_wire, PROJ_SKF->size)) {
            ui_message("SAVE ERROR");
            return 2;
        }
        memcpy(&proj_slot[slot & 3u], &proj_wire, sizeof proj_wire);
        proj_name_get(proj_name, (const uint8_t *)p->name);
        proj_cur = (uint8_t)(slot & 3u);
        project_clean();
        ui_message("SAVED");
        return 0;
    }
#endif
    memcpy(&proj_slot[slot & 3u], &proj_wire, sizeof proj_wire);
    proj_name_get(proj_name, (const uint8_t *)p->name);
    proj_cur = (uint8_t)(slot & 3u);
    project_clean();
    ui_message("SAVED UNTIL POWER OFF");
    return 0;
}
static int project_save(uint32_t slot) { return project_save_as(slot, 0); }
static void project_cur_name(char *b) { str_cpy(b, proj_name, PROJ_NAME_LEN + 1u); }   /* b: 13 bytes */

/* slot's name -> b (PROJ_NAME_LEN + 1 bytes); 0 = an empty slot (b "").
 * Uses proj_scratch */
static int project_name(uint32_t slot, char *b)
{
    b[0] = 0;
    if (!proj_import(&proj_scratch, &proj_slot[slot & 3u], sizeof(project_store_t))) {
        return 0;
    }
    proj_name_get(b, (const uint8_t *)proj_scratch.name);
    return 1;
}

/* a stored project renamed in place (nothing else of it changes; the music playing is not touched, but the slot
 * it was loaded from / saved to: its name is the new one, the next SAVE's prefill): 0 done, 1 refused (playing,
 * an empty slot), 2 failed (the slot as it was) */
static int project_rename(uint32_t slot, const char *name)
{
    project_t *p = &proj_scratch;
    const proj_fmt_t *f = proj_fmt_of(&proj_slot[slot & 3u], sizeof(project_store_t));   /* (phase 5, 6) its format
                                                                                * kept: SKF2 .. SKF4 name their patterns */
    if (transport_busy()) {
        ui_message("STOP TO SAVE");
        return 1;
    }
    if (!proj_import(p, &proj_slot[slot & 3u], sizeof(project_store_t))) {
        ui_message("EMPTY SLOT");
        return 1;
    }
    memset(p->name, 0, sizeof p->name);
    memcpy(p->name, name, str_len(name) < PROJ_NAME_LEN ? str_len(name) : PROJ_NAME_LEN);
    proj_wire_gen++;
    if (!proj_pack2(&proj_wire, p, f)) { ui_message("SAVE FORMAT ERROR"); return 2; }
#if FELUCCA_FLASH
    if (flash_ok && st_save(OBJ_PROJECT0 + (slot & 3u), &proj_wire, f ? f->size : sizeof proj_wire)) {
        ui_message("SAVE ERROR");
        return 2;
    }
#endif
    memcpy(&proj_slot[slot & 3u], &proj_wire, sizeof proj_wire);
    if (proj_cur == (slot & 3u)) {                      /* (the music's name too: as it was, saved or not) */
        int was = !project_changed();
        proj_name_get(proj_name, (const uint8_t *)p->name);
        if (was)
            project_clean();
    }
#if FELUCCA_FLASH
    if (flash_ok) { ui_message("RENAMED"); return 0; }
#endif
    ui_message("RENAMED UNTIL POWER OFF");
    return 0;
}

static int project_restore_runtime(const project_t *input)
{
    project_t *p = &proj_scratch;
    uint32_t i, k;
    if (!proj_ok(input) || !proj_engines_ok(input)) return 1;
    if (p != input) memcpy(p, input, sizeof *p);
    transport_req = 2;
    panic_req = (1u << NTRK) - 1u;
    fm1_irq_off();                                      /* the audio ISR must not see half a project */
    seq_stop();
    launch_drop();                                      /* (Skiff) the bank's staged patterns: not over it (launch.c) */
    transport_req = 0;
    memset(&motion, 0, sizeof motion);                  /* ((2.0) the patterns hold it: each track's, as it loads) */
    memset(motion_active, 0, sizeof motion_active);
    motion_base_valid = 0;
    memcpy(lists.ent, p->ent, sizeof lists.ent);       /* (phase 5) the lists; the tracks on no pattern yet */
    memcpy(lists.snd, p->snd, sizeof lists.snd);       /* ((2.0, #30) the entries' sounds) */
    memcpy(lists.cur, p->cur, sizeof lists.cur);
    lists.song = p->song;
    memcpy(lists.scene, p->scene, sizeof lists.scene);
    scene_forget();
    memset(trk_pid, 0, sizeof trk_pid);
    motion_lost = 0;
    for (i = 0; i < G_COUNT; i++)                       /* ((Skiff U3, R8) not the unit's CLOCK MIDI IN TUNE: ui.c) */
        if (i != G_SLOT && i != G_LOAD && i != G_SAVE && (unit_g_adopt || !unit_g_id(i)))
            song.g[i] = (int16_t)clamp(p->g[i], GP[i].min, GP[i].max);
    for (k = 0; k < NTRK; k++) {
        track_t *t = &trk[k];
        const proj_trk_t *s = &p->t[k];
        uint32_t e = s->engine;                         /* (< NENGINES: proj_engines_ok) */
        t->eng_req = (uint8_t)e;
        t->user = s->user && up_used(s->user - 1u) && up_engine(s->user - 1u) == e ? s->user : 0u;   /* ((2.0, #20)
                                                         * the user sound it came from, while that slot still holds one
                                                         * of its engine: HOME and EDIT name it; else none, as before) */
        for (i = 0; i < P_COUNT; i++) {                 /* every value back inside its range (param_fit) */
            const param_desc_t *d = param_desc_of(e, i);
            t->p[i] = (int16_t)param_fit(d, s->p[i]);
        }
        t->preset = (uint8_t)(ENGINES[e]->npresets ? (s->preset >= PROJ_DEF_KEEP ? 0u : s->preset) % ENGINES[e]->npresets : 0u);
        memset(t->step, 0, sizeof t->step);             /* ((2.0) none until its pattern loads, below) */
        for (i = 0; i < NSTEP; i++)
            t->step[i].time = ST_REST;
        {   /* the project's own FM6 patch, never reloaded from SLOT: F n if it is that factory patch, else OWN */
            uint8_t v[FP_SIZE + 1u];
            fm6_unpack(p->fm6[k], v);
            fm6_set_patch(k, v);
            fm6_adopt(k);
        }
    }
    sel_set((uint8_t)(p->sel < NTRK ? p->sel : 0u));
    memcpy(dx_kit, p->dx_on && dx_kit_ok(p->dx) ? p->dx : DX_KIT_DEF, sizeof dx_kit);   /* (phase 7) its DRUM-X kit */
    fm1_irq_on();
    proj_name_get(proj_name, (const uint8_t *)p->name);
    proj_cur = PROJ_NO_SLOT;                            /* (project_load: its slot) */
    undo_clear();                                       /* (ui.c) the undo copies belong to the old project */
    undo_depth++;                                       /* and these loads take none */
    for (k = 0; k < NTRK; k++) {                        /* the power-on sounds: format 1 (tracks 2..4), old drums */
        track_t *t = &trk[k];
        if (p->t[k].preset == PROJ_DEF_SOUND) {
            apply_preset_to(t, TRK_DEF[k][1]);
            track_defaults_steps(t);
        }
        {   /* (phase 5) the pattern of the entry it is on (SKF2); none: the record's steps (a format before) */
            uint32_t id = ent_pat(k, lists.cur[k]);
            if (id != PAT_NONE && pool_used(id))
                (void)pat_load(t, id);
        }
    }
    undo_depth--;
    sync_reload = 1;
    ui.force = 1;
    ui_message("LOADED");
    memset(snd_said, 0, sizeof snd_said);               /* a missing sample is said again, after LOADED */
    return 0;
}
static void project_load(uint32_t slot)
{
    mom_back(0);                                  /* ((1.3) MOMENTARY: the values as they were, then the load) */
    memset(drum_mute, 0, sizeof drum_mute);       /* ((1.3) the DRUM lanes' mutes, performance state) */
#if FELUCCA_FLASH
    if (flash_ok && !proj_import(&proj_scratch, &proj_slot[slot & 3u], sizeof(project_store_t))) proj_fetch(slot);
#endif
    if (!proj_import(&proj_scratch, &proj_slot[slot & 3u], sizeof(project_store_t))) {
        ui_message("EMPTY SLOT");
        return;
    }
    proj_bound(&proj_scratch);                          /* a retained RAM slot of an older format: as from flash */
    transport_stop_now();                               /* (phase 5) stopped first (the ISR swaps nothing more, the */
    (void)stage_cancel();                               /* steps are still while kept), nothing staged comes in; */
                                                        /* what the tracks have kept (a swap made: what went out) */
    if (keep_all() == 1) {
        ui_way("PATTERNS FULL", WAY_PATTERNS);
        return;
    }
    if (!project_restore_runtime(&proj_scratch)) {
        proj_cur = (uint8_t)(slot & 3u);
        project_clean();
    }
}

/* settings + learned panel table: one flash object. The flash copy wins at
 * boot (the .noinit copies are garbage after a power-off). */
#include "settings_persist.c"
#if FELUCCA_FLASH && FELUCCA_SLICE
#include "slice_store.c"                          /* SLICE's MAN slices, kept in the user slots */
#endif
#if FELUCCA_FLASH
static sks_t persist_saved;                     /* ((2.0) SKS1: what the flash holds, or would after a PER4 read) */
static uint8_t persist_pending;                 /* 1 requested, 2 waiting after a flash error */
static uint32_t persist_retry_ms;
#endif

static void persist_boot(void)                    /* before settings_init / panel_init */
{
#if FELUCCA_FLASH
    union { persist_t per; sks_t sks; } p;         /* ((2.0) SKS1, or a PER4 of before) */
    uint32_t f = irq_save();
#if BOARD_SMK37
    flash_ok = FL_JEDEC_OK(FL_FAR(fl_jedec_ram)());      /* the SMK-37's part (or the FM-1's): fm1_flash_map.h */
#else
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;       /* the expected 1 MiB part, else stay RAM-only */
#endif
    irq_restore(f);
    if (!flash_ok)
        return;
    fl_plain_window_init();                        /* flash above 0x93000 (SMK 0x9D000) reads as plaintext through XIP
                                                    * (user sample sets are played from there) */
#if BOARD_SMK37
    st_owner_check();                              /* never parse another firmware's store (storage.c) */
#endif
#if FELUCCA_SLICE
    slc_store_boot();                              /* (the scans read each slot's stored slices) */
#endif
    {
        uint32_t k;
        for (k = 0; k < SMP_USER_SLOTS; k++)
            smp_user_scan(k);
    }
    {
        int n = st_load(OBJ_SETTINGS, &p, sizeof p);
        if (settings_import(&p, n)) {
            if (p.sks.magic != SKS_MAGIC)              /* (a PER4: as SKS1, written at the next change) */
                memset(&p.sks, 0, sizeof p.sks);
            persist_saved = p.sks;
            settings_export(&persist_saved);
        }
    }
    {   /* projects: fill empty RAM slots from flash, so the slot list is right after power-on */
        uint32_t i;
        for (i = 0; i < 4u; i++)
            if (!proj_import(&proj_scratch, &proj_slot[i], sizeof(project_store_t)))
                proj_fetch(i);
    }
    up_boot();                                     /* user presets */
#endif
}

/* a project there */
static int project_used(uint32_t slot)
{
    return proj_import(&proj_scratch, &proj_slot[slot & 3u], sizeof(project_store_t));
}

static void settings_poll(void)
{
#if FELUCCA_FLASH
    sks_t p;
#if FELUCCA_SLICE
    slc_store_poll();                              /* SLICE's slices edited on the SLICES page */
#endif
    if (!persist_pending || !flash_ok || transport_busy() ||
        (persist_pending == 2u && (uint32_t)(fm1_ms - persist_retry_ms) < 1000u))
        return;
    p = persist_saved;
    settings_export(&p);
    if (!memcmp(&p, &persist_saved, sizeof p)) {
        persist_pending = 0;
        return;                                    /* unchanged: no erase cycle */
    }
    if (st_save(OBJ_SETTINGS, &p, sizeof p) == 0) {
        persist_saved = p;
        persist_pending = 0;
    } else {
        persist_pending = 2;
        persist_retry_ms = fm1_ms;
    }
#endif
}

static void settings_save(void)
{
#if FELUCCA_FLASH
    persist_pending = 1;
#endif
    settings_poll();
}

/* The autosave (1.2, Discussion #130): the music as it is (what a project holds: every track's sound, steps, LEN DIV
 * SWING GATE, mix, the song, the automation and locks, the FM6 patches, the name) kept in its own storage object
 * (OBJ_AUTOSAVE, A/B at 0xE5000 / 0xE6000, never one of the four project slots), and put back at power-on
 * (autosave_boot) with MENU > SYSTEM > RESTORE LAST ON (the default). Flash wear: a 4 KiB erase per write, the two
 * sectors taking turns, so a write only when
 *   - something a project holds changed since the last one (autosave_sig: a hash of those values, no copy; not the
 *     selected track, nor the PROJECT page's own controls),
 *   - and it has stayed the same for AS_IDLE_MS (2 s) with the transport stopped, no key or button down, no voice
 *     sounding and no editor transfer (a backup or a sample: autosave_hold); an erase stops the audio for its
 *     ~40 ms, as every save does, so it never happens while anything plays,
 *   - and at least AS_GAP_MS (20 s) after the one before (AS_RETRY_MS after a failed write);
 *   - (2.0) after a save, a load or NEW (autosave_soon): idle AS_SOON_MS (1 s), no gap.
 * So at most three writes a minute while editing, none while playing or idle: about 100,000 erase cycles per sector
 * give over 200,000 writes, three a minute for 8 hours a day for over four months, far more with real pauses. ((2.0,
 * the user's pick on the unit, 2026-10-24: a DRUM-X switch and a pan, the unit off two seconds on, both lost; 1.2 had
 * 10 s and a minute: a minute's edits lost, a year's wear at the worst).
 * RESTORE LAST OFF writes none (and restores none). Never after a crash, a hang or a failed boot (main.c): the music
 * that was playing then could be what crashed. Checked every AS_POLL_MS; the main loop only (never the audio ISR).
 * (Skiff phase 5) The steps are the patterns' (bank.c): each write is a round, the patterns the tracks changed kept
 * first (a chunk of them a poll, each an erase in the pool's rotating sectors), then the project; RESTORE LAST OFF
 * keeps the patterns all the same. Patterns waiting with no slot free for another: the round does not wait its gap */
#define AS_POLL_MS 250u
#define AS_IDLE_MS 2000u
#define AS_GAP_MS 20000u
#define AS_RETRY_MS 30000u
#define AS_SOON_MS 1000u
static struct {
    uint32_t saved, seen;                        /* the signature written last (or loaded at power-on), seen last */
    uint32_t t, last, poll;                      /* fm1_ms: seen changed / busy, the last round, the last check */
    uint32_t writes;                             /* (written since power-on: the host tests count them) */
    uint8_t wrote, err;                          /* a round since power-on (last is valid); the last one failed */
    uint8_t round;                               /* (phase 5) a round going on: the patterns, then the project */
    uint8_t soon;                                /* (2.0) a save asked for one: idle AS_SOON_MS, no gap */
} as __attribute__((section(".pool")));
/* (2.0, the unit 2026-10-24: a user sound saved on T1, the unit switched off at once, T1 came back ACID) a save, a load
 * or NEW (SAVE > USER, SAVE > PROJECT) brings the autosave up to date within a second: the music as it is then comes
 * back at power-on. The user's own act, a flash write already: no wear to speak of */
static void autosave_soon(void) { as.soon = 1; }

static uint32_t as_mix(uint32_t h, const void *p, uint32_t n)   /* FNV-1a, continued */
{
    const uint8_t *b = p;
    while (n--) {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

/* what an autosave would hold, hashed (no copy, no RAM): project_capture's values */
static uint32_t autosave_sig(void)
{
    uint32_t h = 2166136261u, i, j;
    for (i = 0; i < NTRK; i++) {
        const track_t *t = &trk[i];
        for (j = 0; j < P_COUNT; j++) {
            int16_t v = motion_base_value(t, j);
            h = as_mix(h, &v, sizeof v);
        }
        h = as_mix(h, &t->eng_req, 1);
        h = as_mix(h, &t->preset, 1);
        h = as_mix(h, &t->user, 1);                   /* ((2.0, #20) the user sound it is on: SKF4 holds it) */
        h = as_mix(h, t->step, sizeof t->step);
        h = as_mix(h, fm6_patch[i], sizeof fm6_patch[i]);
    }
    h = as_mix(h, dx_kit, sizeof dx_kit);             /* (phase 7) the DRUM-X kit */
    for (i = 0; i < G_COUNT; i++)                       /* ((Skiff U3) not the unit's CLOCK MIDI IN TUNE, ui.c) */
        if (i != G_SLOT && i != G_NAME && i != G_LOAD && i != G_SAVE && !unit_g_id(i))
            h = as_mix(h, &song.g[i], sizeof song.g[i]);
    h = as_mix(h, &motion, sizeof motion);
    h = as_mix(h, &lists, sizeof lists);              /* (phase 5) the lists, the patterns the tracks are on */
    h = as_mix(h, trk_pid, sizeof trk_pid);
    i = (ui_prefs & PREF_RESTORE_OFF) != 0;           /* (RESTORE LAST back ON: a round, the project written) */
    h = as_mix(h, &i, sizeof i);
    return as_mix(h, proj_name, sizeof proj_name);
}

/* (Skiff U3, the review's A2) the project the music is: the slot it was saved to or loaded from (proj_cur), and whether
 * it changed since (autosave_sig against its value then, looked at every 250 ms): the header's letter and dot
 * (ui_blocks.c), and the LOAD dialog asks only then (ui_input.c). Kept over a power cycle with the autosave in settings
 * bytes no engine uses ((2.0) SKS1's restore_slot, restore_clean): [15][0] the slot + 1 (0 none), [15][1..4] the value.
 * Unknown (none kept, another slot's music restored): changed, so nothing is lost without a question */
static uint32_t pj_clean __attribute__((section(".pool")));
static uint32_t pj_at __attribute__((section(".pool")));    /* fm1_ms | 1 of the last look; 0 none */
static uint8_t pj_dirty __attribute__((section(".pool")));
static void project_clean(void)                     /* a save or a load: the music is proj_cur's */
{
    pj_clean = autosave_sig();
    pj_dirty = 0;
    pj_at = fm1_ms | 1u;
    favorites.factory[15][0] = (uint8_t)(proj_cur < 4u ? proj_cur + 1u : 0u);
    memcpy(&favorites.factory[15][1], &pj_clean, 4);
    settings_save();
    autosave_soon();
}
static int project_dirty(void)
{
    if (!pj_at || fm1_ms - pj_at >= 250u) {
        pj_at = fm1_ms | 1u;
        pj_dirty = autosave_sig() != pj_clean;
    }
    return pj_dirty;
}
static int project_changed(void)                    /* (now, not as last looked: a dialog's question) */
{
    pj_at = 0;
    return project_dirty();
}
static uint32_t project_mark(void)
{
    return proj_cur < 4u ? ('A' + proj_cur) | (uint32_t)project_dirty() << 8 : 0u;
}
/* power-on, the autosave back (autosave_boot): the slot it was and its value then; nothing restored: the music as it is
 * now (nothing there to lose) */
static void project_mark_boot(int restored)
{
    uint32_t k = favorites.factory[15][0];
    pj_at = 0;
    if (!restored) {
        pj_clean = autosave_sig();
        return;
    }
    pj_clean = 0;
    if (k >= 1u && k <= 4u && project_used(k - 1u)) {
        proj_cur = (uint8_t)(k - 1u);
        memcpy(&pj_clean, &favorites.factory[15][1], 4);
    }
}

/* (1.2.1, #6) SAVE > PROJECT's DELETE: the slot emptied, in flash (an empty record: st_load reads 0) and in its RAM copy
 * (.noinit, kept over a warm reset; persist_boot and project_load read the flash only when it is empty). The music
 * playing stays; if it was this slot's it is on no slot now, and unknown counts as changed (project_dirty: nothing is
 * lost without a question). Patterns only it named stay in the pool, unused (PATTERNS can clear them).
 * 0 done, 1 refused (playing, an empty slot), 2 failed (the slot as it was) */
static int project_erase(uint32_t slot)
{
    slot &= 3u;
    if (transport_busy()) {
        ui_message("STOP TO DELETE");
        return 1;
    }
    if (!project_used(slot)) {
        ui_message("EMPTY SLOT");
        return 1;
    }
#if FELUCCA_FLASH
    if (flash_ok && st_save(OBJ_PROJECT0 + slot, &proj_wire, 0)) {
        ui_message("DELETE ERROR");
        return 2;
    }
#endif
    memset(&proj_slot[slot], 0, sizeof proj_slot[slot]);
    if (proj_cur == slot) {
        proj_cur = PROJ_NO_SLOT;
        pj_clean = autosave_sig() + 1u;
        pj_at = 0;
    }
#if FELUCCA_FLASH
    if (flash_ok) { ui_message("PROJECT DELETED"); return 0; }
#endif
    ui_message("DELETED UNTIL POWER OFF");
    return 0;
}

/* (1.2.1, #6) SAVE > PROJECT's NEW (KNOB 3 on an empty slot): the power-on project (felucca_init: the tracks' default
 * sounds, no steps, no lists, no motion, no scenes, the default DRUM-X kit), the way a load puts one in (stopped, the
 * tracks' patterns kept first, the ISR kept out while the sequencer's state changes). The unit's CLOCK MIDI IN TUNE and
 * SAVE's slot stay; unnamed and on no slot until SAVE writes one, with nothing to lose (clean) */
static void project_new(void)
{
    uint32_t i, k;
    mom_back(0);                                  /* ((1.3) MOMENTARY: as project_load) */
    memset(drum_mute, 0, sizeof drum_mute);
    transport_stop_now();
    (void)stage_cancel();
    if (keep_all() == 1) {
        ui_way("PATTERNS FULL", WAY_PATTERNS);
        return;
    }
    transport_req = 2;
    panic_req = (1u << NTRK) - 1u;
    fm1_irq_off();
    seq_stop();
    launch_drop();
    transport_req = 0;
    memset(&motion, 0, sizeof motion);
    memset(motion_active, 0, sizeof motion_active);
    motion_base_valid = 0;
    motion_lost = 0;
    lists_clear();
    scene_forget();
    for (i = 0; i < G_COUNT; i++)
        if (i != G_SLOT && i != G_LOAD && i != G_SAVE && !unit_g_id(i))
            song.g[i] = GP[i].def;
    memcpy(dx_kit, DX_KIT_DEF, sizeof dx_kit);
    sel_set(0);
    fm1_irq_on();
    undo_clear();
    undo_depth++;                                       /* (no undo copies of these loads) */
    fm6_init();
    for (k = 0; k < NTRK; k++) {                        /* (as felucca_init; the engine switch is the ISR's, faded) */
        track_t *t = &trk[k];
        track_defaults(t);
        t->user = 0;
        set_engine_of(t, TRK_DEF[k][0]);
        apply_preset_to(t, TRK_DEF[k][1]);
        track_defaults_steps(t);
    }
    undo_depth--;
    proj_name[0] = 0;
    proj_cur = PROJ_NO_SLOT;
    project_clean();
    sync_reload = 1;
    ui.force = 1;
    ui_message("NEW PROJECT");
    memset(snd_said, 0, sizeof snd_said);
}

/* nothing going on that an erase (the audio stopped ~40 ms) would be heard in, or would race */
static int autosave_quiet(void)
{
    uint32_t k, i;
    if (transport_busy() || fm1_in.notes || fm1_in.buttons)
        return 0;
    for (k = 0; k < NTRK; k++)
        for (i = 0; i < NVOICE; i++)
            if (trk[k].v[i].active)
                return 0;
    return 1;
}

static void autosave_hold(void) { as.t = fm1_ms; }   /* the editor's transfers: their staging RAM is proj_wire */

/* main loop, every pass (after settings_poll) */
static void autosave_poll(void)
{
#if FELUCCA_FLASH
    uint32_t sig, w;
    int rc;
    if (!flash_ok || fm1_ms - as.poll < AS_POLL_MS)
        return;
    as.poll = fm1_ms;
    sig = autosave_sig();
    if (sig != as.seen || !autosave_quiet()) {
        as.seen = sig;
        as.t = fm1_ms;
        return;
    }
    if (fm1_ms - as.t < (as.soon ? AS_SOON_MS : AS_IDLE_MS))
        return;
    if (!as.round) {                                   /* (phase 5) a round: the patterns kept, one erase a poll; */
        w = keep_waiting();                            /* then the project, RESTORE LAST ON */
        if (sig == as.saved && !w) {
            as.soon = 0;                               /* ((2.0) nothing new for it: the next change waits its 2 s) */
            return;
        }
        if (w < 2u && as.wrote && !as.soon && fm1_ms - as.last < (as.err ? AS_RETRY_MS : AS_GAP_MS))
            return;                                    /* (no slot free to wait in: not the gap) */
        as.round = 1;
        as.soon = 0;
        as.last = fm1_ms;
        as.wrote = 1;
    }
    rc = keep_poll();
    sig = as.seen = autosave_sig();                    /* (the round's own change, a new pattern named: no edit) */
    if (rc == 1)
        return;
    as.round = 0;
    as.err = rc != 0;
    if (as.err)
        return;
    if (!(ui_prefs & PREF_RESTORE_OFF)) {
        project_capture(&proj_scratch);
        proj_wire_gen++;
        as.err = !proj_pack2(&proj_wire, &proj_scratch, PROJ_SKF) || st_save(OBJ_AUTOSAVE, &proj_wire, PROJ_SKF->size) != 0;
        if (as.err)
            return;
        as.writes++;
    }
    as.saved = sig;
#endif
}

/* power-on, after the defaults (main.c): the autosave becomes the music when `allowed` (no crash, hang or failed boot
 * before) and RESTORE LAST is ON; an autosave that does not load (none, torn, damaged, another format) is ignored.
 * The music now (restored or not) is what is saved: nothing is written until it changes. 1 = restored */
static int autosave_boot(int allowed)
{
    int rc = 0;
#if FELUCCA_FLASH
    if (flash_ok && allowed && !(ui_prefs & PREF_RESTORE_OFF)) {
        int n;
        proj_wire_gen++;
        n = st_load(OBJ_AUTOSAVE, &proj_wire, sizeof proj_wire);
        if (n > 0 && proj_import(&proj_scratch, &proj_wire, n)) {
            proj_bound(&proj_scratch);
            if (!project_restore_runtime(&proj_scratch)) {
                ui_message("RESTORED");
                rc = 1;
            }
        }
    }
    if (flash_ok && !allowed && !(ui_prefs & PREF_RESTORE_OFF))   /* ((Skiff U4, F5) a crash, a hang or a failed boot
                                                         * before: skipped on purpose, and said) */
        ui_way("SESSION NOT RESTORED", "IT STOPPED BEFORE");
#else
    (void)allowed;
#endif
    as.saved = as.seen = autosave_sig();
    as.t = fm1_ms;
    project_mark_boot(rc);                              /* ((Skiff U3, A2) the project it is, changed or not) */
    return rc;
}

#if FELUCCA_FLASH
_Static_assert(sizeof(project_store_t) <= ST_PAYLOAD_MAX, "project does not fit one flash sector");
_Static_assert(sizeof(persist_t) <= ST_PAYLOAD_MAX && sizeof(sks_t) <= ST_PAYLOAD_MAX, "settings do not fit one flash sector");
#endif
#endif /* PROJ_HOST */
