/* SPDX-License-Identifier: GPL-3.0-only */
/* The voice pool. A voice renders only from its packet. */
#define KEY_NONE 0xFFu
#define BUDGET_PERCENT 70u       /* of a block's time. The rest covers the sequencer, the key scan and flash writes */
#define COST_VARIANT_PERC (1u + PM_COUNT)
#define COST_VARIANTS (COST_VARIANT_PERC + 1u)
typedef struct {
    pkt_t pkt, next;             /* next: the trigger that starts when this stolen voice has faded */
    uint8_t active, key, has_next, next_key;
    uint32_t age;
    uint32_t hold_blocks, next_hold;   /* a sequenced note's blocks left before release; 0 = none */
    chain_t chain;
    union {
        fm_voice_t fm;
        phys_voice_t phys;
        perc_voice_t perc;
    } engine;
} voice_t;

static voice_t voices[NVOICE] __attribute__((section(".pool")));
static uint32_t voice_clock;
static uint32_t mod_clock;       /* render blocks since boot: free-running LFOs read it */
static uint32_t voice_cost[COST_VARIANTS];     /* ticks of one block, measured at boot */
static uint32_t voice_budget = 0xFFFFFFFFu;
static volatile uint32_t voices_dropped;   /* triggers that never sounded */
static volatile uint32_t voices_stolen;    /* sounding voices cut short; a nearly silent one does not count */

/* how many voices of one variant the budget holds */
static uint32_t voices_fit(uint32_t variant)
{
    uint32_t n = voice_cost[variant] ? voice_budget / voice_cost[variant] : NVOICE;
    return n > NVOICE ? NVOICE : n;
}

static int voice_engine(const pkt_t *p) { return clamp(p->v[PV_ENGINE], 0, ENGINES - 1); }

static uint32_t cost_variant(const pkt_t *p)
{
    int engine = voice_engine(p);
    return engine == ENGINE_PERC ? COST_VARIANT_PERC : engine == ENGINE_PHYS ? 1u + phys_model(p) : 0u;
}

static void voice_start(voice_t *v, const pkt_t *p, uint32_t key, uint32_t hold_blocks)
{
    v->pkt = *p;
    v->key = (uint8_t)key;
    v->hold_blocks = hold_blocks;
    v->age = ++voice_clock;
    v->active = 1;
    v->has_next = 0;
    if (voice_engine(p) == ENGINE_PERC)
        perc_start(&v->engine.perc, &v->pkt, voice_clock * 2654435761u);
    else if (voice_engine(p) == ENGINE_PHYS)
        phys_start(&v->engine.phys, &v->pkt, voice_clock * 2654435761u);
    else
        fm_start(&v->engine.fm, &v->pkt);
    chain_start(&v->chain, &v->pkt);
}

static void voice_release(voice_t *v)
{
    if (voice_engine(&v->pkt) == ENGINE_PERC)
        return;
    if (voice_engine(&v->pkt) == ENGINE_FM)
        fm_release(&v->engine.fm);
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
    uint32_t cost = voice_cost[cost_variant(p)], prio = (uint32_t)clamp(p->v[PV_PRIO], 0, PRIOS - 1), total = 0, i;
    voice_t *free_voice = 0, *stolen = 0;
    for (i = 0; i < NVOICE; i++) {
        if (!voices[i].active && !free_voice)
            free_voice = &voices[i];
        else if (voices[i].active && !voice_fading(&voices[i]))
            total += voice_cost[cost_variant(&voices[i].pkt)];
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
        }
        victim = voice_victim(info, NVOICE, prio);
        if (victim < 0) {
            voices_dropped++;
            return;
        }
        voices_stolen += info[victim].loudness >= NEARLY_SILENT;
        voice_fade(&voices[victim]);
        total -= voice_cost[cost_variant(&voices[victim].pkt)];
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

static uint32_t voices_active(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += voices[i].active;
    return n;
}

static void voice_render(voice_t *v, int32_t *l, int32_t *r, int32_t *delay_bus, int32_t *reverb_bus)
{
    int32_t x[CTL], vl[CTL], vr[CTL], delay_q8, reverb_q8, alive = 1;
    uint32_t k;
    if (v->hold_blocks && !--v->hold_blocks)
        voice_release(v);
    for (k = 0; k < CTL; k++)
        x[k] = vl[k] = vr[k] = 0;
    chain_mod(&v->chain, &v->pkt, mod_clock);
    if (voice_engine(&v->pkt) == ENGINE_PERC)
        alive = perc_render(&v->engine.perc, &v->pkt, x, v->chain.off[MOD_DST_PITCH] >> 7);
    else if (voice_engine(&v->pkt) == ENGINE_PHYS)
        phys_render(&v->engine.phys, &v->pkt, x, v->chain.off[MOD_DST_PITCH] >> 7);
    else
        alive = fm_render(&v->engine.fm, &v->pkt, x, v->chain.off[MOD_DST_PITCH] >> 7);
    alive &= chain_render(&v->chain, &v->pkt, x, vl, vr);
    delay_q8 = clamp(v->pkt.v[PV_SEND_DELAY], 0, 127) * 2;
    reverb_q8 = clamp(v->pkt.v[PV_SEND_REVERB], 0, 127) * 2;
    for (k = 0; k < CTL; k++) {
        int32_t mono = (vl[k] + vr[k]) >> 1;
        l[k] += vl[k];
        r[k] += vr[k];
        delay_bus[k] += (mono * delay_q8) >> 8;
        reverb_bus[k] += (mono * reverb_q8) >> 8;
    }
    if (alive)
        return;
    v->active = 0;
    if (v->has_next)
        voice_start(v, &v->next, v->next_key, v->next_hold);
}

/* adds one block of every voice to the mix and to the send buses */
static void voices_render(int32_t *l, int32_t *r, int32_t *delay_bus, int32_t *reverb_bus)
{
    uint32_t i;
    mod_clock++;
    for (i = 0; i < NVOICE; i++)
        if (voices[i].active)
            voice_render(&voices[i], l, r, delay_bus, reverb_bus);
}

/* measures each engine's slowest block with every effect on, and the shared effects, on this hardware. Runs before audio
 * and interrupts start. The packet is the most any sound can ask of the chain, so the budget holds for all of them */
static void voices_measure(void)
{
    enum { WARM_BLOCKS = 4, MEASURE_BLOCKS = 32 };
    int32_t l[CTL], r[CTL], d[CTL], w[CTL];
    uint32_t variant, b, i, fx_cost = 0;
    voice_t *v = &voices[0];
    for (variant = 0; variant < COST_VARIANTS; variant++) {
        pkt_t p;
        uint32_t worst = 0;
        sound_init(&p, 0);
        p.v[PV_NOTE] = 36;
        p.v[PV_VEL] = 127;
        p.v[PV_LEN] = 1;
        p.v[PV_FLT_TYPE] = FLT_BP;
        p.v[PV_DRIVE] = p.v[PV_SHAPER] = p.v[PV_CRUSH] = p.v[PV_RATE] = 64;
        p.v[PV_ENGINE] = variant == COST_VARIANT_PERC ? ENGINE_PERC : variant ? ENGINE_PHYS : ENGINE_FM;
        if (variant == COST_VARIANT_PERC) {          /* every partial sounding, with every stage on */
            p.v[PV_E0] = PERC_METAL;
            p.v[PV_E1] = 127;
            p.v[PV_E2] = 30;
            p.v[PV_E3] = 127;
            p.v[PV_E4] = p.v[PV_E5] = p.v[PV_E6] = p.v[PV_E7] = 64;
        } else if (variant) {
            p.v[PV_E0] = (int8_t)(variant - 1u);
        }
        voice_start(v, &p, KEY_NONE, 0);
        for (b = 0; b < WARM_BLOCKS + MEASURE_BLOCKS; b++) {
            uint32_t t0, dt;
            for (i = 0; i < CTL; i++)
                l[i] = r[i] = d[i] = w[i] = 0;
            t0 = fm1_ticks();
            voice_render(v, l, r, d, w);
            dt = fm1_ticks() - t0;
            if (b >= WARM_BLOCKS && dt > worst)
                worst = dt;
        }
        voice_cost[variant] = worst;
    }
    v->active = 0;
    for (b = 0; b < WARM_BLOCKS + MEASURE_BLOCKS; b++) {
        uint32_t t0, dt;
        master_t m = {MASTER_LIMIT};
        for (i = 0; i < CTL; i++)
            l[i] = r[i] = d[i] = w[i] = 10000;
        t0 = fm1_ticks();
        delay_block(&delay_fx, d, l, r, 1000, 64);
        reverb_block(&reverb_fx, w, l, r, 64, 64);
        for (i = 0; i < CTL; i++)
            master_sample(&m, &l[i], &r[i]);
        dt = fm1_ticks() - t0;
        if (b >= WARM_BLOCKS && dt > fx_cost)
            fx_cost = dt;
    }
    memset(&delay_fx, 0, sizeof delay_fx);
    memset(&reverb_fx, 0, sizeof reverb_fx);
    b = CTL * 1000000u / FS * FM1_TICKS_PER_US * BUDGET_PERCENT / 100u;
    voice_budget = b > fx_cost ? b - fx_cost : 0u;
}
