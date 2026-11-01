/* SPDX-License-Identifier: GPL-3.0-only */
/* Which voice a trigger takes: docs/index.md, Priority. No hardware here: tests/inop_test.c runs this on the host. */
#define NEARLY_SILENT 512        /* of 32768: about -36 dB */

typedef struct {
    uint8_t stealable, prio;
    int32_t loudness;
    uint32_t age;                /* a larger age is newer */
    uint8_t rising;              /* in its attack: quiet because it has barely started, not because it has ended */
} steal_t;

/* the index to steal for a trigger of priority prio, or -1 when none may be */
static int32_t voice_victim(const steal_t *v, uint32_t n, uint32_t prio)
{
    int32_t best = -1;
    uint32_t i;
    for (i = 0; i < n; i++)
        if (v[i].stealable && v[i].prio <= prio && !v[i].rising && v[i].loudness < NEARLY_SILENT && (best < 0 || v[i].loudness < v[best].loudness))
            best = (int32_t)i;
    if (best >= 0)
        return best;
    for (i = 0; i < n; i++)
        if (v[i].stealable && v[i].prio < prio && (best < 0 || v[i].prio < v[best].prio ||
                                                   (v[i].prio == v[best].prio && v[i].loudness < v[best].loudness)))
            best = (int32_t)i;
    if (best >= 0)
        return best;
    for (i = 0; i < n; i++)
        if (v[i].stealable && v[i].prio == prio && (best < 0 || v[i].age < v[best].age))
            best = (int32_t)i;
    return best;
}
