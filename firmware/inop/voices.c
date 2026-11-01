/* SPDX-License-Identifier: GPL-3.0-only */
/* The voice pool. A voice renders only from its packet. */
#define KEY_NONE 0xFFu
#define KEY_TRACK(k) (0x80u | (k))   /* a held note's owner, where a played note has its key */
#define HOLD_CHECK_BLOCKS 32u
#define BUDGET_PERCENT 70u       /* of a block's time. The rest covers the sequencer, the key scan and flash writes */
typedef struct {
    pkt_t pkt, next;             /* next: the trigger that starts when this stolen voice has faded */
    uint8_t active, key, has_next, next_key;
    uint32_t age;
    uint32_t charge;             /* what this voice counts for against the budget */
    uint32_t hold_blocks, next_hold;   /* a sequenced note's blocks left before release; 0 = none */
    chain_t chain;
    union {
        fm_voice_t fm;
        wave_voice_t wave;
        perc_voice_t perc;
        swarm_voice_t swarm;
    } engine;
} voice_t;

static voice_t voices[NVOICE] __attribute__((section(".pool")));
static uint32_t voice_clock;
static uint32_t mod_clock;       /* render blocks since boot: free-running LFOs read it */
static uint32_t voice_cost[ENGINES];     /* ticks of one block, measured at boot */
static uint32_t swarm_base_cost, swarm_edge_cost;   /* ticks of a block with no edges, and ticks per edge: a SWARM note's cost depends on its pitch */
static uint32_t voice_budget = 0xFFFFFFFFu;
enum { COST_FILTER, COST_SHAPER, COST_DRIVE, COST_CRUSH, COST_RATE, COST_EFFECTS };
static uint32_t cost_alone[ENGINES], cost_shared[ENGINES];   /* ticks of a block with no effects: one voice, and one voice's share among all */
static uint32_t cost_effect[COST_EFFECTS], cost_delay, cost_reverb, cost_master;
static volatile uint32_t voices_dropped;   /* triggers that never sounded */
static volatile uint32_t voices_stolen;    /* voices cut short or never heard; one that had nearly ended does not count */

static AUDIO_INLINE int voice_engine(const pkt_t *p) { return clamp(p->v[PV_ENGINE], 0, ENGINES - 1); }

static uint32_t voice_charge(const pkt_t *p)
{
    return voice_engine(p) == ENGINE_SWARM ? swarm_base_cost + ((swarm_edges_q8(p) * swarm_edge_cost) >> 8) : voice_cost[voice_engine(p)];
}

static void voice_start(voice_t *v, const pkt_t *p, uint32_t key, uint32_t hold_blocks)
{
    v->pkt = *p;
    v->charge = voice_charge(p);
    v->key = (uint8_t)key;
    v->hold_blocks = hold_blocks;
    v->age = ++voice_clock;
    v->active = 1;
    v->has_next = 0;
    if (voice_engine(p) == ENGINE_PERC)
        perc_start(&v->engine.perc, &v->pkt, voice_clock * 2654435761u);
    else if (voice_engine(p) == ENGINE_SWARM)
        swarm_start(&v->engine.swarm, &v->pkt, voice_clock * 2654435761u);
    else if (voice_engine(p) == ENGINE_WAVE)
        wave_start(&v->engine.wave, &v->pkt, voice_clock * 2654435761u);
    else
        fm_start(&v->engine.fm, &v->pkt);
    chain_start(&v->chain, &v->pkt);
    if (voice_engine(p) == ENGINE_PERC)
        v->chain.amp_env.sustain = ENV_FULL;     /* the partials fall by AMP D and S; applying them here too would square the fall */
}

static AUDIO_INLINE void voice_release(voice_t *v)
{
    chain_release(&v->chain);
}

/* a stolen voice's envelope drops to 0, so its next block fades over the block's ramp and ends it */
static void voice_fade(voice_t *v)
{
    v->chain.amp_env.env = 0;
    v->chain.amp_env.stage = ENV_DONE;
}

static int voice_fading(const voice_t *v) { return v->chain.amp_env.stage == ENV_DONE; }

/* a Perc voice falls by its own envelopes while its amp envelope holds */
static int32_t voice_loudness(const voice_t *v)
{
    int32_t amp = v->chain.amp_env.env >> 9;
    return voice_engine(&v->pkt) == ENGINE_PERC ? (amp * (v->engine.perc.loudness >> 3)) >> 12 : amp;
}

static void voice_trigger(const pkt_t *p, uint32_t key, uint32_t hold_blocks)
{
    uint32_t cost = voice_charge(p), prio = (uint32_t)clamp(p->v[PV_PRIO], 0, PRIOS - 1), total = 0, i;
    voice_t *free_voice = 0, *stolen = 0;
    for (i = 0; i < NVOICE; i++) {
        if (!voices[i].active && !free_voice)
            free_voice = &voices[i];
        else if (voices[i].active && !voice_fading(&voices[i]))
            total += voices[i].charge;
    }
    while ((total && total + cost > voice_budget) || (!free_voice && !stolen)) {   /* one voice always sounds */
        steal_t info[NVOICE];
        int32_t victim;
        for (i = 0; i < NVOICE; i++) {
            const voice_t *v = &voices[i];
            info[i].stealable = v->active && !voice_fading(v) && !v->has_next;
            info[i].prio = (uint8_t)clamp(v->pkt.v[PV_PRIO], 0, PRIOS - 1);
            info[i].loudness = voice_loudness(v);
            info[i].age = v->age;
            info[i].rising = v->chain.amp_env.stage == ENV_ATTACK;
        }
        victim = voice_victim(info, NVOICE, prio);
        if (victim < 0) {
            voices_dropped++;
            return;
        }
        voices_stolen += info[victim].rising || info[victim].loudness >= NEARLY_SILENT;
        voice_fade(&voices[victim]);
        total -= voices[victim].charge;
        if (!stolen)
            stolen = &voices[victim];
    }
    if (stolen) {
        stolen->next = *p;
        stolen->next_key = (uint8_t)key;
        stolen->next_hold = hold_blocks;
        stolen->has_next = 1;
    } else {
        voice_start(free_voice, p, key, hold_blocks);
    }
}

static void voice_release_key(uint32_t key)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (voices[i].active && voices[i].key == key && voices[i].chain.amp_env.stage < ENV_RELEASE)
            voice_release(&voices[i]);
}

static int pkt_same(const pkt_t *a, const pkt_t *b)
{
    uint32_t i;
    for (i = 0; i < PV_COUNT && a->v[i] == b->v[i]; i++)
        ;
    return i == PV_COUNT;
}

/* a track's trigger. A new note ends the track's held note, unless it is the same held note again: that one keeps sounding,
 * so a pattern can loop under a drone */
static void voice_trigger_step(const pkt_t *p, uint32_t track, uint32_t samples_per_step)
{
    uint32_t key = KEY_TRACK(track), i;
    if (p->v[PV_LEN] != (int8_t)LEN_HOLD) {
        voice_release_key(key);
        voice_trigger(p, KEY_NONE, (uint32_t)p->v[PV_LEN] * samples_per_step / CTL);
        return;
    }
    for (i = 0; i < NVOICE; i++)
        if (voices[i].active && voices[i].key == key && voices[i].chain.amp_env.stage < ENV_RELEASE && pkt_same(&voices[i].pkt, p))
            return;
    voice_release_key(key);
    voice_trigger(p, key, 0);
}

/* a held note also ends when its track has no steps left to end it. One track per check: this runs inside the audio block */
static void voices_hold_service(const pattern_t *pattern)
{
    static uint32_t clock;
    uint32_t k, i;
    const track_pat_t *t;
    if (++clock % HOLD_CHECK_BLOCKS)
        return;
    k = (clock / HOLD_CHECK_BLOCKS) % NTRK;
    t = &pattern->track[k];
    for (i = 0; i < t->length; i++)
        if (t->steps[i].on)
            return;
    voice_release_key(KEY_TRACK(k));
}

static void voices_silence(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++) {
        voices[i].has_next = 0;
        voice_fade(&voices[i]);
    }
}

static uint32_t voices_active(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += voices[i].active;
    return n;
}

AUDIO_RAM static void voice_render(voice_t *v, int32_t *l, int32_t *r, int32_t *delay_bus, int32_t *reverb_bus)
{
    int32_t x[CTL], alive = 1;
    uint32_t k;
    if (v->hold_blocks && !--v->hold_blocks)
        voice_release(v);
    for (k = 0; k < CTL; k++)
        x[k] = 0;
    chain_mod(&v->chain, &v->pkt, mod_clock);
    if (voice_engine(&v->pkt) == ENGINE_PERC)
        alive = perc_render(&v->engine.perc, &v->pkt, x, v->chain.off[MOD_DST_PITCH] >> 7, v->chain.off[MOD_DST_TIMBRE] >> 8);
    else if (voice_engine(&v->pkt) == ENGINE_SWARM)
        swarm_render(&v->engine.swarm, &v->pkt, x, v->chain.off[MOD_DST_PITCH] >> 7, v->chain.off[MOD_DST_TIMBRE] >> 8);
    else if (voice_engine(&v->pkt) == ENGINE_WAVE)
        wave_render(&v->engine.wave, &v->pkt, x, v->chain.off[MOD_DST_PITCH] >> 7, v->chain.off[MOD_DST_TIMBRE] >> 8);
    else
        fm_render(&v->engine.fm, &v->pkt, x, v->chain.off[MOD_DST_PITCH] >> 7, v->chain.mod_env.env >> 9, v->chain.off[MOD_DST_TIMBRE] >> 8);
    alive &= chain_render(&v->chain, &v->pkt, x, l, r, delay_bus, reverb_bus);
    if (alive)
        return;
    v->active = 0;
    if (v->has_next)
        AUDIO_FAR(voice_start)(v, &v->next, v->next_key, v->next_hold);
}

/* adds one block of every voice to the mix and to the send buses */
static void voices_render(int32_t *l, int32_t *r, int32_t *delay_bus, int32_t *reverb_bus)
{
    uint32_t i;
    mod_clock++;
    for (i = 0; i < NVOICE; i++)
        if (voices[i].active)
            AUDIO_FAR(voice_render)(&voices[i], l, r, delay_bus, reverb_bus);
}

static uint32_t voice_slowest_block(voice_t *v, const pkt_t *p)
{
    enum { WARM_BLOCKS = 4, MEASURE_BLOCKS = 32 };
    int32_t l[CTL], r[CTL], d[CTL], w[CTL];
    uint32_t worst = 0, b, i;
    voice_start(v, p, KEY_NONE, 0);
    for (b = 0; b < WARM_BLOCKS + MEASURE_BLOCKS; b++) {
        uint32_t t0, dt;
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        t0 = fm1_ticks();
        AUDIO_FAR(voice_render)(v, l, r, d, w);
        dt = fm1_ticks() - t0;
        if (b >= WARM_BLOCKS && dt > worst)
            worst = dt;
    }
    return worst;
}

/* the most an engine's own controls can ask, with no effect of the chain on */
static void voice_bare_packet(pkt_t *p, uint32_t variant)
{
    sound_init(p, 0);
    p->v[PV_NOTE] = 36;
    p->v[PV_VEL] = 127;
    p->v[PV_LEN] = 1;
    p->v[PV_AMP_S] = 127;
    p->v[PV_FLT_TYPE] = FLT_OFF;
    p->v[PV_DRIVE] = p->v[PV_SHAPER] = p->v[PV_CRUSH] = p->v[PV_RATE] = 0;
    p->v[PV_SEND_DELAY] = p->v[PV_SEND_REVERB] = 0;
    p->v[PV_ENGINE] = (int8_t)variant;
    if (variant == ENGINE_SWARM) {               /* sync, the second saw of a pulse, the sub and drift */
        p->v[PV_E0] = SWARM_OCTAVES;
        p->v[PV_E1] = p->v[PV_E4] = p->v[PV_E5] = p->v[PV_E6] = 127;
        p->v[PV_E2] = 90;
        p->v[PV_E3] = 64;
    } else if (variant == ENGINE_PERC) {         /* every partial sounding, with every stage on */
        p->v[PV_E0] = PERC_METAL;
        p->v[PV_E1] = 127;
        p->v[PV_E2] = 30;
        p->v[PV_E3] = 127;
        p->v[PV_E4] = p->v[PV_E5] = p->v[PV_E6] = p->v[PV_E7] = 64;
    } else if (variant == ENGINE_WAVE) {         /* both oscillators between frames, bent, with FM and noise */
        p->v[PV_E1] = p->v[PV_E2] = 64;
        p->v[PV_E3] = 7;
        p->v[PV_E4] = 40;
        p->v[PV_E5] = p->v[PV_E7] = 100;
    } else {                                     /* the algorithm with the most mixes, and with feedback */
        p->v[PV_E0] = FM_ALGS - 1u;
        p->v[PV_E4] = p->v[PV_E5] = p->v[PV_E6] = 64;
    }
}

/* one voice's share of a block when every voice plays this engine, on different notes and tables. Against a voice alone,
 * this shows what the voices cost each other in the cache */
static uint32_t voices_slowest_shared(const pkt_t *p)
{
    enum { WARM_BLOCKS = 4, MEASURE_BLOCKS = 32 };
    int32_t l[CTL], r[CTL], d[CTL], w[CTL];
    uint32_t worst = 0, b, i;
    for (i = 0; i < NVOICE; i++) {
        pkt_t q = *p;
        q.v[PV_NOTE] = (int8_t)(p->v[PV_NOTE] + (int32_t)i * 2);
        if (voice_engine(p) == ENGINE_WAVE)
            q.v[PV_E0] = (int8_t)(i % WAVE_TABLES);
        voice_start(&voices[i], &q, KEY_NONE, 0);
    }
    for (b = 0; b < WARM_BLOCKS + MEASURE_BLOCKS; b++) {
        uint32_t t0, dt;
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 0;
        t0 = fm1_ticks();
        voices_render(l, r, d, w);
        dt = fm1_ticks() - t0;
        if (b >= WARM_BLOCKS && dt > worst)
            worst = dt;
    }
    for (i = 0; i < NVOICE; i++)
        voices[i].active = 0;
    return worst / NVOICE;
}

/* measures each engine, each effect and the shared effects on this hardware. Runs before audio and interrupts start.
 * A voice's cost has every effect on, so the budget holds for every sound */
static void voices_measure(void)
{
    enum { WARM_BLOCKS = 4, MEASURE_BLOCKS = 32 };
    static const uint8_t EFFECT_ID[COST_EFFECTS] = {PV_FLT_TYPE, PV_SHAPER, PV_DRIVE, PV_CRUSH, PV_RATE};
    int32_t l[CTL], r[CTL], d[CTL], w[CTL];
    uint32_t variant, b, i;
    voice_t *v = &voices[0];
    pkt_t p;
    for (variant = 0; variant < ENGINES; variant++) {
        voice_bare_packet(&p, variant);
        cost_alone[variant] = voice_slowest_block(v, &p);
        cost_shared[variant] = voices_slowest_shared(&p);
    }
    voice_bare_packet(&p, ENGINE_FM);
    for (i = 0; i < COST_EFFECTS; i++) {
        pkt_t q = p;
        uint32_t with;
        q.v[EFFECT_ID[i]] = i == COST_FILTER ? FLT_BP : 64;
        with = voice_slowest_block(v, &q);
        cost_effect[i] = with > cost_alone[ENGINE_FM] ? with - cost_alone[ENGINE_FM] : 0u;
    }
    for (variant = 0; variant < ENGINES; variant++) {
        voice_bare_packet(&p, variant);
        p.v[PV_FLT_TYPE] = FLT_BP;
        p.v[PV_DRIVE] = p.v[PV_SHAPER] = p.v[PV_CRUSH] = p.v[PV_RATE] = 64;
        voice_cost[variant] = voice_slowest_block(v, &p);
        if (variant == ENGINE_SWARM) {               /* from a low note and a high one */
            uint32_t low_edges = swarm_edges_q8(&p), low = voice_cost[variant], high, high_edges;
            p.v[PV_NOTE] = 84;
            high = voice_slowest_block(v, &p);
            high_edges = swarm_edges_q8(&p);
            swarm_edge_cost = high > low ? (high - low) * 256u / (high_edges - low_edges) + 1u : 1u;
            swarm_base_cost = low;
        }
    }
    v->active = 0;
    for (b = 0; b < WARM_BLOCKS + MEASURE_BLOCKS; b++) {
        uint32_t t0, t1, t2, t3;
        master_t m = {MASTER_LIMIT};
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 10000;
        t0 = fm1_ticks();
        AUDIO_FAR(delay_block)(&delay_fx, d, l, r, 1000, 64);
        t1 = fm1_ticks();
        AUDIO_FAR(reverb_block)(&reverb_fx, w, l, r, 64, 64);
        t2 = fm1_ticks();
        AUDIO_FAR(master_block)(&m, l, r);
        t3 = fm1_ticks();
        if (b >= WARM_BLOCKS) {
            cost_delay = t1 - t0 > cost_delay ? t1 - t0 : cost_delay;
            cost_reverb = t2 - t1 > cost_reverb ? t2 - t1 : cost_reverb;
            cost_master = t3 - t2 > cost_master ? t3 - t2 : cost_master;
        }
    }
    memset(&delay_fx, 0, sizeof delay_fx);
    memset(&reverb_fx, 0, sizeof reverb_fx);
    b = CTL * 1000000u / FS * FM1_TICKS_PER_US * BUDGET_PERCENT / 100u;
    voice_budget = b > cost_delay + cost_reverb + cost_master ? b - cost_delay - cost_reverb - cost_master : 0u;
}
