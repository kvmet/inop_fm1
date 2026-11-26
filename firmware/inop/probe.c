/* SPDX-License-Identifier: GPL-3.0-only */
/* Test commands on the USB serial port, in INOP_PROBE builds. tools/inop_probe.py sends them and turns names into numbers.
 * Each command answers with text lines, then "ok" or "err". */
#define PROBE_ARGS 6u
#define PROBE_SEND_MS 1000u

static struct { char line[64]; uint32_t len; uint8_t tapping; } probe;

/* a dropped byte breaks a screenshot: the host sees a short read */
static void probe_putc(uint8_t c)
{
    uint32_t t0 = fm1_ms;
    while (co_w - co_r >= CO_N) {
        fm1_wdt_feed();
        if (!cdc.dtr || fm1_ms - t0 > PROBE_SEND_MS)
            return;
    }
    cdc_out[co_w % CO_N] = c;
    RING_PUBLISH();
    co_w++;
}

static void probe_puts(const char *s)
{
    while (*s)
        probe_putc((uint8_t)*s++);
}

static void probe_stat(const char *name, int32_t v)
{
    char b[16];
    probe_puts(name);
    probe_putc(' ');
    put_int(b, v);
    probe_puts(b);
    probe_putc('\n');
}

/* screen records: 'F' x y w h, color. 'R' x y w h, pixels as the panel takes them. 'E': the end */
static void probe_tap_head(uint8_t kind, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    probe_putc(kind);
    probe_putc((uint8_t)x);
    probe_putc((uint8_t)y);
    probe_putc((uint8_t)w);
    probe_putc((uint8_t)h);
}

static void probe_tap_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    if (!probe.tapping)
        return;
    probe_tap_head('F', x, y, w, h);
    probe_putc((uint8_t)(c >> 8));
    probe_putc((uint8_t)c);
}

static void probe_tap_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    const uint8_t *bytes = (const uint8_t *)px;
    uint32_t i;
    if (!probe.tapping)
        return;
    probe_tap_head('R', x, y, w, h);
    for (i = 0; i < w * h * 2u; i++)
        probe_putc(bytes[i]);
}

/* the device has no room for a frame, so it draws the whole screen again and sends each draw */
static void probe_shot(void)
{
    uint32_t i;
    probe.tapping = 1;
    lcd_fill(0, 0, 240, 240, T_BG);
    grid_invalidate();
    for (i = 0; i < METERS; i++)
        meter_drawn[i] = 0;
    for (i = 0; i < MASTER_BARS; i++)
        master_bar_drawn[i] = 0;
    follower_bar_drawn = 0;
    ui_draw();
    probe.tapping = 0;
    probe_putc('E');
}

/* each debounced press, with row PA7's history around it: a press no finger made shows what the scan saw */
#define PROBE_PRESSES 32u
#define PROBE_ROW_TRACED 3u
#define PROBE_HIST_BEFORE 16u
#define PROBE_HIST_AFTER 8u
#define PROBE_HIST (PROBE_HIST_BEFORE + PROBE_HIST_AFTER)
#define PROBE_HIST_IDLE (1u << 15)
static struct {
    uint32_t ms, frame;
    uint8_t id;
    uint16_t hist[PROBE_HIST];   /* per frame: bit c column c read closed, PROBE_HIST_IDLE read closed with no column driven */
} probe_presses[PROBE_PRESSES];
static volatile uint32_t probe_press_count, probe_press_ready;
static uint16_t probe_hist[PROBE_HIST];
static uint32_t probe_hist_frame;

static void probe_key_pressed(uint32_t id)
{
    uint32_t at = probe_press_count % PROBE_PRESSES;
    probe_presses[at].ms = fm1_ms;
    probe_presses[at].frame = probe_hist_frame;
    probe_presses[at].id = (uint8_t)id;
    probe_press_count++;
}

/* frames in which a row read closed with no column driven, per row */
static volatile uint32_t probe_row_idle_frames[6];

static void probe_row_idle(uint32_t rows)
{
    uint32_t r, c, row = 0;
    for (r = 0; r < 6u; r++)
        if (rows >> r & 1u)
            probe_row_idle_frames[r]++;
    for (c = 0; c < FM1_NCOL; c++)
        row |= (fm1_in.raw[c] >> PROBE_ROW_TRACED & 1u) << c;
    if (rows >> PROBE_ROW_TRACED & 1u)
        row |= PROBE_HIST_IDLE;
    probe_hist[probe_hist_frame % PROBE_HIST] = (uint16_t)row;
    probe_hist_frame++;
    while (probe_press_ready != probe_press_count &&
           probe_hist_frame - probe_presses[probe_press_ready % PROBE_PRESSES].frame >= PROBE_HIST_AFTER) {
        uint32_t at = probe_press_ready % PROBE_PRESSES, i;
        for (i = 0; i < PROBE_HIST; i++)
            probe_presses[at].hist[i] = probe_hist[(probe_presses[at].frame + PROBE_HIST_AFTER - PROBE_HIST + i) % PROBE_HIST];
        probe_press_ready++;
    }
}

/* the presses since the last call: a key id is a button below 14, else note key id - 14 */
static void probe_presses_report(void)
{
    static uint32_t shown;
    uint32_t count = probe_press_ready, i, f;
    if (count - shown > PROBE_PRESSES)
        shown = count - PROBE_PRESSES;
    for (i = shown; i != count; i++) {
        uint32_t at = i % PROBE_PRESSES;
        probe_stat("press_id", probe_presses[at].id);
        probe_stat("press_ms", (int32_t)probe_presses[at].ms);
        for (f = 0; f < PROBE_HIST; f++)
            probe_stat("press_hist", probe_presses[at].hist[f]);
    }
    shown = count;
}

static void probe_report(void)
{
    probe_stat("cpu_pct", (int32_t)(audio_cpu_q8 * 100u / 256u));
    probe_stat("audio_max_us", (int32_t)audio_max_us);
    probe_stat("audio_late", (int32_t)audio_late);
    probe_stat("voices", (int32_t)voices_active());
    probe_stat("voices_stolen", (int32_t)voices_stolen);
    probe_stat("voices_dropped", (int32_t)voices_dropped);
    probe_stat("voice_budget_us", (int32_t)voice_budget);
    probe_stat("cost_delay_us", (int32_t)cost_delay);
    probe_stat("cost_reverb_us", (int32_t)cost_reverb);
    probe_stat("cost_master_us", (int32_t)cost_master);
    probe_stat("row_idle_pa0", (int32_t)probe_row_idle_frames[0]);
    probe_stat("row_idle_pa5", (int32_t)probe_row_idle_frames[1]);
    probe_stat("row_idle_pa6", (int32_t)probe_row_idle_frames[2]);
    probe_stat("row_idle_pa7", (int32_t)probe_row_idle_frames[3]);
    probe_stat("row_idle_pa8", (int32_t)probe_row_idle_frames[4]);
    probe_stat("row_idle_pb7", (int32_t)probe_row_idle_frames[5]);
    probe_stat("cost_alone_fm", (int32_t)cost_alone[ENGINE_FM]);
    probe_stat("cost_alone_swarm", (int32_t)cost_alone[ENGINE_SWARM]);
    probe_stat("cost_alone_wave", (int32_t)cost_alone[ENGINE_WAVE]);
    probe_stat("cost_voice_fm", (int32_t)voice_cost[ENGINE_FM]);
    probe_stat("cost_voice_swarm", (int32_t)voice_cost[ENGINE_SWARM]);
    probe_stat("cost_voice_wave", (int32_t)voice_cost[ENGINE_WAVE]);
    probe_stat("cost_stereo_fm", (int32_t)stereo_cost[ENGINE_FM]);
    probe_stat("cost_stereo_swarm", (int32_t)stereo_cost[ENGINE_SWARM]);
    probe_stat("cost_stereo_wave", (int32_t)stereo_cost[ENGINE_WAVE]);
    probe_stat("bpm", (int32_t)ui.bpm);
    probe_stat("seq_run", seq.run);
    probe_stat("pattern", seq.playing);
    probe_stat("track", ui.track);
    probe_stat("page", (int32_t)(ui_page() - PAGES));
    probe_stat("slot", ui.slot);
    probe_stat("usb_audio_underruns", (int32_t)uac.underruns);
    probe_stat("usb_audio_missed", (int32_t)uac.missed);
    probe_stat("usb_audio_stalls", (int32_t)uac.stalls);
    probe_stat("usb_audio_overruns", (int32_t)uac.overruns);
    probe_stat("usb_audio_packets", (int32_t)uac.pkts);
    probe_stat("usb_audio_frames", (int32_t)uac.frames);
    probe_stat("usb_audio_adj_up", (int32_t)uac.adj_up);
    probe_stat("usb_audio_adj_down", (int32_t)uac.adj_down);
    probe_stat("usb_audio_fill_lo", (int32_t)uac.fill_lo);
    probe_stat("usb_audio_fill_hi", (int32_t)uac.fill_hi);
    probe_stat("ms", (int32_t)fm1_ms);
}

static int probe_track(int32_t pattern, int32_t track, track_pat_t **t)
{
    if (pattern < 0 || pattern >= (int32_t)NPATTERN || track < 0 || track >= (int32_t)NTRK)
        return 0;
    *t = &patterns[pattern].track[track];
    return 1;
}

static int probe_set_step(const int32_t *a)
{
    track_pat_t *t;
    step_t *s;
    uint32_t irq;
    if (!probe_track(a[0], a[1], &t) || a[2] < 0 || a[2] >= (int32_t)NSTEP)
        return 0;
    s = &t->steps[a[2]];
    irq = irq_save();
    switch (a[3]) {
    case 0: s->on = (uint8_t)a[4]; break;
    case 1: s->note = (uint8_t)a[4]; break;
    case 2: s->vel = (uint8_t)a[4]; break;
    case 3: s->len = (uint8_t)a[4]; break;
    case 4: s->sound = (uint8_t)a[4]; break;
    case 5: s->chance = (uint8_t)a[4]; break;
    case 6: s->hits = (uint8_t)clamp(a[4], 1, STEP_HITS_MAX); break;
    case 7: s->time = (int8_t)clamp(a[4], -SEQ_TIME_MAX, SEQ_TIME_MAX); break;
    default: irq_restore(irq); return 0;
    }
    irq_restore(irq);
    return 1;
}

static int probe_set_lock(const int32_t *a)
{
    track_pat_t *t;
    uint32_t irq;
    int full;
    if (!probe_track(a[0], a[1], &t) || a[2] < 0 || a[2] >= (int32_t)NSTEP || a[3] < 0 || a[3] >= (int32_t)PV_COUNT)
        return 0;
    irq = irq_save();
    full = step_write(t, (uint32_t)a[2], (uint32_t)a[3], a[4]);
    irq_restore(irq);
    return !full;
}

enum { PROBE_LENGTH, PROBE_BASE, PROBE_SWING, PROBE_HUMAN };

static int probe_track_field(const int32_t *a, int field)
{
    track_pat_t *t;
    if (!probe_track(a[0], a[1], &t))
        return 0;
    if (field == PROBE_LENGTH)
        t->length = (uint8_t)clamp(a[2], 1, NSTEP);
    else if (field == PROBE_BASE)
        t->base = (uint8_t)clamp(a[2], 0, NSOUND - 1);
    else if (field == PROBE_SWING)
        t->swing = (uint8_t)clamp(a[2], 0, SEQ_TIME_MAX);
    else
        t->human = (uint8_t)clamp(a[2], 0, SEQ_TIME_MAX);
    return 1;
}

static int probe_clear(int32_t pattern)
{
    uint32_t k, irq;
    if (pattern < 0 || pattern >= (int32_t)NPATTERN)
        return 0;
    irq = irq_save();
    for (k = 0; k < NTRK; k++)
        track_init(&patterns[pattern].track[k], patterns[pattern].track[k].base);
    irq_restore(irq);
    return 1;
}

static void probe_hold(volatile uint32_t *mask, int32_t bit, int32_t on)
{
    if (on)
        *mask |= 1u << bit;
    else
        *mask &= ~(1u << bit);
}

static void probe_turn(int32_t encoder, int32_t delta)
{
    uint32_t k = fm1__lock();
    fm1_in.enc_steps[encoder] = (int16_t)(fm1_in.enc_steps[encoder] + delta);
    fm1__unlock(k);
}

static void probe_zero(void)
{
    audio_max_us = 0;
    audio_late = 0;
    voices_stolen = 0;
    voices_dropped = 0;
    ui.drop_seen = 0;
}

/* the command's first word, or 0. Numbers go in a */
static uint32_t probe_parse(char *s, const char **name, int32_t *a)
{
    uint32_t n = 0;
    while (*s == ' ')
        s++;
    *name = s;
    while (*s && *s != ' ')
        s++;
    while (*s) {
        int32_t sign = 1, v = 0;
        *s++ = 0;
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        if (*s == '-') {
            sign = -1;
            s++;
        }
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (*s++ - '0');
        if (n < PROBE_ARGS)
            a[n++] = sign * v;
    }
    return n;
}

static int probe_is(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int probe_exec(char *line)
{
    const char *name;
    int32_t a[PROBE_ARGS] = {0};
    uint32_t n = probe_parse(line, &name, a);
    if (probe_is(name, "btn") && n == 2u && a[0] >= 0 && a[0] < 14)
        probe_hold(&probe_buttons, a[0], a[1]);
    else if (probe_is(name, "key") && n == 2u && a[0] >= 0 && a[0] < 27)
        probe_hold(&probe_notes, a[0], a[1]);
    else if (probe_is(name, "enc") && n == 2u && a[0] >= 0 && a[0] < (int32_t)FM1_NENC)
        probe_turn(a[0], a[1]);
    else if (probe_is(name, "step") && n == 5u)
        return probe_set_step(a);
    else if (probe_is(name, "lock") && n == 5u)
        return probe_set_lock(a);
    else if (probe_is(name, "len") && n == 3u)
        return probe_track_field(a, PROBE_LENGTH);
    else if (probe_is(name, "base") && n == 3u)
        return probe_track_field(a, PROBE_BASE);
    else if (probe_is(name, "swing") && n == 3u)
        return probe_track_field(a, PROBE_SWING);
    else if (probe_is(name, "human") && n == 3u)
        return probe_track_field(a, PROBE_HUMAN);
    else if (probe_is(name, "clear") && n == 1u)
        return probe_clear(a[0]);
    else if (probe_is(name, "snd") && n == 3u && a[0] >= 0 && a[0] < (int32_t)NSOUND && a[1] >= 0 && a[1] < (int32_t)PV_COUNT)
        sounds[a[0]].v[a[1]] = (int8_t)a[2];
    else if (probe_is(name, "glob") && n == 2u && a[0] >= 0 && a[0] < G_COUNT)
        ui_knob(ID_GLOBAL((uint32_t)a[0]), a[1] - globals[a[0]]);
    else if (probe_is(name, "bpm") && n == 1u)
        ui_knob(ID_BPM, a[0] - (int32_t)ui.bpm);
    else if (probe_is(name, "stat") && n == 0u)
        probe_report();
    else if (probe_is(name, "presses") && n == 0u)
        probe_presses_report();
    else if (probe_is(name, "zero") && n == 0u)
        probe_zero();
    else if (probe_is(name, "shot") && n == 0u)
        probe_shot();
    else
        return 0;
    return 1;
}

/* the main loop: each command finishes before the next is read, so the host paces a press against its release */
static void probe_service(void)
{
    while (ci_r != ci_w) {
        char c;
        RING_PUBLISH();
        c = (char)cdc_in[ci_r % CI_N];
        RING_PUBLISH();
        ci_r++;
        if (c != '\n') {
            if (probe.len + 1u < sizeof probe.line)
                probe.line[probe.len++] = c;
            continue;
        }
        probe.line[probe.len] = 0;
        probe.len = 0;
        probe_puts(probe_exec(probe.line) ? "ok\n" : "err\n");
    }
}
