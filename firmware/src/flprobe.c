/* SPDX-License-Identifier: GPL-3.0-only */
/* FELUCCA_FLPROBE: console commands that measure whether a flash erase can be split around the audio ISR
 * (docs/dev/never-stop.md). Included by console.c. Erases and programs FLP_OFF only: the last sector of sample slot 3,
 * refused when that slot holds a sample. */
#define FLP_OFF (SMP_USER_BASE + 3u * SMP_USER_SIZE - 0x1000u)
#define FLP_SLOT3 (SMP_USER_BASE + 2u * SMP_USER_SIZE)
#define FLP_PAGES 16u                                   /* a pattern chunk's size, about */

#include "fm1_flash_probe.h"

static uint8_t flp_buf[256];
static int flp_slot_free(void)
{
    uint32_t magic = 0;
    if (!flash_ok || st_read(FLP_SLOT3, &magic, 4u))
        return 0;
    if (magic == SMP_USER_MAGIC) {
        con_puts("sample slot 3 holds a sample: refused\r\n");
        return 0;
    }
    return 1;
}
static void flp_wait_half(void)
{
    uint32_t h = audio_halves;
    while (audio_halves == h)
        ;
    fm1_wdt_feed();
}
static int flp_blank(void)
{
    uint32_t a, i;
    for (a = 0; a < 0x1000u; a += 256u) {
        if (st_read(FLP_OFF + a, flp_buf, 256u))
            return 0;
        for (i = 0; i < 256u; i++)
            if (flp_buf[i] != 0xFFu)
                return 0;
    }
    return 1;
}
static void flp_fill(uint32_t seed)
{
    uint32_t i;
    for (i = 0; i < 256u; i++)
        flp_buf[i] = (uint8_t)(seed + i);
}

/* flp id: the part and its status registers */
static void flp_id(void)
{
    uint32_t f = irq_save(), id = FL_FAR(fl_jedec_ram)(), sr = FL_FAR(fl_status_ram)();
    irq_restore(f);
    con_kx("jedec", id);
    con_kx("sr2_sr1", sr);
}

/* flp base: one plain erase, as the firmware does it now */
static void flp_base(void)
{
    uint32_t took = 0, f, i;
    if (!flp_slot_free())
        return;
    for (i = 0; i < 3u; i++) {
        flp_fill(i);
        (void)fl_write(FLP_OFF, flp_buf, 256u);
        f = irq_save();
        audio_silence();
        (void)FL_FAR(fl_erase4k_ram)(FLP_OFF, &took);
        irq_restore(f);
        fm1_wdt_feed();
        con_kv("erase_us", (int32_t)took);
    }
    flp_fill(0);
    (void)fl_write(FLP_OFF, flp_buf, 256u);
    {
        uint32_t t0 = fm1_ticks();
        f = irq_save();
        (void)FL_FAR(fl_prog_ram)(FLP_OFF + 256u, flp_buf, 256u, &took);
        irq_restore(f);
        con_kv("page_busy_us", (int32_t)took);
        con_kv("page_all_us", (int32_t)((fm1_ticks() - t0) / FM1_TICKS_PER_US));
    }
    f = irq_save();
    audio_silence();
    (void)FL_FAR(fl_erase4k_ram)(FLP_OFF, &took);
    irq_restore(f);
    con_kv("blank", flp_blank());
}

/* flp sus RUN_US: suspend and resume with the audio silent */
static void flp_sus(uint32_t run_us)
{
    static flp_off_t r;
    uint32_t f;
    int rc;
    if (!flp_slot_free())
        return;
    flp_fill(0x5Au);
    (void)fl_write(FLP_OFF, flp_buf, 256u);
    memset(&r, 0, sizeof r);
    fm1_wdt_feed();
    f = irq_save();
    audio_silence();
    rc = FL_FAR(flp_sus_ram)(FLP_OFF, run_us, &r);
    irq_restore(f);
    fm1_wdt_feed();
    con_kv("rc", rc);
    con_kx("sr_idle", r.sr_idle);
    con_kx("sr_suspended", r.sr_sus);
    con_kv("other_read_same", r.other0 == r.other1);
    con_kv("slices", (int32_t)r.slices);
    con_kv("suspend_max_us", (int32_t)r.sus_max_us);
    con_kv("total_us", (int32_t)r.total_us);
    con_kv("gave_up", (int32_t)r.gave_up);
    con_kv("blank", flp_blank());
}

static uint32_t flp_late0, flp_shed0;
static void flp_mark(uint32_t hog_us)
{
    flp_hog_ticks = hog_us * FM1_TICKS_PER_US;
    flp_wait_half();
    flp_wait_half();
    audio_max_us = 0;
    flp_late0 = felucca_dbg.late;
    flp_shed0 = shed_count;
}
static void flp_report(void)
{
    con_kv("late", (int32_t)(felucca_dbg.late - flp_late0));
    con_kv("shed", (int32_t)(shed_count - flp_shed0));
    con_kv("render_max_us", (int32_t)audio_max_us);
    con_kv("cpu_pct", (int32_t)(song.cpu_q8 * 100u / 256u));
    flp_hog_ticks = 0;
}

/* flp idle HOG_US HALVES: the control run: the same load with no flash work */
static void flp_quiet(uint32_t hog_us, uint32_t halves)
{
    flp_mark(hog_us);
    while (halves--)
        flp_wait_half();
    flp_report();
}

/* one erase or page program in slices, a render between them */
static int flp_op(uint32_t off, uint32_t op, const uint8_t *src, uint32_t *slices, uint32_t *off_max, uint32_t *sus_max)
{
    static flp_slice_t s;
    int rc;
    do {
        uint32_t f = irq_save();
        rc = FL_FAR(flp_slice_ram)(off, op, src, &s);
        irq_restore(f);
        if (rc < 0) {
            flp_hog_ticks = 0;
            con_kv("slice_rc", rc);
            return rc;
        }
        op = FLP_RESUME;
        (*slices)++;
        if (s.off_us > *off_max)
            *off_max = s.off_us;
        if (s.sus_us > *sus_max)
            *sus_max = s.sus_us;
        if (!rc)
            flp_wait_half();
    } while (!rc);
    return 0;
}

/* flp live HOG_US CYCLES: erase and program FLP_PAGES pages in slices, the audio running */
static void flp_live(uint32_t hog_us, uint32_t cycles)
{
    uint32_t c, off_max = 0, sus_max = 0, slices = 0, prog_max = 0, er_max = 0, prog_sus = 0, took, p;
    if (!flp_slot_free())
        return;
    flp_mark(hog_us);
    for (c = 0; c < cycles; c++) {
        uint32_t t0;
        flp_wait_half();
        t0 = fm1_ticks();
        if (flp_op(FLP_OFF, FLP_ERASE, 0, &slices, &off_max, &sus_max) < 0)
            return;
        took = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
        if (took > er_max)
            er_max = took;
        flp_fill(c);
        for (p = 0; p < FLP_PAGES; p++) {
            uint32_t page_slices = 0;
            flp_wait_half();
            if (flp_op(FLP_OFF + p * 256u, FLP_PROG, flp_buf, &page_slices, &prog_max, &sus_max) < 0)
                return;
            if (page_slices > 1u)
                prog_sus += page_slices - 1u;
        }
    }
    flp_report();
    con_kv("cycles", (int32_t)cycles);
    con_kv("slices", (int32_t)slices);
    con_kv("erase_wall_max_us", (int32_t)er_max);
    con_kv("slice_irq_off_max_us", (int32_t)off_max);
    con_kv("suspend_max_us", (int32_t)sus_max);
    con_kv("page_irq_off_max_us", (int32_t)prog_max);
    con_kv("prog_suspends", (int32_t)prog_sus);
    for (p = 0, c = 0; p < FLP_PAGES; p++) {
        uint32_t i;
        uint8_t v = 0;
        if (st_read(FLP_OFF + p * 256u, flp_buf, 256u))
            c++;
        v = flp_buf[0];
        for (i = 0; i < 256u; i++)
            c += flp_buf[i] != (uint8_t)(v + i);
    }
    con_kv("verify_errors", (int32_t)c);
}

static void con_flp(const char *p)
{
    int ok, ok2;
    if (con_word(&p, "id"))
        flp_id();
    else if (con_word(&p, "base"))
        flp_base();
    else if (con_word(&p, "play"))
        transport_req = 1;
    else if (con_word(&p, "sus")) {
        uint32_t run = con_num(&p, &ok);
        flp_sus(ok ? run : 1000u);
    } else if (con_word(&p, "idle")) {
        uint32_t hog = con_num(&p, &ok), n = con_num(&p, &ok2);
        flp_quiet(ok ? hog : 0u, ok2 ? n : 2000u);
    } else if (con_word(&p, "live")) {
        uint32_t hog = con_num(&p, &ok), n = con_num(&p, &ok2);
        flp_live(ok ? hog : 0u, ok2 ? n : 10u);
    } else
        con_puts("flp id | base | play | sus RUN_US | idle HOG_US HALVES | live HOG_US CYCLES\r\n");
}
