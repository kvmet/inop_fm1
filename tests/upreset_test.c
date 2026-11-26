/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Host test of the user preset record (firmware/src/upreset.c, -DUP_HOST part):
 * UP_PUT parsing, a bank round trip through storage.c on a simulated NOR,
 * bank / record version checks, map-by-count, pattern <-> steps, PHYS MODEL DRUM records -> the DRUM engine,
 * drum grid records (version 3: UP_PUT's kind 1 and its high bits, a round trip). Versions 4..6 only,
 * no DIGITAL record. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define __attribute__(x)
#define UP_HOST 1
#include "../firmware/src/core.h"

static uint8_t nor[0x100000];
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off) { memset(nor + off, 0xFF, 4096); return 0; }
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    return 0;
}
#include "../firmware/src/storage.c"
#include "../firmware/src/upreset.c"

static int check(const char *what, int ok)
{
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static uint32_t put_frame(uint8_t *a, uint32_t slot, uint32_t eng, const char *name, int32_t base)
{
    uint32_t n = 0, i;
    a[n++] = (uint8_t)slot;
    a[n++] = (uint8_t)eng;
    for (i = 0; name[i]; i++)
        a[n++] = (uint8_t)name[i];
    a[n++] = 0;
    for (i = 0; i < P_COUNT; i++) {
        uint32_t u = (uint32_t)(base + (int32_t)(i % 100u) + 8192);   /* ((1.3) P_COUNT past 127: values in range) */
        a[n++] = u & 127u;
        a[n++] = (u >> 7) & 127u;
    }
    for (i = 0; i < 16u; i++) {
        a[n++] = (uint8_t)(i % 3u ? 40u + i : 0u);  /* rests on 0, 3, 6 .. */
        a[n++] = (uint8_t)(i == 3u ? 4u : i % 3u ? 1u : 2u);   /* step 3: tie; slide on a rest drops */
    }
    return n;
}

int main(void)
{
    uint8_t a[640];
    up_rec_t r, got;
    uint32_t n, slot = 99, i;
    int bad = 0, len, ok;
    int16_t v[P_COUNT], def[P_COUNT];
    memset(nor, 0xFF, sizeof nor);

    n = put_frame(a, 5, 2, "Bass One", -40);
    bad += check("UP_PUT frame < 640 bytes", 5u + n + 1u < 640u);
    bad += check("UP_PUT parses", up_parse(a, n, &r, &slot) == 0 && slot == 5u && r.engine == 2u &&
                                      up_valid(&r) && !memcmp(r.name, "Bass One", 8) && !r.name[8]);
    ok = 1;
    for (i = 0; i < P_COUNT; i++)
        ok &= up_value(&r, i) == (int16_t)(-40 + (int32_t)(i % 100u));
    bad += check("UP_PUT values (negative v14 too)", ok);
    bad += check("pattern: rest drops flags, tie has no note",
                 r.note[0] == 0 && r.flags[0] == 0 && r.note[1] == 41 && r.flags[1] == 1 && r.note[3] == 0 &&
                     r.flags[3] == 4);
    bad += check("UP_PUT short frame -> args", up_parse(a, n - 1u, &r, &slot) == 1);
    {
        up_rec_t keep = r;
        ok = 1;
        for (i = 0; i < n; i++)
            ok &= up_parse(a, i, &r, &slot) == 1 && !memcmp(&r, &keep, sizeof r);
        bad += check("every short prefix rejected without record changes", ok);
        a[n] = 0;
        bad += check("short optional tail rejected", up_parse(a, n + 1u, &r, &slot) == 1 &&
                                                       !memcmp(&r, &keep, sizeof r));
    }
    n = put_frame(a, 32, 0, "X", 0);
    bad += check("UP_PUT slot 32 -> args", up_parse(a, n, &r, &slot) == 1);
    n = put_frame(a, 0, NENGINES, "X", 0);
    bad += check("UP_PUT bad engine -> args", up_parse(a, n, &r, &slot) == 1);
    n = put_frame(a, 3, ENGI_DIGITAL, "OLD FM", 0);   /* (DIGITAL, retired) */
    {   /* no DIGITAL record: UP_PUT refuses one; a bank's (and versions 1..3) read as empty */
        up_rec_t keep = r;
        bad += check("(2.0) UP_PUT engine 1 (DIGITAL) -> args, the record unchanged",
                     up_parse(a, n, &r, &slot) == 1 && !memcmp(&r, &keep, sizeof r));
        *up_rec(30) = r;
        up_rec(30)->engine = ENGI_DIGITAL;
        ok = !up_used(30);
        up_rec(30)->engine = 2;
        ok &= up_used(30);
        up_rec(30)->ver = 3;
        ok &= !up_used(30);
        up_rec(30)->ver = 1;
        ok &= !up_used(30);
        memset(up_rec(30), 0, sizeof r);
        ok &= !up_used(30);
        bad += check("a DIGITAL record, versions 1..3: not used", ok);
    }
    n = put_frame(a, 0, 0, "", 0);
    bad += check("UP_PUT empty name -> args", up_parse(a, n, &r, &slot) == 1);
    n = put_frame(a, 0, 0, "THIRTEEN CHRS", 0);
    bad += check("UP_PUT 13-char name -> args", up_parse(a, n, &r, &slot) == 1);
    n = put_frame(a, 0, 0, "TWELVE CHARS", 0);
    bad += check("UP_PUT 12-char name ok", up_parse(a, n, &r, &slot) == 0 && !memcmp(r.name, "TWELVE CHARS", 12));
    {
        char nm[13];
        *up_rec(31) = r;
        memcpy(up_rec(31)->name, "Low case", 9);
        up_name(31, nm);
        bad += check("name shown upper case", !strcmp(nm, "LOW CASE"));
    }

    /* bank round trip through storage.c */
    n = put_frame(a, 17, 3, "Keys", 7);
    up_parse(a, n, &r, &slot);
    up_bank[1].magic = UP_BANK_MAGIC;
    up_bank[1].rsize = sizeof(up_rec_t);
    up_bank[1].nslot = UP_PER_BANK;
    *up_rec(17) = r;
    bad += check("bank fits one object", sizeof(up_bank_t) <= ST_PAYLOAD_MAX);
    bad += check("bank save", st_save(OBJ_UPRESET0 + 1, &up_bank[1], sizeof up_bank[1]) == 0);
    memset(up_bank, 0, sizeof up_bank);
    len = st_load(OBJ_UPRESET0 + 1, &up_bank[1], sizeof up_bank[1]);
    up_bank_check(1, len);
    got = *up_rec(17);
    bad += check("bank load: the record is back", up_used(17) && !memcmp(&got, &r, sizeof r));
    bad += check("other slots empty", !up_used(16) && !up_used(18) && !up_used(0));
    len = st_load(OBJ_UPRESET0, &up_bank[0], sizeof up_bank[0]);
    up_bank_check(0, len);
    bad += check("bank 0 never written -> empty", len < 0 && !up_used(0) && up_bank[0].magic == 0);
    bad += check("banks in 0xDC000..0xDFFFF", st_sector(OBJ_UPRESET0, 0) == 0xDC000u &&
                                                   st_sector(OBJ_UPRESET0 + 1, 1) == 0xDF000u &&
                                                   st_sector(OBJ_PROJECT0 + 3, 1) + 4096u <= 0xA0000u);
    up_bank[1].rsize = 190;                                 /* another record layout */
    up_bank_check(1, (int)sizeof up_bank[1]);
    bad += check("bank with another record size -> empty", !up_used(17));
    up_bank[1].magic = UP_BANK_MAGIC;
    up_bank[1].rsize = sizeof(up_rec_t);
    up_bank[1].nslot = UP_PER_BANK;
    *up_rec(17) = r;
    up_rec(17)->ver = UP_VER_SID + 1u;
    bad += check("record with another version -> empty", !up_used(17));
    up_rec(17)->ver = UP_VER_GRID;
    bad += check("a version 3 record (a drum grid) is used", up_used(17));

    /* a drum grid by UP_PUT: kind 1, then bit 7 of each step's hits (bit 0) and accents (bit 1) */
    {
        uint8_t g[640];
        uint32_t k, m = put_frame(g, 9, ENGI_DRUM, "My Beat", 0), at = m - 32u;
        for (k = 0; k < 16u; k++) {
            g[at + 2u * k] = (uint8_t)(k % 4u ? 0x08u : 0x09u);    /* HAT CL, the kick on the beats */
            g[at + 2u * k + 1u] = (uint8_t)(k == 0u ? 0x7Fu : 0u);  /* step 1: every lane's accent (one too many) */
        }
        g[m++] = 1;
        for (k = 0; k < 16u; k++)
            g[m++] = (uint8_t)(k == 2u ? 3u : k == 5u ? 2u : 0u);  /* step 3: BELL hit + accent; 6: an accent alone */
        ok = up_parse(g, m, &got, &slot) == 0 && slot == 9u && got.ver == UP_VER_GRID && up_valid(&got) &&
             got.note[0] == 0x09u && got.flags[0] == 0x09u && got.note[1] == 0x08u && !got.flags[1] &&
             got.note[2] == 0x88u && got.flags[2] == 0x80u && got.note[5] == 0x08u && !got.flags[5] &&
             got.engine == ENGI_DRUM;
        bad += check("UP_PUT kind 1: a grid record, accents only on hits", ok);
        g[m - 17u] = 0;                                     /* kind 0: the notes and flags as before */
        ok = up_parse(g, m, &got, &slot) == 0 && got.ver == UP_VER && got.note[2] == 0x08u && got.flags[0] == 4u;   /* (flags 0x7F: a tie) */
        bad += check("UP_PUT kind 0: a note pattern (version 2)", ok);
        g[m - 17u] = 1;
        {
            up_rec_t keep = got;
            ok = up_parse(g, m - 1u, &got, &slot) == 1 && !memcmp(&got, &keep, sizeof got);
            bad += check("UP_PUT short grid rejected, record unchanged", ok);
            g[m - 17u] = 2;
            bad += check("UP_PUT unknown kind rejected", up_parse(g, m, &got, &slot) == 1 &&
                                                         !memcmp(&got, &keep, sizeof got));
            g[m - 17u] = 1;
            g[m] = 0;
            bad += check("UP_PUT trailing byte rejected", up_parse(g, m + 1u, &got, &slot) == 1 &&
                                                         !memcmp(&got, &keep, sizeof got));
        }
        up_parse(g, m, &got, &slot);
        *up_rec(20) = got;
        st_save(OBJ_UPRESET0 + 1, &up_bank[1], sizeof up_bank[1]);
        memset(up_bank, 0, sizeof up_bank);
        up_bank_check(1, st_load(OBJ_UPRESET0 + 1, &up_bank[1], sizeof up_bank[1]));
        bad += check("a grid record: bank round trip", up_used(20) && !memcmp(up_rec(20), &got, sizeof got));
    }


    /* map by count: a record from a build with 2 parameters fewer */
    for (i = 0; i < P_COUNT; i++)
        def[i] = (int16_t)(1000 + i);
    r.np = P_COUNT - 2u;
    for (i = 0; i < P_COUNT; i++)
        up_set_value(&r, i, (int16_t)(i % 100u));
    up_params(&r, v, def);
    ok = 1;
    for (i = 0; i < P_E0; i++)
        ok &= v[i] == (i < P_E0 - 2u ? (int16_t)(i % 100u) : def[i]);
    for (i = 0; i < 8u; i++)
        ok &= v[P_E0 + i] == (int16_t)((P_E0 - 2u + i) % 100u);
    bad += check("np < P_COUNT: mapped by count", ok);
    /* records of versions 4 (packed bytes) and 5 (a drum grid) saved before the DRUM lane levels (1.0.x: P_COUNT 91,
     * P_E0 83), before the chord keys (89, P_E0 81) and before the FM operator ENVs (69, P_E0 61): their engine
     * values land on today's E0..E7 (91..98), the lane levels (100 %) and the chord keys their defaults (OFF, CLOSE), the
     * operator parameters too for 69 */
    {
        static const uint8_t VERS[2] = {UP_VER, UP_VER_GRID}, NPS[3] = {91, 89, 69};
        uint32_t a, b;
        ok = 1;
        for (a = 0; a < 2u; a++)
            for (b = 0; b < 3u; b++) {
                uint32_t np = NPS[b];
                up_rec_t o;
                memset(&o, 0, sizeof o);
                o.used = UP_USED; o.ver = VERS[a]; o.engine = 2; o.np = (uint8_t)np;   /* ((2.0) PHASE: no DIGITAL) */
                memcpy(o.name, "OLD", 3);
                for (i = 0; i < np; i++)
                    up_set_value(&o, i, (int16_t)(i % 100u - 30));
                ok &= up_valid(&o);
                up_params(&o, v, def);
                for (i = 0; i < np - 8u; i++)
                    ok &= v[i] == (int16_t)(i % 100u - 30);
                for (i = np - 8u; i < P_E0; i++)
                    ok &= v[i] == def[i];
                for (i = 0; i < 8u; i++)
                    ok &= v[P_E0 + i] == (int16_t)((np - 8u + i) % 100u - 30);
            }
        bad += check("v4 / v5 records of 91, 89 and 69 parameters: E0..E7 at P_E0, lane levels and chord keys defaults", ok);
    }
    r.ver = UP_VER;
    r.np = P_COUNT;
    for (i = 0; i < P_COUNT; i++)
        up_set_value(&r, i, (int16_t)i);
    up_params(&r, v, def);
    ok = 1;
    for (i = 0; i < P_COUNT; i++)
        ok &= v[i] == (int16_t)i;
    bad += check("np == P_COUNT: as stored", ok);
    {   /* (2.0, Tier B 7) version 6: the sound by stored id over the parameters' and the pattern's bytes, no pattern; a
         * newer record's ids passed over, an older one's missing ones at their defaults */
        static int16_t s[P_COUNT];
        up_rec_t six;
        uint32_t k;
        for (i = 0; i < P_COUNT; i++)
            s[i] = (int16_t)((int32_t)(i * 7u % 192u) - 64);
        memset(&six, 0, sizeof six);
        six.used = UP_USED; six.engine = 2; memcpy(six.name, "SIX", 3);
        up_set_sound(&six, s);
        up_params(&six, v, def);
        ok = six.ver == UP_VER_SID && six.np == SID_COUNT && up_valid(&six) && !up_pat(&six) && !up_grid(&six) &&
             !memcmp(v, s, sizeof s) && UP_SIDS == 176u;
        for (k = SID_COUNT; k < UP_SIDS; k++)
            ok &= !six.sid[k];
        bad += check("v6: by stored id, round trip, the rest 0", ok);
        got = six;
        got.np = (uint8_t)(SID_COUNT + 5u);                 /* a newer firmware's: five more ids */
        memset(got.sid + SID_COUNT, 64 + 9, 5);
        up_params(&got, v, def);
        ok = up_valid(&got) && !memcmp(v, s, sizeof s);
        got.np = 100;                                       /* an older one's: the first 100 */
        up_params(&got, v, def);
        ok &= up_valid(&got) && !memcmp(v, s, 100u * sizeof v[0]) && v[100] == def[100] &&
              v[P_COUNT - 1u] == def[P_COUNT - 1u];
        bad += check("v6: a newer record's ids passed over, an older one's defaults", ok);
        got = six; got.sid[3] = 192u;
        ok = !up_valid(&got);
        got = six; got.np = (uint8_t)(UP_SIDS + 1u);
        ok &= !up_valid(&got);
        bad += check("v6 refused: a byte past 191, more ids than its room", ok);
        got = six;                                          /* (ids into the old pattern's bytes; MODEL 4 where a */
        got.np = (uint8_t)UP_SIDS;                          /* record by count has PHYS's E0: neither touched) */
        for (k = SID_COUNT; k < UP_SIDS; k++)
            got.sid[k] = (uint8_t)(64u + k % 50u);
        got.sid[UP_SIDS - 8u] = 64u + 4u;
        got.engine = ENGI_PHYS;
        *up_rec(22) = got;
        st_save(OBJ_UPRESET0 + 1, &up_bank[1], sizeof up_bank[1]);
        memset(up_bank, 0, sizeof up_bank);
        up_bank_check(1, st_load(OBJ_UPRESET0 + 1, &up_bank[1], sizeof up_bank[1]));
        bad += check("v6: a bank round trip leaves its bytes as they are",
                     up_used(22) && !memcmp(up_rec(22), &got, sizeof got) && up_rec(22)->engine == ENGI_PHYS);
    }
    printf("%s\n", bad ? "USER PRESET TEST FAILED" : "user preset test passed");
    return bad != 0;
}
