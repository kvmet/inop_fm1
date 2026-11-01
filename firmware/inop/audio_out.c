/* SPDX-License-Identifier: GPL-3.0-only */
/* I2S output and the audio ISR. */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define OUT_SHIFT 7              /* Q15 to 24-bit, -6 dBFS ceiling */
#define CPU_AVG (4096u / HALF_FRAMES)

static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static volatile uint32_t t5_nested_ticks;
static volatile uint32_t audio_in_isr, audio_late, audio_max_us, audio_cpu_q8;
static uint32_t audio_cpu_rem;
static volatile uint32_t master_q12 = 2048u;

static sound_t sounds[NSOUND] __attribute__((section(".pool")));
static pattern_t patterns[NPATTERN] __attribute__((section(".pool")));
static seq_t seq;
static master_t master = MASTER_INIT;
static volatile int8_t globals[G_COUNT];
static volatile uint8_t keys_play;       /* 1: the keys play notes on keys_track */
static volatile uint8_t keys_track;
static volatile int8_t keys_octave;
static volatile uint32_t keys_mask;      /* the keys that play notes */
enum { METER_L, METER_R, METER_DELAY, METER_REVERB, METERS };
static volatile uint32_t meter_peak[METERS];     /* the panel reads and clears these */

static uint32_t seq_emit(uint32_t track, uint32_t step, const track_pat_t *t, uint32_t hits)
{
    pkt_t p;
    packet_resolve(&p, sounds, t, step);
    return voice_trigger_step(&p, track, seq.samples_per_step, hits);
}

/* keys poll here so a press waits at most one block */
static int32_t keys_note(uint32_t k) { return 53 + (int32_t)k + 12 * keys_octave; }   /* key 0 is F3 at octave 0 */

/* a played arp starts on its rate's next boundary, so it stays on the sequencer's grid. In sixths of a sample, as arp_t.wait */
static int32_t keys_arp_wait(const pkt_t *p)
{
    uint32_t cycle = LFO_SYNC_UNITS_PER_STEP / 6u * arp_rate_sixths(p), into = seq_sync_pos(&seq) % cycle;
    return seq.run == SEQ_RUNNING && into ? (int32_t)((cycle - into) * seq.samples_per_step / (LFO_SYNC_UNITS_PER_STEP / 6u)) : 0;
}

static void keys_block(void)
{
    static uint32_t held;
    uint32_t now = keys_play ? fm1_in.notes & keys_mask : 0u, changed = now ^ held, k, i;
    const sound_t *base = &sounds[patterns[seq.playing].track[keys_track].base];
    held = now;
    if (base->v[PV_ARP_MODE] == ARP_OFF || !base->v[PV_ARP_LATCH])
        arp_remove_released(&arps[keys_track]);
    for (k = 0; changed; k++, changed >>= 1) {
        if (!(changed & 1u))
            continue;
        if ((now >> k) & 1u) {
            pkt_t p = *base;
            p.v[PV_NOTE] = (int8_t)keys_note(k);
            if (p.v[PV_ARP_MODE] != ARP_OFF)
                arp_note_on(&arps[keys_track], &p, k, 0, keys_arp_wait(&p));
            else
                voice_trigger(&p, k, 0);
        } else {
            voice_release_key(k);
            for (i = 0; i < NTRK; i++)
                arp_key_off(&arps[i], k);
        }
    }
}

static inline int32_t abs_max(int32_t peak, int32_t x)
{
    x = x < 0 ? -x : x;
    return x > peak ? x : peak;
}

static void audio_block(int32_t *out)
{
    int32_t l[CTL], r[CTL], delay_bus[CTL], reverb_bus[CTL], peak[METERS] = {0, 0, 0, 0};
    master_params_t mp = {globals[G_MST_COMP], globals[G_MST_ATTACK], globals[G_MST_RELEASE], globals[G_MST_TILT], globals[G_MST_WIDTH], globals[G_MST_DRIVE], (int32_t)master_q12};
    uint32_t i;
    for (i = 0; i < CTL; i++)
        l[i] = r[i] = delay_bus[i] = reverb_bus[i] = 0;
    keys_block();
    seq_block(&seq, patterns, CTL, seq_emit);
    for (i = 0; i < NTRK; i++)
        arp_block(&arps[i], CTL, seq.samples_per_step, voice_arp_hit);
    lfo_sync_pos = seq_sync_pos(&seq);
    voices_hold_service(&patterns[seq.playing]);
    voices_render(l, r, delay_bus, reverb_bus);
    follower_block(globals[G_FOL_ATTACK], globals[G_FOL_RELEASE]);
    AUDIO_FAR(delay_block)(&delay_fx, delay_bus, l, r, (uint32_t)globals[G_DELAY_TIME] * seq.samples_per_step, globals[G_DELAY_FB], globals[G_DELAY_WIDTH]);
    AUDIO_FAR(reverb_block)(&reverb_fx, reverb_bus, l, r, globals[G_REVERB_SIZE], globals[G_REVERB_DAMP]);
#if FELUCCA_UAC
    mp.volume_q12 = MASTER_VOLUME_FULL_Q12;      /* a recording's level does not depend on the MASTER knob */
#endif
    AUDIO_FAR(master_block)(&master, l, r, &mp);
#if FELUCCA_UAC
    {
        int32_t usb_out[2u * CTL];
        for (i = 0; i < CTL; i++) {
            usb_out[2u * i] = l[i];
            usb_out[2u * i + 1u] = r[i];
            l[i] = (l[i] * (int32_t)master_q12) >> 12;
            r[i] = (r[i] * (int32_t)master_q12) >> 12;
        }
        uac_tap(usb_out, CTL);
    }
#endif
    for (i = 0; i < CTL; i++) {
        out[2u * i] = l[i] * (1 << OUT_SHIFT);
        out[2u * i + 1u] = r[i] * (1 << OUT_SHIFT);
        peak[METER_L] = abs_max(peak[METER_L], l[i]);
        peak[METER_R] = abs_max(peak[METER_R], r[i]);
        peak[METER_DELAY] = abs_max(peak[METER_DELAY], delay_bus[i]);
        peak[METER_REVERB] = abs_max(peak[METER_REVERB], reverb_bus[i]);
    }
    for (i = 0; i < METERS; i++)
        if ((uint32_t)peak[i] > meter_peak[i])
            meter_peak[i] = (uint32_t)peak[i];
}

void fm1_alnk0_irq(void)
{
    uint8_t p;
    uint32_t t0;
    audio_in_isr = 1;
    p = fm1_audio_pending();
    t0 = fm1_ticks();
    t5_nested_ticks = 0;
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), b, us, load;
        int32_t *o = &abuf[half * HALF_WORDS];
#if FELUCCA_UAC
        uac_render_start();
#endif
        for (b = 0; b < HALF_FRAMES; b += CTL)
            audio_block(o + 2u * b);
        fm1_audio_ack_half();
        us = (fm1_ticks() - t0 - t5_nested_ticks) / FM1_TICKS_PER_US;
        if (us > audio_max_us)
            audio_max_us = us;
        load = audio_cpu_q8 * (CPU_AVG - 1u) + (us * 256u) / (HALF_FRAMES * 1000000u / FS) + audio_cpu_rem;
        audio_cpu_q8 = load / CPU_AVG;
        audio_cpu_rem = load % CPU_AVG;
        if (fm1_audio_free_half() != half)
            audio_late++;
    }
    audio_in_isr = 0;
}
extern void isr_alnk0(void);

static void audio_init(void)
{
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}
