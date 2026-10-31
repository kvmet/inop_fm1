/* SPDX-License-Identifier: GPL-3.0-only */
/* FELUCCA_FLPROBE: the RAM side of firmware/src/flprobe.c: an erase suspended (0x75) and resumed (0x7A) */
#pragma once
#define FLP_OTHER 0xE5000u                              /* read while suspended: the autosave's sector, never written here */
#define FLP_CAP_US 300000u                              /* give up slicing: the WDT and the IRQs are waiting */
#define FLP_TICKS_US 24u                                /* T4_CNT */

typedef struct {
    uint32_t sr_idle, sr_sus, other0, other1, total_us, sus_max_us, slices, gave_up;
} flp_off_t;
typedef struct { uint32_t sus_us, off_us; } flp_slice_t;

RAMINL uint32_t flp_sr(void) { return fl_rdsr(0x05) | (uint32_t)fl_rdsr(0x35) << 8 | (uint32_t)fl_rdsr(0x15) << 16; }
RAMINL uint32_t flp_rd32(uint32_t a)
{
    uint32_t v;
    fl_cs(0); fl_tx(0x03); fl_addr(a);
    v = fl_rx(); v |= (uint32_t)fl_rx() << 8; v |= (uint32_t)fl_rx() << 16; v |= (uint32_t)fl_rx() << 24;
    fl_cs(1);
    return v;
}
RAMINL void flp_us(uint32_t us) { uint32_t t = T4_CNT; while (T4_CNT - t < us * FLP_TICKS_US) ; }
/* twice: a status read right after a resume must not end the erase while the part is still busy, XIP would come back */
RAMINL int flp_idle(void) { return !(fl_rdsr(0x05) & 1u) && !(fl_rdsr(0x05) & 1u); }
RAMINL uint32_t flp_suspend(void)
{
    uint32_t t = T4_CNT, n = 2000u * FLP_TICKS_US;
    fl_cs(0); fl_tx(0x75); fl_cs(1);
    while ((fl_rdsr(0x05) & 1u) && T4_CNT - t < n) ;
    return (T4_CNT - t) / FLP_TICKS_US;
}

/* IRQs off, audio silenced: erase, suspended after each run_us, until it ends or FLP_CAP_US */
static RAMFN int flp_sus_ram(uint32_t off, uint32_t run_us, flp_off_t *r)
{
    fl_saved_t s;
    uint32_t t0, t, took, dummy;
    if (!FL_GUARD(off, 0x1000u))
        return -1;
    if (fl_enter(&s))
        return -2;
    r->sr_idle = flp_sr();
    r->other0 = flp_rd32(FLP_OTHER);
    fl_wren();
    if (!(fl_rdsr(0x05) & 2u)) {
        fl_exit(&s);
        return -3;
    }
    fl_cs(0); fl_tx(0x20); fl_addr(off); fl_cs(1);
    t0 = T4_CNT;
    for (;;) {
        t = T4_CNT;
        while (T4_CNT - t < run_us * FLP_TICKS_US)
            if (flp_idle())
                goto done;
        if (flp_idle())
            goto done;
        took = flp_suspend();
        if (took > r->sus_max_us)
            r->sus_max_us = took;
        if (!r->slices) {
            r->sr_sus = flp_sr();
            r->other1 = flp_rd32(FLP_OTHER);
        }
        r->slices++;
        if ((T4_CNT - t0) / FLP_TICKS_US > FLP_CAP_US) {
            r->gave_up = 1;
            fl_cs(0); fl_tx(0x7A); fl_cs(1);
            flp_us(5);
            (void)fl_wait_ready(500000u, &dummy);
            break;
        }
        fl_cs(0); fl_tx(0x7A); fl_cs(1);
        flp_us(5);
    }
done:
    r->total_us = (T4_CNT - t0) / FLP_TICKS_US;
    fl_inval(off, 4096u);
    fl_exit(&s);
    return 0;
}

enum { FLP_RESUME, FLP_ERASE, FLP_PROG };
/* IRQs off: start or resume an erase or page program, run until a half buffer is free, suspend.
 * 1: the operation ended, 0: suspended */
static RAMFN int flp_slice_ram(uint32_t off, uint32_t op, const uint8_t *src, flp_slice_t *r)
{
    fl_saved_t s;
    uint32_t t0 = T4_CNT, i;
    if (!FL_GUARD(off, 0x1000u))
        return -1;
    if (fl_enter(&s))
        return -2;
    if (op != FLP_RESUME) {
        fl_wren();
        if (!(fl_rdsr(0x05) & 2u)) {
            fl_exit(&s);
            return -3;
        }
        fl_cs(0);
        fl_tx(op == FLP_ERASE ? 0x20 : 0x02);
        fl_addr(off);
        if (op == FLP_PROG)
            for (i = 0; i < 256u; i++)
                fl_tx(src[i]);
        fl_cs(1);
    } else {
        fl_cs(0); fl_tx(0x7A); fl_cs(1);
    }
    flp_us(5);
    r->sus_us = 0;
    while (!(FM1_ALNK_CON2 & FM1_AUDIO_HALF))
        if (flp_idle()) {
            fl_inval(off & ~0xFFFu, 4096u);
            fl_exit(&s);
            r->off_us = (T4_CNT - t0) / FLP_TICKS_US;
            return 1;
        }
    r->sus_us = flp_suspend();
    fl_exit(&s);
    r->off_us = (T4_CNT - t0) / FLP_TICKS_US;
    return 0;
}

