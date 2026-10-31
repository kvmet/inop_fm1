/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Robustness against damaged or crafted stored data and editor requests (the 1.0.4 audit's items 5-10): the real
 * C paths of the editor build (editor_test.c is the base: hostsim, stubs, RAM flash).
 *   user sample slots   a SLICE scan bounded by the slot's size; a slot that fails its check leaves no zone behind
 *   projects            an engine number past the last refused (every format, and a RAM restore); a retained
 *                       older-format RAM slot bounded as one from flash
 *   user presets        a bank from flash or a backup: each record's pattern inside its fields
 *   editor              a malformed SMP_END / BACKUP_PUT does not stop the transport */
#define EDITOR_TEST_NO_MAIN 1
#include "editor_test.c"

static void zone_set(smp_zone_t *z, uint32_t off, uint32_t n)
{
    memset(z, 0, sizeof *z);
    z->off = off; z->n = n; z->ls = 0; z->le = n - 1u; z->rate = 1u << 16; z->idx = 0; z->lo = 0; z->hi = 127;
}

static int sample_slots(void)
{
    int bad = 0;
    uint32_t i, dl = SMP_USER_SIZE - SMP_USER_DATA;
    smp_user_hdr_t *h;
    reset(); memset(host_samples, 0, sizeof host_samples);
    /* 16 zones over overlapping data, each almost the whole slot (every zone inside the data) */
    h = (smp_user_hdr_t *)host_samples[0];
    h->magic = SMP_USER_MAGIC; h->version = 1; h->nz = 16; h->data_len = dl;
    for (i = 0; i < 16u; i++) zone_set(&h->zone[i], 32u * i, 2u * (dl - 32u * i));
    smp_user_scan(0);
#if FELUCCA_SLICE
    printf("robust: SLICE material of 16 overlapping zones: %u samples (the slot holds %u)\n", slc_usr[0].len,
           2u * dl);
    bad += check("16 overlapping zones: the SLICE scan is at most the slot's size", usr_nz[0] == 16u &&
                 slc_usr[0].len && slc_usr[0].len <= 2u * dl);
#endif
    /* zone 0 good, zone 1 malformed: nothing of the slot is left (usr_zone[1][0] was copied before the check) */
    h = (smp_user_hdr_t *)host_samples[1];
    h->magic = SMP_USER_MAGIC; h->version = 1; h->nz = 2; h->data_len = 4096;
    zone_set(&h->zone[0], 0, 4096);
    zone_set(&h->zone[1], 0, 4096); h->zone[1].idx = 99;
    smp_user_scan(1);
    bad += check("a slot with a malformed zone: unusable, no zone of it left behind",
                 !usr_nz[1] && !usr_zone[1][0].n && !usr_zone[1][1].n);
    h->zone[1].idx = 0;
    smp_user_scan(1);
    bad += check("the same slot made valid scans again", usr_nz[1] == 2u && usr_zone[1][0].n == 4096u);
    return bad;
}

static int projects(void)
{
    static project_t p, q;
    static project_store_t st;
    int bad = 0;
    uint32_t e0;
    reset();
    project_capture(&p);
    bad += check("(2.0) a captured project packs (SKF4) and imports", proj_pack2(&st, &p, PROJ_SKF) &&
                 proj_import(&q, &st, PROJ_STORE4));
    q = p; q.t[2].engine = (uint8_t)NENGINES;
    bad += check("(2.0) SKF4 with an engine past the last is not a project", proj_pack2(&st, &q, PROJ_SKF) &&
                 !proj_import(&q, &st, PROJ_STORE4));
    q = p; q.t[2].engine = ENGI_DIGITAL;
    bad += check("(2.0) .. nor one with a DIGITAL track (engine 1: the converter's)", proj_pack2(&st, &q, PROJ_SKF) &&
                 !proj_import(&q, &st, PROJ_STORE4));
    p.t[1].engine = 200; p.sum = proj_sum(&p);              /* a RAM project_t (valid magic, size and sum) */
    e0 = trk[1].eng_req;
    bad += check("restoring a project with an engine past the last is refused, nothing changes",
                 project_restore_runtime(&p) == 1 && trk[1].eng_req == e0);
    reset();                                                /* (2.0) a retained RAM slot of before (FUN5): not loaded */
    memset(&proj_slot[2], 0, sizeof proj_slot[2]);
    proj_slot[2].align = 0x46554E35u;
    {
        step_t s0 = trk[0].step[0];
        uint32_t e = trk[2].eng_req;
        project_load(2);
        bad += check("(2.0) a retained FUN5 RAM slot: not loaded, the music as it was",
                     !memcmp(&trk[0].step[0], &s0, sizeof s0) && trk[2].eng_req == e);
    }
    return bad;
}

static int user_presets(void)
{
    int bad = 0;
    up_bank_t *bk = &up_bank[0];
    up_rec_t *r;
    reset();
    memset(bk, 0, sizeof *bk);
    bk->magic = UP_BANK_MAGIC; bk->rsize = sizeof(up_rec_t); bk->nslot = UP_PER_BANK;
    r = &bk->r[0];                                          /* a note pattern with a note > 127 and stray flags */
    r->used = UP_USED; r->ver = UP_VER; r->engine = 0; r->np = 8; r->name[0] = 'A';   /* ((2.0) 4: 2 is the converter's) */
    r->note[3] = 200; r->flags[3] = 0x43;
    r->note[4] = 0; r->flags[4] = 0x03;                     /* a rest with flags */
    r = &bk->r[1];                                          /* a drum grid: accents where there is no hit */
    r->used = UP_USED; r->ver = UP_VER_GRID; r->engine = ENGI_DRUM; r->np = 8; r->name[0] = 'G';
    r->note[0] = 0x05; r->flags[0] = 0xFF;
    up_bank_check(0, sizeof *bk);
    bad += check("a bank from flash: a note pattern's note inside 0..127, its flags inside their fields",
                 up_used(0) && bk->r[0].note[3] == (200u & 127u) && bk->r[0].flags[3] == 3u && !bk->r[0].flags[4]);
    bad += check("a bank from flash: a drum grid's accents only on its hits", up_used(1) && bk->r[1].note[0] == 5u &&
                 bk->r[1].flags[0] == 5u);
    return bad;
}

static int stop_after_checks(void)
{
    int bad = 0;
    uint8_t a[16] = {0};
    reset();
    song.playing = 1; host_progress = 1;                    /* the transport runs and could be stopped */
    a[0] = 0; a[1] = 1; a[2] = 2;
    request(ED_SMP_END, a, 3);                              /* a header far too short */
    bad += check("a malformed SMP_END is refused without stopping the transport", host_wire[6] == 1 && song.playing);
    a[0] = 0; a[1] = 2;
    request(ED_BACKUP_PUT, a, 2);                           /* BEGIN without its arguments */
    bad += check("a malformed BACKUP_PUT is refused without stopping the transport", host_wire[7] == 1 && song.playing);
    a[0] = 1; a[1] = 2;
    request(ED_BACKUP_PUT, a, 9);                           /* data with no session */
    bad += check("BACKUP_PUT data without a session: refused, the transport runs", host_wire[7] == 5 && song.playing);
    song.playing = 0;
    return bad;
}

/* (Skiff phase 6) a light track's engine memory (engines.c eng_lite, DRUM's 1.8 KB): PHYS forced there (no path does it:
 * every one refuses) plays nothing and touches nothing past its area; DRUM after it plays from there; each engine
 * switch clears that area only (ASan watches). With four tracks the last plays PHYS */
static int peak_of(uint32_t blocks)
{
    int32_t buf[2 * CTL];
    uint32_t b, i;
    int peak = 0;
    for (b = 0; b < blocks; b++) {
        mix_block(buf, CTL);
        for (i = 0; i < 2u * CTL; i++)
            peak = abs(buf[i]) > peak ? abs(buf[i]) : peak;
    }
    return peak;
}
static int light_engine_memory(void)
{
    int bad = 0, p;
    uint32_t k = NTRK - 1u, i;
    reset();
    song.master_q12 = 4096;                                  /* (reset leaves MASTER at 0) */
    for (i = 0; i < NTRK; i++)
        trk[i].p[P_LEVEL] = (int16_t)(i == k ? 127 : 0);    /* (the last track alone heard) */
    trk[k].eng_req = ENGI_PHYS;
    (void)peak_of(16u);                                      /* (the switch: a fade, then its area cleared) */
    trk_note_on(&trk[k], 60, 120);
    p = peak_of(400u);
    bad += check("PHYS forced onto the last track: silent when light (no state there), heard when not",
                 trk[k].engine == ENGI_PHYS && (k >= NHEAVY ? p == 0 : p > 0));
    trk_note_off(&trk[k], 60);
    (void)peak_of(400u);
    trk[k].eng_req = ENGI_DRUM;
    apply_preset_to(&trk[k], 0);
    (void)peak_of(16u);
    trk_note_on(&trk[k], 36, 120);
    bad += check("  then DRUM there: its kick from that area", trk[k].engine == ENGI_DRUM && peak_of(200u) > 0);
    return bad;
}

int main(void)
{
    int bad = sample_slots() + projects() + user_presets() + stop_after_checks() + light_engine_memory();
    printf("%s\n", bad ? "ROBUSTNESS TEST FAILED" : "robustness test passed");
    return bad != 0;
}
