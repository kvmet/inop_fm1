/* SPDX-License-Identifier: GPL-3.0-only */
/* The arpeggiator: docs/index.md, Arp. It sits after the sequencer and the keys, and before the voices. No hardware here: tests/inop_test.c runs this on the host. */
#define ARP_NOTES 8u
#define ARP_OWNER_STEP 0xFFu

typedef struct {
    pkt_t pkt;
    uint32_t left;               /* samples until the note leaves; 0: until something ends it */
    uint8_t owner;               /* the key that holds it, or ARP_OWNER_STEP */
    uint8_t released;            /* a latched note whose key is up */
} arp_note_t;

struct arp_s {
    arp_note_t note[ARP_NOTES];  /* oldest first */
    uint8_t n;
    uint32_t pos;                /* hits since the arp was last empty */
    int32_t wait;                /* until the next hit, in sixths of a sample so triplets do not drift */
};

typedef struct arp_s arp_t;
/* hold_blocks 0: the voice lasts until the note's owner lets go */
typedef void (*arp_emit_t)(const arp_t *a, const pkt_t *p, uint32_t owner, uint32_t hold_blocks);

static const uint8_t ARP_RATE_SIXTHS[ARP_RATES] = {1, 2, 3, 6, 8, 12, 16, 24};   /* sixths of a step */
static uint32_t arp_rng = 0x2545F491u;

static int arp_is_strum(int32_t mode) { return mode == ARP_STRUM_UP || mode == ARP_STRUM_DOWN; }

static uint32_t arp_rate_sixths(const pkt_t *p) { return ARP_RATE_SIXTHS[clamp(p->v[PV_ARP_RATE], 0, ARP_RATES - 1)]; }

static void arp_remove(arp_t *a, uint32_t i)
{
    a->n--;
    for (; i < a->n; i++)
        a->note[i] = a->note[i + 1u];
}

static void arp_remove_released(arp_t *a)
{
    uint32_t i = a->n;
    while (i--)
        if (a->note[i].released)
            arp_remove(a, i);
}

/* LEN_HOLD notes, which last until the track's next note */
static void arp_end_held(arp_t *a)
{
    uint32_t i = a->n;
    while (i--)
        if (a->note[i].owner == ARP_OWNER_STEP && !a->note[i].left)
            arp_remove(a, i);
}

/* gives the arp the packet's whole chord. first_wait applies when the arp was empty, or to a strum, which starts its pass again. A pitch already there is replaced, so a looping step does not stack up */
static void arp_note_on(arp_t *a, const pkt_t *p, uint32_t owner, uint32_t samples, int32_t first_wait)
{
    int8_t notes[CHORD_NOTES];
    uint32_t i, c, n = chord_notes(p, notes), keys_down = 0;
    int was_empty = !a->n;
    for (i = 0; i < a->n; i++)
        keys_down += a->note[i].owner != ARP_OWNER_STEP && !a->note[i].released;
    if (owner == ARP_OWNER_STEP)
        arp_end_held(a);
    else if (!keys_down)
        arp_remove_released(a);
    for (c = 0; c < n; c++) {
        arp_note_t *e;
        i = a->n;
        while (i--)
            if (a->note[i].pkt.v[PV_NOTE] == notes[c])
                arp_remove(a, i);
        if (a->n == ARP_NOTES)
            arp_remove(a, 0);
        e = &a->note[a->n++];
        e->pkt = *p;
        e->pkt.v[PV_NOTE] = notes[c];
        e->left = samples;
        e->owner = (uint8_t)owner;
        e->released = 0;
    }
    if (was_empty || arp_is_strum(p->v[PV_ARP_MODE])) {
        a->wait = first_wait;
        a->pos = 0;
    }
}

static void arp_key_off(arp_t *a, uint32_t key)
{
    uint32_t i = a->n;
    while (i--)
        if (a->note[i].owner == key && !a->note[i].released) {
            if (a->note[i].pkt.v[PV_ARP_LATCH])
                a->note[i].released = 1;
            else
                arp_remove(a, i);
        }
}

/* a held chord changes type: notes that stay keep the arp's place */
static void arp_key_change(arp_t *a, const pkt_t *p, uint32_t key)
{
    int8_t notes[CHORD_NOTES];
    uint32_t n = chord_notes(p, notes), i = a->n;
    while (i--)
        if (a->note[i].owner == key && !chord_has(notes, n, a->note[i].pkt.v[PV_NOTE]))
            arp_remove(a, i);
    arp_note_on(a, p, key, 0, 0);
}

/* where hit pos lands in the run of notes across the octaves: position % n is the note, position / n the octave */
static uint32_t arp_position(int32_t mode, uint32_t n, uint32_t range, uint32_t pos, uint32_t random)
{
    uint32_t run = n * range;
    if (mode == ARP_RANDOM)
        return random % run;
    if (mode == ARP_DOWN || mode == ARP_STRUM_DOWN)
        return run - 1u - pos % run;
    if (mode == ARP_UPDOWN && run > 1u) {
        uint32_t i = pos % (2u * run - 2u);      /* the top and bottom sound once a cycle */
        return i < run ? i : 2u * run - 2u - i;
    }
    return pos % run;
}

/* the index of the rank-th lowest note */
static uint32_t arp_ranked(const arp_t *a, uint32_t rank)
{
    uint8_t order[ARP_NOTES];
    uint32_t i, j;
    for (i = 0; i < a->n; i++) {
        for (j = i; j && a->note[order[j - 1u]].pkt.v[PV_NOTE] > a->note[i].pkt.v[PV_NOTE]; j--)
            order[j] = order[j - 1u];
        order[j] = (uint8_t)i;
    }
    return order[rank];
}

static void arp_hit(arp_t *a, uint32_t block, uint32_t samples_per_step, arp_emit_t emit)
{
    const pkt_t *newest = &a->note[a->n - 1u].pkt;
    int32_t mode = clamp(newest->v[PV_ARP_MODE], ARP_UP, ARP_MODES - 1);
    uint32_t range = (uint32_t)clamp(newest->v[PV_ARP_RANGE], 1, ARP_RANGE_MAX), spacing = samples_per_step * arp_rate_sixths(newest);
    uint32_t gate = (uint32_t)clamp(newest->v[PV_ARP_GATE], 0, 127), hold = spacing * gate / 127u / 6u / block, at;
    const arp_note_t *from;
    pkt_t p;
    a->wait += (int32_t)spacing;
    if (arp_is_strum(mode) && a->pos >= a->n * range) {
        a->wait = 0;                             /* the pass is over: the notes ring until they end */
        return;
    }
    arp_rng ^= arp_rng << 13;
    arp_rng ^= arp_rng >> 17;
    arp_rng ^= arp_rng << 5;
    at = arp_position(mode, a->n, range, a->pos, arp_rng);
    from = &a->note[mode == ARP_PLAYED ? at % a->n : arp_ranked(a, at % a->n)];
    p = from->pkt;
    p.v[PV_NOTE] = (int8_t)clamp(p.v[PV_NOTE] + 12 * (int32_t)(at / a->n), 0, 127);
    if (arp_is_strum(mode)) {
        uint32_t ring = from->left / block;      /* to the chord's end, whatever the GATE */
        emit(a, &p, from->owner, from->left && !ring ? 1u : ring);
    } else {
        emit(a, &p, from->owner, hold ? hold : 1u);
    }
    a->pos++;
}

/* a note leaves a block before its time, so the next step's note finds the arp empty and starts it, whatever the rounding to blocks */
static void arp_block(arp_t *a, uint32_t samples, uint32_t samples_per_step, arp_emit_t emit)
{
    uint32_t i;
    if (!a->n)
        return;
    if (a->wait <= 0)
        arp_hit(a, samples, samples_per_step, emit);
    a->wait -= (int32_t)(samples * 6u);
    i = a->n;
    while (i--)
        if (a->note[i].left) {
            if (a->note[i].left <= 2u * samples)
                arp_remove(a, i);
            else
                a->note[i].left -= samples;
        }
}
