/* SPDX-License-Identifier: GPL-3.0-only */
/* The model in docs/index.md: packets and voices. */
#pragma once
#include <stdint.h>
#include "felucca_tables.h"

#define HALF_FRAMES 128u         /* I2S half buffer: 2.9 ms */
#define NTRK 6u
#define NVOICE 16u
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")

#define NSOUND 64u
#define NPATTERN 27u
#define NSTEP 64u
#define TRACK_LOCKS 270u
#define SOUND_NONE 0xFFu
#define STEP_INHERIT 0xFFu       /* a step's note, velocity or length that comes from its sound */
#define LEN_HOLD (NSTEP + 1u)    /* a length: the note sounds until the track's next note */

#ifndef AUDIO_RAM
#define AUDIO_RAM
#define AUDIO_FAR(fn) fn
#endif

/* flatten inlines only what the RAM function itself calls. What those call must inline too, or it is a call out of RAM's reach */
#define AUDIO_INLINE inline __attribute__((always_inline))

static AUDIO_INLINE int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }

enum {
    PV_NOTE, PV_VEL, PV_LEN, PV_PRIO,
    PV_AMP_A, PV_AMP_D, PV_AMP_S, PV_AMP_R,
    PV_FLT_TYPE, PV_FLT_CUT, PV_FLT_RES, PV_FLT_KEY,
    PV_MOD_A, PV_MOD_D, PV_MOD_S, PV_MOD_R,
    PV_MOD2_A, PV_MOD2_D, PV_MOD2_S, PV_MOD2_R,
    PV_SHAPER, PV_CRUSH, PV_RATE, PV_DRIVE,
    PV_LEVEL, PV_PAN, PV_STEREO,
    PV_SEND_DELAY, PV_SEND_REVERB, PV_SEND_DIST, PV_SEND_FOLLOW,
    PV_LFO1_SHAPE, PV_LFO1_PHASE, PV_LFO1_RATE, PV_LFO1_START,
    PV_LFO2_SHAPE, PV_LFO2_PHASE, PV_LFO2_RATE, PV_LFO2_START,
    PV_SLOT_SRC0, PV_SLOT_SRC1, PV_SLOT_SRC2, PV_SLOT_SRC3, PV_SLOT_SRC4, PV_SLOT_SRC5, PV_SLOT_SRC6, PV_SLOT_SRC7,
    PV_SLOT_DST0, PV_SLOT_DST1, PV_SLOT_DST2, PV_SLOT_DST3, PV_SLOT_DST4, PV_SLOT_DST5, PV_SLOT_DST6, PV_SLOT_DST7,
    PV_SLOT_AMT0, PV_SLOT_AMT1, PV_SLOT_AMT2, PV_SLOT_AMT3, PV_SLOT_AMT4, PV_SLOT_AMT5, PV_SLOT_AMT6, PV_SLOT_AMT7,
    PV_ENGINE,
    PV_E0, PV_E1, PV_E2, PV_E3, PV_E4, PV_E5, PV_E6, PV_E7,
    PV_COUNT
};
typedef struct { int8_t v[PV_COUNT]; } pkt_t;
enum { MOD_SRC_OFF, MOD_SRC_ENV, MOD_SRC_ENV2, MOD_SRC_LFO1, MOD_SRC_LFO2, MOD_SRC_VEL, MOD_SRC_RANDOM, MOD_SRC_FOLLOW, MOD_SRCS };
enum { MOD_DST_OFF, MOD_DST_PITCH, MOD_DST_CUT, MOD_DST_RES, MOD_DST_LEVEL, MOD_DST_PAN, MOD_DST_DRIVE, MOD_DST_SHAPER, MOD_DST_CRUSH, MOD_DST_RATE, MOD_DST_TIMBRE, MOD_DST_INDEX, MOD_DST_LFO1_RATE, MOD_DST_LFO2_RATE, MOD_DSTS };

/* project values: a trigger does not carry these. A page lists them as PV_COUNT + G_ */
enum { G_DELAY_TIME, G_DELAY_FB, G_DELAY_WIDTH, G_REVERB_SIZE, G_REVERB_DAMP, G_MST_COMP, G_MST_ATTACK, G_MST_RELEASE, G_MST_TILT, G_MST_WIDTH, G_MST_DRIVE, G_FOL_ATTACK, G_FOL_RELEASE, G_COUNT };
#define ID_GLOBAL(g) (PV_COUNT + (g))
enum { ENGINE_FM, ENGINE_PERC, ENGINE_SWARM, ENGINE_WAVE, ENGINES };
enum { PRIO_XXLOW, PRIO_XLOW, PRIO_LOW, PRIO_NORMAL, PRIO_HIGH, PRIO_XHIGH, PRIO_XXHIGH, PRIOS };
typedef pkt_t sound_t;           /* a pool sound is a packet with no step applied */
#define ENGINE_VALUES (PV_E7 - PV_E0 + 1)
typedef struct { uint8_t seen; int8_t v[ENGINES][ENGINE_VALUES]; } engine_memory_t;   /* seen: a bit per engine that v holds */
#define STEP_CHANCE_ALWAYS 100u
#define STEP_HITS_MAX 8u
#define SEQ_TIME_UNITS 48        /* in a step: the unit of a step's time, a track's swing and its human amount */
#define SEQ_TIME_MAX (SEQ_TIME_UNITS / 2)
#define LFO_SYNC_UNITS_PER_STEP 768u   /* 3 goes into it, so a triplet cycle is a whole number and stays on the grid */

typedef struct {
    uint8_t on, note, vel, len;  /* STEP_INHERIT, or this step's own. len: steps a note holds */
    uint8_t sound;               /* sound lock: a pool index, or SOUND_NONE */
    uint8_t chance;              /* percent */
    uint8_t hits;                /* a ratchet: triggers evenly spaced over the note's length */
    int8_t time;                 /* ahead of or behind the grid, in SEQ_TIME_UNITS */
} step_t;

typedef struct { uint8_t step, id; int8_t value; } lock_t;

typedef struct {
    uint8_t length, base;        /* base: a pool index */
    uint16_t nlock;
    uint8_t swing, human;        /* in SEQ_TIME_UNITS. swing: how late even-numbered steps fire. human: about the size of the wander */
    step_t steps[NSTEP];
    lock_t locks[TRACK_LOCKS];   /* sorted by step, then id */
} track_pat_t;

typedef struct { track_pat_t track[NTRK]; } pattern_t;
_Static_assert(sizeof(pattern_t) <= 8192u, "a pattern fits two flash sectors");
